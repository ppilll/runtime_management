#pragma once

#include "runtime/config_manager.hpp"
#include "runtime/event.hpp"
#include "runtime/logger.hpp"
#include "runtime/monitor.hpp"
#include "runtime/process_supervisor.hpp"
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
};

class ServiceManager {
public:
    ServiceManager(ProcessSupervisor& processes, Monitor& monitor, Logger& logger);
    void add(ServiceConfig config);
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
    ProcessSupervisor& processes_;
    Monitor& monitor_;
    Logger& logger_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Service> services_;
};

} // namespace runtime
