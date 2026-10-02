#include "runtime/service_manager.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/wait.h>
#include <utility>

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

ServiceManager::ServiceManager(ProcessSupervisor& processes, Monitor& monitor, Logger& logger,
                               StateChangeSink state_changes, RestartRequestSink restart_requests)
    : processes_(processes), monitor_(monitor), logger_(logger),
      state_changes_(std::move(state_changes)), restart_requests_(std::move(restart_requests)) {}

namespace {
constexpr unsigned maximum_restarts = 5;

std::chrono::seconds restart_delay(unsigned attempt) {
    unsigned seconds = 2;
    while (attempt > 0 && seconds < 60) {
        --attempt;
        seconds = std::min(seconds * 2, 60u);
    }
    return std::chrono::seconds(seconds);
}

bool valid_transition(ServiceState from, ServiceState to) {
    // Include Phase 1's clean exit, manual restart and shutdown paths alongside
    // the Phase 2 lifecycle edges so existing event handling keeps its contract.
    switch (from) {
    case ServiceState::created:
        return to == ServiceState::starting || to == ServiceState::stopped;
    case ServiceState::starting:
        return to == ServiceState::running || to == ServiceState::failed || to == ServiceState::stopped;
    case ServiceState::running:
        return to == ServiceState::stopping || to == ServiceState::failed || to == ServiceState::stopped;
    case ServiceState::stopping:
        return to == ServiceState::stopped;
    case ServiceState::stopped:
        return to == ServiceState::starting;
    case ServiceState::failed:
        return to == ServiceState::recovering || to == ServiceState::starting ||
               to == ServiceState::stopping || to == ServiceState::stopped;
    case ServiceState::recovering:
        return to == ServiceState::starting || to == ServiceState::stopping ||
               to == ServiceState::stopped;
    }
    return false;
}
} // namespace

void ServiceManager::transition(Service& service, ServiceState next, Clock::time_point at) {
    const auto previous = service.status.state;
    if (previous == next) return;
    if (!valid_transition(previous, next))
        throw std::logic_error(std::string("invalid service transition: ") +
            state_name(previous) + " -> " + state_name(next));
    service.status.state = next;
    if (state_changes_)
        pending_changes_.push_back({service.status.service_name, previous, next, at});
}

void ServiceManager::dispatch_changes(std::vector<ServiceStateChange> changes) const {
    if (!state_changes_) return;
    for (const auto& change : changes) state_changes_(change);
}

void ServiceManager::add(ServiceConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto name = config.service_name;
    if (name.empty() || config.executable.empty()) throw std::invalid_argument("service name and executable are required");
    if (config.startup_timeout <= std::chrono::seconds::zero() ||
        config.shutdown_timeout <= std::chrono::seconds::zero() ||
        config.heartbeat_timeout <= std::chrono::seconds::zero())
        throw std::invalid_argument("service timeouts must be positive");
    Service service;
    service.config = std::move(config);
    service.status.service_name = name;
    if (!services_.emplace(name, std::move(service)).second)
        throw std::invalid_argument("duplicate service: " + name);
}

void ServiceManager::registerService(ServiceConfig config) { add(std::move(config)); }

void ServiceManager::startService(const std::string& name, Clock::time_point now) {
    handle(Event{EventType::start, name, now});
}

void ServiceManager::stopService(const std::string& name, Clock::time_point now) {
    handle(Event{EventType::stop, name, now});
}

bool ServiceManager::restartService(const std::string& name) const {
    RestartRequestSink sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (services_.find(name) == services_.end() || !restart_requests_) return false;
        sink = restart_requests_;
    }
    sink(name);
    return true;
}

std::optional<ServiceStatus> ServiceManager::queryServiceStatus(const std::string& name) const {
    return query(name);
}

std::optional<ServiceConfig> ServiceManager::queryServiceDefinition(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(name);
    if (it == services_.end()) return std::nullopt;
    return it->second.config;
}

std::vector<ServiceStatus> ServiceManager::listServices() const { return all_statuses(); }

std::vector<std::string> ServiceManager::dependency_order() const {
    std::map<std::string, std::size_t> remaining;
    std::map<std::string, std::vector<std::string>> dependents;
    std::set<std::string> ready;
    for (const auto& [name, service] : services_) {
        const std::set<std::string> unique(service.config.dependency.begin(),
                                           service.config.dependency.end());
        remaining[name] = unique.size();
        for (const auto& dependency : unique) {
            if (dependency == name || services_.find(dependency) == services_.end())
                throw std::invalid_argument("invalid dependency: " + name + " -> " + dependency);
            dependents[dependency].push_back(name);
        }
        if (unique.empty()) ready.insert(name);
    }
    std::vector<std::string> order;
    while (!ready.empty()) {
        const auto name = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(name);
        for (const auto& dependent : dependents[name])
            if (--remaining.at(dependent) == 0) ready.insert(dependent);
    }
    if (order.size() != services_.size())
        throw std::invalid_argument("service dependency cycle");
    return order;
}

std::vector<std::string> ServiceManager::startup_order() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dependency_order();
}

bool ServiceManager::dependencies_running(const Service& service) const {
    for (const auto& dependency : service.config.dependency) {
        const auto it = services_.find(dependency);
        if (it == services_.end() || it->second.status.state != ServiceState::running)
            return false;
    }
    return true;
}

void ServiceManager::start_dependencies(const std::string& name, Clock::time_point now) {
    const auto order = dependency_order(); // Validate before any launch.
    const auto& target = services_.at(name);
    if (target.status.state == ServiceState::running ||
        target.status.state == ServiceState::stopping ||
        target.status.state == ServiceState::recovering) return;
    std::set<std::string> needed{name};
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (needed.count(*it) == 0) continue;
        const auto& dependencies = services_.at(*it).config.dependency;
        needed.insert(dependencies.begin(), dependencies.end());
    }
    for (const auto& service_name : order) {
        if (needed.count(service_name) == 0) continue;
        auto& service = services_.at(service_name);
        // A start request must not bypass another service's automatic backoff.
        if (service.status.state == ServiceState::recovering ||
            service.status.state == ServiceState::stopping) continue;
        if (dependencies_running(service)) start(service, now);
        else stop(service, now);
    }
}

void ServiceManager::stop_dependents(const std::string& name, Clock::time_point now) {
    const auto order = dependency_order();
    std::set<std::string> affected{name};
    for (const auto& service_name : order) {
        const auto& dependencies = services_.at(service_name).config.dependency;
        if (std::any_of(dependencies.begin(), dependencies.end(),
                        [&](const std::string& dependency) { return affected.count(dependency) != 0; }))
            affected.insert(service_name);
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (*it != name && affected.count(*it) != 0) stop(services_.at(*it), now);
}

void ServiceManager::start(Service& service, Clock::time_point now) {
    if (service.status.pid > 0 || (service.status.state != ServiceState::created &&
        service.status.state != ServiceState::stopped && service.status.state != ServiceState::failed &&
        service.status.state != ServiceState::recovering)) return;
    if (!dependencies_running(service)) return;
    service.restart_at.reset();
    service.started = now;
    transition(service, ServiceState::starting, now);
    try {
        service.status.pid = processes_.start(service.config);
        if (service.status.pid <= 0) throw std::runtime_error("launch returned an invalid PID");
        // The exec handshake can consume most of startup_timeout. Heartbeat
        // monitoring begins when the child is actually ready to run.
        service.started = std::max(now, Clock::now());
        service.status.start_time = service.started;
        service.status.heartbeat_time.reset();
        service.restart_at.reset();
        transition(service, ServiceState::running, service.started);
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
        transition(service, ServiceState::stopped, now);
        service.termination_deadline.reset();
        return;
    }
    transition(service, ServiceState::stopping, now);
    service.termination_deadline = now + service.config.shutdown_timeout;
    try { processes_.stop(service.status.pid); }
    catch (const std::exception& error) {
        logger_.log(LogLevel::error, "service_manager", service.status.service_name + " stop: " + error.what());
    }
}

void ServiceManager::fail(Service& service, Clock::time_point now, const std::string& reason) {
    monitor_.unwatch(service.status.service_name);
    service.restart_at.reset();
    transition(service, ServiceState::failed, now);
    logger_.log(LogLevel::error, "service_manager", service.status.service_name + " FAILED: " + reason);
    stop_dependents(service.status.service_name, now);
    if (service.status.pid > 0) {
        service.termination_deadline = now + service.config.shutdown_timeout;
        try { processes_.stop(service.status.pid); }
        catch (const std::exception& error) {
            logger_.log(LogLevel::error, "service_manager", service.status.service_name + " stop: " + error.what());
        }
    }
    if (service.config.restart_policy != RestartPolicy::never && service.status.restart_count < maximum_restarts) {
        service.restart_at = now + restart_delay(service.status.restart_count);
        ++service.status.restart_count;
        transition(service, ServiceState::recovering, now);
    }
}

void ServiceManager::process_exit(Service& service, const Event& event) {
    if (event.pid <= 0 || service.status.pid != event.pid) return;
    service.status.pid = -1;
    service.status.start_time.reset();
    service.termination_deadline.reset();
    monitor_.unwatch(service.status.service_name);
    if (service.status.state == ServiceState::stopping) {
        transition(service, ServiceState::stopped, event.at);
    } else if (service.status.state != ServiceState::recovering && service.status.state != ServiceState::failed) {
        const bool clean = event.exit_status >= 0 && WIFEXITED(event.exit_status) &&
                           WEXITSTATUS(event.exit_status) == 0;
        if (clean && service.config.restart_policy != RestartPolicy::always) {
            transition(service, ServiceState::stopped, event.at);
            logger_.log(LogLevel::info, "service_manager", service.status.service_name + " exited normally");
            stop_dependents(service.status.service_name, event.at);
        } else {
            fail(service, event.at, "process exited, status=" + std::to_string(event.exit_status));
        }
    }
}

void ServiceManager::handle(const Event& event) {
    std::vector<ServiceStateChange> changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = services_.find(event.service_name);
        if (it == services_.end()) return;
        auto& service = it->second;
        switch (event.type) {
        case EventType::start: start_dependencies(event.service_name, event.at); break;
        case EventType::stop:
            stop_dependents(event.service_name, event.at);
            stop(service, event.at);
            break;
        case EventType::heartbeat:
            if (service.status.state == ServiceState::running && event.at >= service.started &&
                (event.pid <= 0 || event.pid == service.status.pid) &&
                (!service.status.heartbeat_time || event.at >= *service.status.heartbeat_time)) {
                service.status.heartbeat_time = event.at;
                monitor_.heartbeat(event.service_name, event.at);
            }
            break;
        case EventType::health_missed:
            if (service.status.state == ServiceState::running && event.missed_count > 0 &&
                (event.pid <= 0 || event.pid == service.status.pid) &&
                event.at >= service.status.heartbeat_time.value_or(service.started) + service.config.heartbeat_timeout)
                fail(service, event.at, "heartbeat timeout");
            break;
        case EventType::process_exited: process_exit(service, event); break;
        default: break;
        }
        changes.swap(pending_changes_);
    }
    dispatch_changes(std::move(changes));
}

void ServiceManager::tick(Clock::time_point now) {
    std::vector<ServiceStateChange> changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto order = dependency_order();
        // Escalation signals follow the same reverse ordering as graceful stop.
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            auto& service = services_.at(*it);
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
        }
        for (const auto& name : order) {
            auto& service = services_.at(name);
            if (service.status.state == ServiceState::starting && now >= service.started + service.config.startup_timeout)
                fail(service, now, "startup timeout");
            if (service.status.state == ServiceState::recovering && service.status.pid < 0 &&
                service.restart_at && now >= *service.restart_at)
                start(service, now);
        }
        changes.swap(pending_changes_);
    }
    dispatch_changes(std::move(changes));
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
    std::vector<ServiceStateChange> changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto order = dependency_order();
        for (auto it = order.rbegin(); it != order.rend(); ++it) stop(services_.at(*it), now);
        changes.swap(pending_changes_);
    }
    dispatch_changes(std::move(changes));
}

} // namespace runtime
