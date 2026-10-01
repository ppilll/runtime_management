#include "runtime/service_manager.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <sys/wait.h>

namespace runtime {

const char* state_name(ServiceState state) {
    switch (state) {
    case ServiceState::created: return "CREATED";
    case ServiceState::starting: return "STARTING";
    case ServiceState::running: return "RUNNING";
    case ServiceState::stopping: return "STOPPING";
    case ServiceState::stopped: return "STOPPED";
    case ServiceState::failed: return "FAILED";
    case ServiceState::recovering: return "RECOVERING";
    }
    return "UNKNOWN";
}

ServiceManager::ServiceManager(ProcessSupervisor& processes, Monitor& monitor, Logger& logger)
    : processes_(processes), monitor_(monitor), logger_(logger) {}

void ServiceManager::add(ServiceConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto name = config.service_name;
    if (name.empty() || config.executable.empty()) throw std::invalid_argument("service name and executable are required");
    Service service;
    service.config = std::move(config);
    service.status.service_name = name;
    if (!services_.emplace(name, std::move(service)).second)
        throw std::invalid_argument("duplicate service: " + name);
}

void ServiceManager::start(Service& service, Clock::time_point now) {
    if (service.status.pid > 0 || (service.status.state != ServiceState::created &&
        service.status.state != ServiceState::stopped && service.status.state != ServiceState::failed &&
        service.status.state != ServiceState::recovering)) return;
    service.status.state = ServiceState::starting;
    try {
        service.status.pid = processes_.start(service.config);
        // The exec handshake can consume most of startup_timeout. Heartbeat
        // monitoring begins when the child is actually ready to run.
        service.started = std::max(now, Clock::now());
        service.status.heartbeat_time.reset();
        service.restart_at.reset();
        service.status.state = ServiceState::running;
        monitor_.watch(service.status.service_name, service.started, service.config.heartbeat_timeout);
        logger_.log(LogLevel::info, "service_manager", service.status.service_name + " RUNNING pid=" + std::to_string(service.status.pid));
    } catch (const std::exception& error) {
        fail(service, now, error.what());
    }
}

void ServiceManager::stop(Service& service, Clock::time_point now) {
    if (service.status.state == ServiceState::stopped ||
        service.status.state == ServiceState::stopping) return;
    monitor_.unwatch(service.status.service_name);
    service.restart_at.reset();
    if (service.status.pid <= 0) {
        service.status.state = ServiceState::stopped;
        service.termination_deadline.reset();
        return;
    }
    service.status.state = ServiceState::stopping;
    service.termination_deadline = now + std::chrono::seconds(2);
    try { processes_.stop(service.status.pid); }
    catch (const std::exception& error) {
        logger_.log(LogLevel::error, "service_manager", service.status.service_name + " stop: " + error.what());
    }
}

void ServiceManager::fail(Service& service, Clock::time_point now, const std::string& reason) {
    monitor_.unwatch(service.status.service_name);
    service.status.state = ServiceState::failed;
    logger_.log(LogLevel::error, "service_manager", service.status.service_name + " FAILED: " + reason);
    if (service.status.pid > 0) {
        service.termination_deadline = now + std::chrono::seconds(2);
        try { processes_.stop(service.status.pid); }
        catch (const std::exception& error) {
            logger_.log(LogLevel::error, "service_manager", service.status.service_name + " stop: " + error.what());
        }
    }
    if (service.config.restart_policy != RestartPolicy::never && service.status.restart_count < 5) {
        static constexpr std::array<int, 5> delay_seconds{{2, 5, 10, 30, 60}};
        service.restart_at = now + std::chrono::seconds(delay_seconds[service.status.restart_count]);
        ++service.status.restart_count;
        service.status.state = ServiceState::recovering;
    }
}

void ServiceManager::process_exit(Service& service, const Event& event) {
    if (service.status.pid != event.pid) return;
    service.status.pid = -1;
    service.termination_deadline.reset();
    monitor_.unwatch(service.status.service_name);
    if (service.status.state == ServiceState::stopping) {
        service.status.state = ServiceState::stopped;
    } else if (service.status.state != ServiceState::recovering && service.status.state != ServiceState::failed) {
        const bool clean = WIFEXITED(event.exit_status) && WEXITSTATUS(event.exit_status) == 0;
        if (clean && service.config.restart_policy != RestartPolicy::always) {
            service.status.state = ServiceState::stopped;
            logger_.log(LogLevel::info, "service_manager", service.status.service_name + " exited normally");
        } else {
            fail(service, event.at, "process exited, status=" + std::to_string(event.exit_status));
        }
    }
}

void ServiceManager::handle(const Event& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = services_.find(event.service_name);
    if (it == services_.end()) return;
    auto& service = it->second;
    switch (event.type) {
    case EventType::start: start(service, event.at); break;
    case EventType::stop: stop(service, event.at); break;
    case EventType::heartbeat:
        if (service.status.state == ServiceState::running &&
            (!service.status.heartbeat_time || event.at >= *service.status.heartbeat_time)) {
            service.status.heartbeat_time = event.at;
            monitor_.heartbeat(event.service_name, event.at);
        }
        break;
    case EventType::health_missed:
        if (service.status.state == ServiceState::running && event.missed_count >= 3 &&
            event.at >= service.started &&
            (!service.status.heartbeat_time || *service.status.heartbeat_time < event.at))
            fail(service, event.at, "three consecutive missed heartbeats");
        break;
    case EventType::process_exited: process_exit(service, event); break;
    default: break;
    }
}

void ServiceManager::tick(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, service] : services_) {
        if (service.status.pid > 0 && service.termination_deadline &&
            now >= *service.termination_deadline) {
            try {
                processes_.force_stop(service.status.pid);
                service.termination_deadline.reset();
            } catch (const std::exception& error) {
                logger_.log(LogLevel::error, "service_manager",
                    service.status.service_name + " force stop: " + error.what());
            }
        }
        if (service.status.state == ServiceState::starting && now >= service.started + service.config.startup_timeout)
            fail(service, now, "startup timeout");
        if (service.status.state == ServiceState::recovering && service.status.pid < 0 &&
            service.restart_at && now >= *service.restart_at)
            start(service, now);
    }
}

std::optional<ServiceStatus> ServiceManager::query(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(name);
    if (it == services_.end()) return std::nullopt;
    return it->second.status;
}

std::vector<ServiceStatus> ServiceManager::all_statuses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceStatus> result;
    for (const auto& [name, service] : services_) result.push_back(service.status);
    return result;
}

void ServiceManager::stop_all(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, service] : services_) stop(service, now);
}

} // namespace runtime
