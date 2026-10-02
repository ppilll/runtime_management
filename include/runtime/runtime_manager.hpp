#pragma once

#include "runtime/device_state_manager.hpp"
#include "runtime/event.hpp"
#include "runtime/logger.hpp"
#include "runtime/monitor.hpp"
#include "runtime/process_supervisor.hpp"
#include "runtime/service_manager.hpp"
#include "runtime/service_aggregation.hpp"
#include "runtime/recovery_manager.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace runtime {

class RuntimeManager {
public:
    // Observer runs on the runtime writer after state commit; it must not throw.
    explicit RuntimeManager(const std::string& config_path, AggregationOptions aggregation = {},
                            DeviceStateManager::StateChangeSink device_changes = {},
                            ResourceThresholds resources = {});
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
    void reportResourceUsage(double cpu_percent, double memory_percent, Clock::time_point at = Clock::now());

private:
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
    Monitor monitor_;
    DeviceStateManager device_states_;
    EventDispatcher dispatcher_;
    ServiceAggregation aggregation_;
    ServiceManager services_;
    std::unique_ptr<RecoveryManager> recovery_;
    std::deque<ServiceStateChange> lifecycle_work_;
    bool shutting_down_ = false;
    std::vector<std::string> autostart_;
    std::thread monitor_thread_;
    std::thread timer_thread_;
    int timer_fd_ = -1;
    std::atomic<bool> running_{false};
};

} // namespace runtime
