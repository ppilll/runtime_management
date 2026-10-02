#pragma once

#include "runtime/config_manager.hpp"
#include "runtime/event.hpp"
#include "runtime/logger.hpp"
#include "runtime/monitor.hpp"
#include "runtime/process_supervisor.hpp"
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
};

struct ServiceStateChange {
    std::string service_name;
    ServiceState from;
    ServiceState to;
    Clock::time_point at;
};

class ServiceManager {
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
    // Validates the complete static graph; throws invalid_argument on bad edges/cycles.
    // Register all definitions first. Prerequisites precede their dependents.
    std::vector<std::string> startup_order() const;
    void handle(const Event& event);
    void tick(Clock::time_point now);
    std::optional<ServiceStatus> query(const std::string& name) const;
    std::vector<ServiceStatus> all_statuses() const;
    void stop_all(Clock::time_point now);

private:
    struct Service {
        ServiceConfig config;
        ServiceStatus status;
        Clock::time_point started{};
        std::optional<Clock::time_point> restart_at;
        std::optional<Clock::time_point> termination_deadline;
    };
    void start(Service& service, Clock::time_point now);
    void stop(Service& service, Clock::time_point now);
    void fail(Service& service, Clock::time_point now, const std::string& reason);
    void process_exit(Service& service, const Event& event);
    std::vector<std::string> dependency_order() const; // Registry lock held.
    bool dependencies_running(const Service& service) const;
    void start_dependencies(const std::string& name, Clock::time_point now);
    void stop_dependents(const std::string& name, Clock::time_point now);
    void transition(Service& service, ServiceState next, Clock::time_point at);
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
