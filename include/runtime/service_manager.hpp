#pragma once

#include "runtime/config_manager.hpp"
#include "runtime/event.hpp"
#include "runtime/logger.hpp"
#include "runtime/monitor.hpp"
#include "runtime/process_supervisor.hpp"
#include "runtime/resource_snapshot.hpp"
#include <functional>
#include <optional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime {

enum class ServiceState { created, starting, running, stopping, stopped, failed, recovering };
const char* state_name(ServiceState state);

struct ServiceStatus {
    std::string service_name;
    ServiceState state = ServiceState::created;
    std::optional<Clock::time_point> heartbeat_time;
    unsigned restart_count = 0;
    int pid = -1;
    std::optional<Clock::time_point> start_time;
    std::uint64_t generation = 0;
    std::uint64_t launched_generation = 0;
};

enum class ServiceChangeCause { lifecycle, failure, recovery_preparation, recovery_finalization, explicit_stop, dependency_stop };

struct ServiceStateChange {
    std::string service_name;
    ServiceState from;
    ServiceState to;
    Clock::time_point at;
    std::uint64_t generation = 0;
    bool recovery_exhausted = false; // Deprecated source compatibility; never a policy/result input.
    ServiceChangeCause cause = ServiceChangeCause::lifecycle;
    std::optional<RecoveryOperation> operation;
    std::optional<FailureType> failure_type;
};

class ServiceManager : public RecoveryExecutor {
public:
    using StateChangeSink = std::function<void(const ServiceStateChange&)>;
    using RestartRequestSink = std::function<void(const std::string&)>;

    // State change and restart callbacks run after releasing the registry lock.
    ServiceManager(ProcessSupervisor& processes, Monitor& monitor, Logger& logger,
                   StateChangeSink state_changes = {}, RestartRequestSink restart_requests = {});
    void add(ServiceConfig config);
    void registerService(ServiceConfig config);
    void startService(const std::string& name, Clock::time_point now = Clock::now());
    void stopService(const std::string& name, Clock::time_point now = Clock::now());
    // Returns false if the service is unknown or no recovery handler is connected.
    bool restartService(const std::string& name) const;
    std::optional<ServiceStatus> queryServiceStatus(const std::string& name) const;
    std::optional<ServiceConfig> queryServiceDefinition(const std::string& name) const;
    std::vector<ServiceStatus> listServices() const;
    // Disengaged means registry busy. Engaged empty means no eligible children.
    // No proc I/O or lifecycle changes. instance_generation = launched_generation.
    std::optional<std::vector<ProcessIdentity>> trySnapshotProcessIdentities() const;
    // One try-lock and O(N) lookups; returns only identities still matching.
    // Collector must reject the entire scan if any captured identity is missing.
    std::optional<std::vector<ProcessIdentity>> tryValidateProcessIdentities(
        const std::vector<ProcessIdentity>& captured) const;
    // Validates the complete static graph; throws invalid_argument on bad edges/cycles.
    // Register all definitions first. Prerequisites precede their dependents.
    std::vector<std::string> startup_order() const;
    void handle(const Event& event);
    void tick(Clock::time_point now);
    std::optional<ServiceStatus> query(const std::string& name) const;
    std::vector<ServiceStatus> all_statuses() const;
    void stop_all(Clock::time_point now);
    // Latest outstanding graceful-stop deadline; no independent fixed budget.
    std::optional<Clock::time_point> shutdown_deadline() const;
    // T3 supplies RM's disposition for spontaneous exit. Default standalone
    // behavior is clean -> STOPPED, abnormal -> FAILED, without automatic retry.
    void handleProcessExit(const Event& event, ExitDisposition disposition);
    std::optional<RecoveryExecutionSnapshot> snapshot(const std::string& name) const override;
    RecoveryExecutionReply prepareRecovery(const RecoveryOperation&, unsigned, Clock::time_point) override;
    bool recoveryReady(const RecoveryOperation&, Clock::time_point, const std::function<bool()>&) override;
    RecoveryExecutionReply launchRecoveryAttempt(const RecoveryOperation&, std::uint64_t,
        Clock::duration, Clock::time_point) override;
    RecoveryExecutionReply finishRecoveryFailure(const RecoveryOperation&, std::uint64_t,
        RecoveryTerminalReason, Clock::time_point) override;
    void releaseRecovery(const std::string&, const RecoveryContext&) override;
    void projectRestartCount(const std::string&, unsigned) override;

private:
    struct Service {
        ServiceConfig config;
        ServiceStatus status;
        Clock::time_point started{};
        std::optional<RecoveryOperation> recovery;
        ServiceChangeCause cause = ServiceChangeCause::lifecycle;
        std::optional<FailureType> failure_type;
        std::optional<Clock::time_point> termination_deadline;
        bool termination_requested = false; // Includes force-signalled, unreaped children.
    };
    void start(Service& service, Clock::time_point now, std::optional<std::chrono::seconds> startup_cap = {});
    void stop(Service& service, Clock::time_point now);
    void fail(Service& service, Clock::time_point now, const std::string& reason,
              FailureType type = FailureType::startup_failure);
    void process_exit(Service& service, const Event& event, std::optional<ExitDisposition> disposition = {});
    std::vector<std::string> dependency_order() const; // Registry lock held.
    bool dependencies_running(const Service& service) const;
    void start_dependencies(const std::string& name, Clock::time_point now,
                            std::optional<Clock::time_point> deadline = {}, const std::function<bool()>& gate = {});
    void stop_dependents(const std::string& name, Clock::time_point now);
    void transition(Service& service, ServiceState next, Clock::time_point at,
                    bool recovery_exhausted = false);
    void dispatch_changes(std::vector<ServiceStateChange> changes) const;
    ProcessSupervisor& processes_;
    Monitor& monitor_;
    Logger& logger_;
    StateChangeSink state_changes_;
    RestartRequestSink restart_requests_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Service> services_;
    std::vector<ServiceStateChange> pending_changes_;
};

} // namespace runtime
