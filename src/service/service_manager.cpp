#include "runtime/service_manager.hpp"
#include <algorithm>
#include <limits>
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
               to == ServiceState::stopped || to == ServiceState::failed;
    }
    return false;
}
} // namespace

void ServiceManager::transition(Service& service, ServiceState next, Clock::time_point at,
                                bool recovery_exhausted) {
    const auto previous = service.status.state;
    if (previous == next) return;
    if (!valid_transition(previous, next) &&
        !(next == ServiceState::failed && service.cause == ServiceChangeCause::recovery_finalization))
        throw std::logic_error(std::string("invalid service transition: ") +
            state_name(previous) + " -> " + state_name(next));
    // A new launch, fault or explicit stop invalidates earlier recovery work.
    // STOPPING -> STOPPED completes the same stop generation.
    if (next == ServiceState::starting || next == ServiceState::failed ||
        next == ServiceState::stopping ||
        (next == ServiceState::stopped && previous != ServiceState::stopping)) {
        if (service.status.generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("service generation exhausted");
        ++service.status.generation;
    }
    service.status.state = next;
    if (next == ServiceState::starting && service.recovery)
        service.recovery->context.execution_generation = service.status.generation;
    if (state_changes_)
        pending_changes_.push_back({service.status.service_name, previous, next, at,
                                   service.status.generation, recovery_exhausted, service.cause, service.recovery, service.failure_type});
}

void ServiceManager::dispatch_changes(std::vector<ServiceStateChange> changes) const {
    if (!state_changes_) return;
    for (const auto& change : changes) state_changes_(change);
}

void ServiceManager::add(ServiceConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto name = config.service_name;
    ConfigManager::validate(config);
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

void ServiceManager::start_dependencies(const std::string& name, Clock::time_point now,
                                        std::optional<Clock::time_point> deadline, const std::function<bool()>& gate) {
    const auto order = dependency_order(); // Validate before any launch.
    const auto& target = services_.at(name);
    if (target.recovery || target.status.state == ServiceState::running ||
        target.status.state == ServiceState::stopping) return;
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
        if (service.recovery || service.status.state == ServiceState::stopping) continue;
        if (dependencies_running(service)) {
            std::optional<std::chrono::seconds> cap;
            if (deadline) {
                const auto checkpoint = std::max(now, Clock::now());
                if (checkpoint >= *deadline) return;
                cap = std::chrono::duration_cast<std::chrono::seconds>(*deadline - checkpoint);
                if (*cap < std::chrono::seconds(1)) return;
            }
            if (gate && !gate()) return;
            start(service, now, cap);
            if (gate && !gate()) return;
        }
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
        if (*it != name && affected.count(*it) != 0) {
            auto& dependent = services_.at(*it);
            dependent.cause = ServiceChangeCause::dependency_stop;
            stop(dependent, now);
        }
}

void ServiceManager::start(Service& service, Clock::time_point now, std::optional<std::chrono::seconds> startup_cap) {
    if (service.status.pid > 0 || (service.status.state != ServiceState::created &&
        service.status.state != ServiceState::stopped && service.status.state != ServiceState::failed &&
        service.status.state != ServiceState::recovering)) return;
    if (!dependencies_running(service)) return;
    service.started = now;
    service.failure_type.reset();
    transition(service, ServiceState::starting, now);
    service.status.launched_generation = service.status.generation;
    if (service.recovery) service.recovery->context.execution_generation = service.status.generation;
    auto launch_config = service.config;
    if (startup_cap) launch_config.startup_timeout = std::min(*startup_cap, launch_config.startup_timeout);
    try {
        service.status.pid = processes_.start(launch_config);
        if (service.status.pid <= 0) throw std::runtime_error("launch returned an invalid PID");
    } catch (const std::runtime_error& error) {
        service.status.launched_generation = 0;
        fail(service, std::max(now, Clock::now()), error.what(),
             std::string(error.what()) == "process startup timeout" ? FailureType::startup_timeout : FailureType::startup_failure);
        return;
    }
    // The exec handshake can consume most of startup_timeout. Heartbeat
    // monitoring begins when the child is actually ready to run.
    service.started = std::max(now, Clock::now());
    service.status.start_time = service.started;
    service.status.heartbeat_time.reset();
    transition(service, ServiceState::running, service.started);
    monitor_.watch(service.status.service_name, service.started, service.config.heartbeat_timeout,
                   service.status.launched_generation);
    logger_.log(LogLevel::info, "service_manager", service.status.service_name + " RUNNING pid=" + std::to_string(service.status.pid));

}

void ServiceManager::stop(Service& service, Clock::time_point now) {
    service.failure_type.reset();
    if (service.status.state == ServiceState::stopped ||
        service.status.state == ServiceState::stopping) {
        service.recovery.reset();
        return;
    }
    monitor_.unwatch(service.status.service_name);
    if (service.status.pid <= 0) {
        transition(service, ServiceState::stopped, now);
        service.termination_deadline.reset();
        service.termination_requested = false;
        service.recovery.reset();
        return;
    }
    transition(service, ServiceState::stopping, now);
    service.recovery.reset();
    if (service.termination_requested) return;
    service.termination_requested = true;
    service.termination_deadline = now + service.config.shutdown_timeout;
    try { processes_.stop(service.status.pid); }
    catch (const std::exception& error) {
        logger_.log(LogLevel::error, "service_manager", service.status.service_name + " stop: " + error.what());
    }
}

void ServiceManager::fail(Service& service, Clock::time_point now, const std::string& reason, FailureType type) {
    monitor_.unwatch(service.status.service_name);
    service.cause = ServiceChangeCause::failure;
    service.failure_type = type;
    transition(service, ServiceState::failed, now);
    logger_.log(LogLevel::error, "service_manager", service.status.service_name + " FAILED: " + reason);
    stop_dependents(service.status.service_name, now);
    if (service.status.pid > 0 && !service.termination_requested) {
        service.termination_requested = true;
        service.termination_deadline = now + service.config.shutdown_timeout;
        try { processes_.stop(service.status.pid); }
        catch (const std::exception& error) {
            logger_.log(LogLevel::error, "service_manager", service.status.service_name + " stop: " + error.what());
        }
    }
}

void ServiceManager::process_exit(Service& service, const Event& event, std::optional<ExitDisposition> disposition) {
    if (event.pid <= 0 || service.status.pid != event.pid ||
        (event.instance_generation && *event.instance_generation != service.status.launched_generation)) return;
    service.status.pid = -1;
    service.status.launched_generation = 0;
    service.status.start_time.reset();
    service.termination_deadline.reset();
    service.termination_requested = false;
    monitor_.unwatch(service.status.service_name);
    if (service.status.state == ServiceState::stopping) {
        if (service.recovery && service.recovery->context.origin == RecoveryOrigin::manual_restart)
            service.cause = ServiceChangeCause::recovery_preparation;
        transition(service, ServiceState::stopped, event.at);
    } else if (service.status.state != ServiceState::recovering && service.status.state != ServiceState::failed) {
        const bool clean = event.exit_status >= 0 && WIFEXITED(event.exit_status) &&
                           WEXITSTATUS(event.exit_status) == 0;
        if (disposition.value_or(clean ? ExitDisposition::normal_stopped : ExitDisposition::recoverable_failure) == ExitDisposition::normal_stopped) {
            transition(service, ServiceState::stopped, event.at);
            logger_.log(LogLevel::info, "service_manager", service.status.service_name + " exited normally");
            stop_dependents(service.status.service_name, event.at);
        } else {
            fail(service, event.at, "process exited, status=" + std::to_string(event.exit_status),
                 clean ? FailureType::clean_exit_restart : FailureType::process_crash);
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
        service.cause = ServiceChangeCause::lifecycle;
        switch (event.type) {
        case EventType::start: start_dependencies(event.service_name, event.at); break;
        case EventType::stop:
            service.cause = ServiceChangeCause::explicit_stop;
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
                (!event.instance_generation || *event.instance_generation == service.status.launched_generation) &&
                (event.pid <= 0 || event.pid == service.status.pid) &&
                event.at >= service.status.heartbeat_time.value_or(service.started) + service.config.heartbeat_timeout)
                fail(service, event.at, "heartbeat timeout", FailureType::heartbeat_timeout);
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
                fail(service, now, "startup timeout", FailureType::startup_timeout);

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

std::optional<Clock::time_point> ServiceManager::shutdown_deadline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<Clock::time_point> latest;
    for (const auto& [name, service] : services_) {
        (void)name;
        if (service.status.pid > 0 && service.termination_deadline &&
            (!latest || *service.termination_deadline > *latest))
            latest = service.termination_deadline;
    }
    return latest;
}

namespace {
RecoveryExecutionSnapshot executionSnapshot(const ServiceStatus& status) {
    RecoveryExecutionState state = RecoveryExecutionState::unavailable;
    switch (status.state) {
    case ServiceState::starting: state = RecoveryExecutionState::starting; break;
    case ServiceState::running: state = RecoveryExecutionState::running; break;
    case ServiceState::stopping: state = RecoveryExecutionState::stopping; break;
    case ServiceState::stopped: state = RecoveryExecutionState::stopped; break;
    case ServiceState::failed: state = RecoveryExecutionState::failed; break;
    case ServiceState::recovering: state = RecoveryExecutionState::recovering; break;
    case ServiceState::created: break;
    }
    return {status.generation, status.launched_generation, status.pid, state};
}

bool matches(const RecoveryOperation& stored, const RecoveryOperation& operation) {
    return stored.service_name == operation.service_name &&
        stored.context.recovery_generation == operation.context.recovery_generation &&
        stored.context.initial_fault_generation == operation.context.initial_fault_generation &&
        stored.context.origin == operation.context.origin;
}
} // namespace

std::optional<RecoveryExecutionSnapshot> ServiceManager::snapshot(const std::string& name) const {
    const auto status = query(name);
    if (!status) return {};
    return executionSnapshot(*status);
}

void ServiceManager::handleProcessExit(const Event& event, ExitDisposition disposition) {
    std::vector<ServiceStateChange> changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = services_.find(event.service_name);
        if (found == services_.end() || event.type != EventType::process_exited) return;
        found->second.cause = ServiceChangeCause::lifecycle;
        process_exit(found->second, event, disposition);
        changes.swap(pending_changes_);
    }
    dispatch_changes(std::move(changes));
}

RecoveryExecutionReply ServiceManager::prepareRecovery(const RecoveryOperation& operation, unsigned count,
                                                       Clock::time_point now) {
    RecoveryExecutionReply reply;
    std::vector<ServiceStateChange> changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = services_.find(operation.service_name);
        if (found == services_.end() || operation.context.recovery_generation == 0 ||
            operation.context.initial_fault_generation == 0 || operation.context.execution_generation) return reply;
        auto& service = found->second;
        if (service.status.generation != operation.expected_current_generation || service.recovery) return reply;
        if (operation.context.origin == RecoveryOrigin::automatic_failure && service.status.state != ServiceState::failed)
            return reply;
        service.status.restart_count = std::max(service.status.restart_count, count);
        service.recovery = operation;
        service.cause = ServiceChangeCause::recovery_preparation;
        if (operation.context.origin == RecoveryOrigin::manual_restart) {
            stop_dependents(operation.service_name, now);
            stop(service, now);
            service.recovery = operation; // stop revoked the previous execution permission.
        } else if (!operation.finalize_only) transition(service, ServiceState::recovering, now);
        reply = {RecoveryStep::prepared, executionSnapshot(service.status)};
        changes.swap(pending_changes_);
    }
    dispatch_changes(std::move(changes));
    return reply;
}

bool ServiceManager::recoveryReady(const RecoveryOperation& operation, Clock::time_point now,
                                   const std::function<bool()>& gate) {
    bool ready = false;
    std::vector<ServiceStateChange> changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = services_.find(operation.service_name);
        if (found == services_.end()) return false;
        auto& service = found->second;
        if (!service.recovery || !matches(*service.recovery, operation) || service.status.pid > 0) return false;
        if (operation.context.origin == RecoveryOrigin::manual_restart) {
            // One prerequisite per writer turn, so its captured callbacks drain
            // before another launch. Retain the static forward closure/order.
            const auto order = dependency_order();
            std::set<std::string> needed(service.config.dependency.begin(), service.config.dependency.end());
            for (auto it = order.rbegin(); it != order.rend(); ++it)
                if (needed.count(*it)) {
                    const auto& dependencies = services_.at(*it).config.dependency;
                    needed.insert(dependencies.begin(), dependencies.end());
                }
            for (const auto& name : order) {
                if (!needed.count(name)) continue;
                auto& prerequisite = services_.at(name);
                if (prerequisite.status.state == ServiceState::running) continue;
                const auto checkpoint = std::max(now, Clock::now());
                if (!prerequisite.recovery && prerequisite.status.pid <= 0 &&
                    prerequisite.status.state != ServiceState::stopping && dependencies_running(prerequisite) &&
                    checkpoint < operation.deadline && (!gate || gate())) {
                    const auto cap = std::chrono::duration_cast<std::chrono::seconds>(operation.deadline - checkpoint);
                    if (cap >= std::chrono::seconds(1)) start(prerequisite, checkpoint, cap);
                }
                changes.swap(pending_changes_);
                // Even the last prerequisite must drain before the target launch.
                break;
            }
            if (!changes.empty()) {
                ready = false;
            } else ready = dependencies_running(service);
        } else {
            ready = dependencies_running(service);
        }
        if (changes.empty()) changes.swap(pending_changes_);
    }
    dispatch_changes(std::move(changes));
    return ready;
}

RecoveryExecutionReply ServiceManager::launchRecoveryAttempt(const RecoveryOperation& operation,
        std::uint64_t expected, Clock::duration remaining, Clock::time_point now) {
    RecoveryExecutionReply reply;
    std::vector<ServiceStateChange> changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = services_.find(operation.service_name);
        if (found == services_.end()) return reply;
        auto& service = found->second;
        if (!service.recovery || !matches(*service.recovery, operation) || service.status.generation != expected)
            return reply;
        // Existing supervisor uses whole seconds. Round down, never exceed the remaining episode budget.
        const auto checkpoint = std::max(now, Clock::now());
        if (checkpoint >= operation.deadline)
            return {RecoveryStep::blocked, executionSnapshot(service.status)};
        const auto cap = std::chrono::duration_cast<std::chrono::seconds>(
            std::min(remaining, operation.deadline - checkpoint));
        if (service.status.pid > 0 || !dependencies_running(service) || cap < std::chrono::seconds(1))
            return {RecoveryStep::blocked, executionSnapshot(service.status)};
        service.recovery = operation;
        service.cause = ServiceChangeCause::lifecycle;
        start(service, now, cap);
        reply.captured = executionSnapshot(service.status);
        reply.step = service.status.state == ServiceState::running ? RecoveryStep::launched : RecoveryStep::failed;
        reply.failure_type = service.failure_type.value_or(FailureType::startup_failure);
        changes.swap(pending_changes_);
    }
    dispatch_changes(std::move(changes));
    return reply;
}

RecoveryExecutionReply ServiceManager::finishRecoveryFailure(const RecoveryOperation& operation,
        std::uint64_t expected, RecoveryTerminalReason reason, Clock::time_point now) {
    (void)reason;
    RecoveryExecutionReply reply;
    std::vector<ServiceStateChange> changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = services_.find(operation.service_name);
        if (found == services_.end()) return reply;
        auto& service = found->second;
        if (!service.recovery || !matches(*service.recovery, operation) || service.status.generation != expected)
            return reply;
        monitor_.unwatch(operation.service_name);
        service.cause = ServiceChangeCause::recovery_finalization;
        service.failure_type.reset();
        transition(service, ServiceState::failed, now);
        stop_dependents(operation.service_name, now);
        // Preserve an already promised grace period, and the unreaped PID/instance token.
        if (service.status.pid > 0 && !service.termination_requested) {
            service.termination_requested = true;
            service.termination_deadline = now + service.config.shutdown_timeout;
            try { processes_.stop(service.status.pid); }
            catch (const std::exception& error) {
                logger_.log(LogLevel::error, "service_manager", operation.service_name + " stop: " + error.what());
            }
        }
        reply = {RecoveryStep::prepared, executionSnapshot(service.status)};
        service.recovery.reset();
        changes.swap(pending_changes_);
    }
    dispatch_changes(std::move(changes));
    return reply;
}

void ServiceManager::releaseRecovery(const std::string& name, const RecoveryContext& context) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(name);
    if (found != services_.end() && found->second.recovery &&
        found->second.recovery->context.recovery_generation == context.recovery_generation)
        found->second.recovery.reset();
}

void ServiceManager::projectRestartCount(const std::string& name, unsigned total) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(name);
    if (found != services_.end()) found->second.status.restart_count = std::max(found->second.status.restart_count, total);
}

} // namespace runtime
