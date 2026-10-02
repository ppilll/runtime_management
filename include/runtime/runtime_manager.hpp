#pragma once

#include "runtime/device_state_manager.hpp"
#include "runtime/event.hpp"
#include "runtime/logger.hpp"
#include "runtime/monitor.hpp"
#include "runtime/process_supervisor.hpp"
#include "runtime/service_manager.hpp"
#include "runtime/service_aggregation.hpp"
#include <atomic>
#include <set>
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
    // Internal producers submit explicit device triggers to the same FIFO loop.
    void post(DeviceStateEvent event);
    void post(RuntimeEvent event);
    void run();
    std::optional<ServiceStatus> query(const std::string& name) const;
    DeviceStateSnapshot queryDeviceState() const;
    void reportResourceUsage(double cpu_percent, double memory_percent, Clock::time_point at = Clock::now());

private:
    void service_state_changed(const ServiceStateChange& change);
    EventQueue queue_;
    EventQueue monitor_queue_;
    Logger logger_;
    PosixProcessSupervisor processes_;
    Monitor monitor_;
    DeviceStateManager device_states_;
    EventDispatcher dispatcher_;
    ServiceAggregation aggregation_;
    ServiceManager services_;
    const Event* service_cause_ = nullptr; // Event-loop-only adapter context.
    std::set<std::string> service_recoveries_;
    bool shutting_down_ = false;
    std::vector<std::string> autostart_;
    std::thread monitor_thread_;
    std::thread timer_thread_;
    int timer_fd_ = -1;
    std::atomic<bool> running_{false};
};

} // namespace runtime
