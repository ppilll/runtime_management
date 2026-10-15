#pragma once

#include "runtime/device_state_manager.hpp"
#include "runtime/event.hpp"
#include "runtime/logger.hpp"
#include "runtime/monitor.hpp"
#include "runtime/process_supervisor.hpp"
#include "runtime/service_manager.hpp"
#include "runtime/service_aggregation.hpp"
#include "runtime/recovery_manager.hpp"
#include "runtime/resource_collector.hpp"
#include <atomic>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace runtime {

enum class ResourceInputMode { native, external };

// Worker-owned deadline: missed ticks never cause catch-up collection.
class ResourceSamplingSchedule {
public:
    explicit ResourceSamplingSchedule(std::chrono::seconds interval);
    bool due(Clock::time_point now) const;
    void completed(Clock::time_point now);
    bool tryQueueTick();
    void consumeTick();
private:
    std::chrono::seconds interval_;
    std::optional<Clock::time_point> next_due_;
    std::atomic<bool> tick_pending_{false};
};

struct ResourceSnapshotView {
    ResourceSnapshotBundle snapshot;
    std::optional<Clock::duration> cpu_age;
    std::optional<Clock::duration> memory_age;
    bool cpu_stale = false;
    bool memory_stale = false;
};

// Writer-local normalized admission core. Production obtains the snapshot from
// SM; unit fixtures inject captured peers and fake time through the same gate.
// This is not a Runtime public ingress and never owns process policy or I/O.
class ComponentAdmission {
public:
    using Lookup = std::function<std::optional<ServiceStatus>(const std::string&)>;
    using FactSink = std::function<void(const ComponentHealthFact&)>;
    ComponentAdmission(ComponentPolicyEntry entry, std::string runtime_epoch, Lookup lookup, FactSink facts);
    ComponentReply apply(const ComponentIngress& ingress, Clock::time_point now);
    void checkpoint(Clock::time_point now);
    void stop(); // Revoke without publishing after shutdown gate.
    ComponentHealthSnapshot snapshot(Clock::time_point now) const;
    bool producer(const std::string& name) const;
private:
    friend class ComponentHealthTestAccess;
    bool current_instance(const std::shared_ptr<ComponentPeer>& peer) const;
    void observe_lifecycle(Clock::time_point now);
    void unavailable(ComponentReason reason, Clock::time_point now);
    void commit(ComponentHealthFact fact, bool publish = true);
    ComponentReply reply(const ComponentIngress& ingress, unsigned error = 0) const;
    const ComponentPolicyEntry entry_;
    const std::string runtime_epoch_;
    Lookup lookup_;
    FactSink facts_;
    ComponentIdentity identity_;
    std::shared_ptr<ComponentPeer> peer_;
    ComponentHealthFact committed_;
    std::optional<ComponentSample> last_owner_;
    std::optional<ComponentReport> last_report_;
    std::uint64_t next_session_ = 1;
    bool stopped_ = false;
};

class ComponentTransportAdapter; // T3-only friend: capture SO_PEERCRED, inject private sink.
class ComponentHealthTestAccess;

class RuntimeManager {
public:
    // Observer runs on the runtime writer after state commit; it must not throw.
    explicit RuntimeManager(const std::string& config_path, AggregationOptions aggregation = {},
                            DeviceStateManager::StateChangeSink device_changes = {},
                            ResourceThresholds resources = {});
    explicit RuntimeManager(RuntimeConfig config, AggregationOptions aggregation = {},
                            DeviceStateManager::StateChangeSink device_changes = {},
                            ResourceInputMode input_mode = ResourceInputMode::native,
                            ResourceCollector::Reader reader = {}, ResourceCollector::Now now = {});
    ~RuntimeManager();
    void post(Event event);
    // Explicit non-recovery device triggers use the same FIFO loop.
    void post(DeviceStateEvent event);
    // Public RuntimeEvent ingress accepts device-scoped resource facts only.
    void post(RuntimeEvent event);
    // Typed results always pass RM's active identity/state/deadline gate.
    void post(RecoveryResult result);
    void run();
    std::optional<ServiceStatus> query(const std::string& name) const;
    DeviceStateSnapshot queryDeviceState() const;
    std::optional<ResourceSnapshotView> queryResourceSnapshot(Clock::time_point now = Clock::now()) const;
    std::vector<ComponentHealthSnapshot> queryComponentHealth(Clock::time_point now = Clock::now()) const;
    void reportResourceUsage(double cpu_percent, double memory_percent, Clock::time_point at = Clock::now());

private:
    friend class ComponentTransportAdapter;
    friend class ComponentHealthTestAccess;
    using ComponentSink = std::function<bool(ComponentIngress)>;
    ComponentSink component_sink(); // Only trusted transport construction/test access.
    bool component_producer(const std::string& name) const;
    void drain_component_ingress(unsigned batch, Clock::time_point now);
    void component_checkpoint(Clock::time_point now);
    void stop_component_ingress();
    struct ComponentMailbox {
        std::mutex mutex;
        std::deque<ComponentIngress> operations;
        std::size_t outstanding = 0; // Queued plus writer batch, at most 32.
        EventQueue* wake_queue = nullptr;
        bool wake_pending = false;
        bool stopped = false;
    };
    RuntimeManager(RuntimeConfig config, AggregationOptions aggregation,
                   DeviceStateManager::StateChangeSink device_changes, ResourceInputMode input_mode,
                   ResourceCollector::Reader reader, ResourceCollector::Now now, ResourceThresholds thresholds);
    void enqueue_resource_fact(RuntimeEvent event);
    void stop_resource_sampling();
    bool resource_sampling_active() const;
    void sample_resources();
    struct ResourceLogState {
        bool failing = false;
        Clock::time_point next_log{};
    };
    void log_resource_failures(const ResourceSnapshotBundle& snapshot);
    void service_state_changed(const ServiceStateChange& change);
    void drain_work();
    void apply_change(const ServiceStateChange& change);
    void handle_event(const Event& event);
    void reap_children();
    void cancel_closure(const std::string& name, RecoveryTerminalReason reason);
    bool captured_recovery(const ServiceStateChange& change) const;
    EventQueue queue_;
    EventQueue monitor_queue_;
    Logger logger_;
    PosixProcessSupervisor processes_;
    const ResourceInputMode resource_mode_;
    const std::chrono::seconds resource_interval_;
    ResourceCollector::Now resource_now_;
    std::unique_ptr<ResourceCollector> collector_;
    ResourceSamplingSchedule resource_schedule_;
    mutable std::mutex snapshot_mutex_;
    std::optional<ResourceSnapshotBundle> resource_snapshot_;
    std::mutex resource_submit_mutex_;
    std::atomic<bool> resource_stopped_{false};
    ResourceLogState cpu_log_, memory_log_, process_log_;
    std::optional<Clock::time_point> next_overrun_log_;
    std::exception_ptr monitor_failure_;
    Monitor monitor_;
    DeviceStateManager device_states_;
    EventDispatcher dispatcher_;
    ServiceAggregation aggregation_;
    ServiceManager services_;
    std::unique_ptr<RecoveryManager> recovery_;
    std::unique_ptr<ComponentAdmission> component_admission_;
    std::shared_ptr<ComponentMailbox> component_mailbox_ = std::make_shared<ComponentMailbox>();
    mutable std::mutex component_snapshot_mutex_;
    std::vector<ComponentHealthSnapshot> component_snapshot_;
    std::deque<ServiceStateChange> lifecycle_work_;
    bool shutting_down_ = false;
    std::vector<std::string> autostart_;
    std::thread monitor_thread_;
    std::thread timer_thread_;
    int timer_fd_ = -1;
    std::atomic<bool> running_{false};
};

} // namespace runtime
