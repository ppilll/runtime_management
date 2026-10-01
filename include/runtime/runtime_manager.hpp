#pragma once

#include "runtime/event.hpp"
#include "runtime/logger.hpp"
#include "runtime/monitor.hpp"
#include "runtime/process_supervisor.hpp"
#include "runtime/service_manager.hpp"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace runtime {

class RuntimeManager {
public:
    explicit RuntimeManager(const std::string& config_path);
    ~RuntimeManager();
    void post(Event event);
    void run();
    std::optional<ServiceStatus> query(const std::string& name) const;

private:
    EventQueue queue_;
    EventQueue monitor_queue_;
    Logger logger_;
    PosixProcessSupervisor processes_;
    Monitor monitor_;
    ServiceManager services_;
    std::vector<std::string> autostart_;
    std::thread monitor_thread_;
    std::thread timer_thread_;
    int timer_fd_ = -1;
    std::atomic<bool> running_{false};
};

} // namespace runtime
