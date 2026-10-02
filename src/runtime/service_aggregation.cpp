#include "runtime/service_aggregation.hpp"
#include <stdexcept>
#include <utility>

namespace runtime {

ServiceAggregation::ServiceAggregation(DeviceStateManager& states, AggregationOptions options)
    : states_(states), options_(std::move(options)) {}

void ServiceAggregation::add_service(const std::string& name, bool autostart) {
    if (name.empty()) throw std::invalid_argument("aggregation requires a service name");
    ServiceHealthPolicy policy{ServiceCriticality::low, autostart};
    if (name == "control_service") policy = {ServiceCriticality::high, true};
    else if (name == "vision_service") policy = {ServiceCriticality::medium, false};
    else if (name == "ota_service") policy = {ServiceCriticality::low, false};
    const auto override = options_.policies.find(name);
    if (override != options_.policies.end()) policy = override->second;
    if (name == "vision_service" && options_.vision_required)
        policy = {ServiceCriticality::high, true};
    if (policy.criticality != ServiceCriticality::high &&
        policy.criticality != ServiceCriticality::medium && policy.criticality != ServiceCriticality::low)
        throw std::invalid_argument("invalid service criticality");
    if (!services_.emplace(name, Health{policy}).second)
        throw std::invalid_argument("duplicate aggregation service: " + name);
}

bool ServiceAggregation::handle(const RuntimeEvent& event) {
    if (event.source.empty() || event.reason.empty()) return false;
    auto service = services_.find(event.service_name);
    if (!event.service_name.empty() && service == services_.end()) return false;
    if (event.type == RuntimeEventType::resource_warning) {
        if (!event.service_name.empty() ||
            (event.severity != ResourceSeverity::warning && event.severity != ResourceSeverity::critical)) return false;
        if (event.active) resource_warnings_[event.source] = event.severity;
        else resource_warnings_.erase(event.source);
        if (event.active && event.severity == ResourceSeverity::critical) device_recovering_ = false;
    } else if (event.service_name.empty()) {
        switch (event.type) {
        case RuntimeEventType::recovery_start: device_recovering_ = true; break;
        case RuntimeEventType::recovery_success: device_recovering_ = false; break;
        case RuntimeEventType::recovery_failed:
            device_recovering_ = false;
            recovery_failed_ = true;
            break;
        default: return false;
        }
    } else {
        auto& health = service->second;
        if (!event.generation || *event.generation == 0 || *event.generation < health.generation) return false;
        const bool same_generation = *event.generation == health.generation;
        const bool unavailable = health.failed || health.stopped || health.heartbeat_lost;
        switch (event.type) {
        case RuntimeEventType::service_started:
            if (same_generation) return health.running && !unavailable;
            break;
        case RuntimeEventType::service_failed:
            if (same_generation) return health.failed && !health.heartbeat_lost;
            break;
        case RuntimeEventType::heartbeat_timeout:
            if (same_generation) return health.heartbeat_lost;
            break;
        case RuntimeEventType::service_stopped:
            if (same_generation) return health.stopped;
            break;
        case RuntimeEventType::recovery_start:
        case RuntimeEventType::recovery_failed:
            if (!same_generation || !unavailable) return false;
            break;
        case RuntimeEventType::recovery_success:
            if (!same_generation) return false;
            if (!unavailable) return health.running; // Idempotent success, no new transition.
            // An explicit stop cancels recovery, even for a matching token.
            if (health.stopped && !health.recovering) return false;
            break;
        default: return false;
        }
        health.generation = *event.generation;
        switch (event.type) {
        case RuntimeEventType::service_started:
        case RuntimeEventType::recovery_success:
            health.running = true;
            health.failed = health.stopped = health.heartbeat_lost = health.recovering = false;
            break;
        case RuntimeEventType::service_failed:
            device_recovering_ = false;
            health.running = false;
            health.failed = true;
            health.recovering = false;
            break;
        case RuntimeEventType::service_stopped:
            health.running = false;
            health.stopped = true;
            health.recovering = false;
            // Do not clear failure/timeout merely because its process was stopped.
            break;
        case RuntimeEventType::heartbeat_timeout:
            device_recovering_ = false;
            health.running = false;
            health.heartbeat_lost = health.failed = true;
            health.recovering = false;
            break;
        case RuntimeEventType::recovery_start:
            if (!health.failed && !health.stopped && !health.heartbeat_lost) return false;
            health.recovering = true;
            break;
        case RuntimeEventType::recovery_failed:
            health.running = false;
            health.failed = true;
            health.recovering = false;
            if (health.policy.criticality == ServiceCriticality::high || health.heartbeat_lost)
                recovery_failed_ = true;
            break;
        default: return false;
        }
    }
    reconcile(event);
    return true;
}

DeviceState ServiceAggregation::evaluate() const {
    if (recovery_failed_) return DeviceState::offline;
    bool critical_failure = false;
    bool unrecovered_critical = false;
    bool warning = !resource_warnings_.empty();
    bool critical_resource = false;
    for (const auto& [source, severity] : resource_warnings_) {
        (void)source;
        critical_resource |= severity == ResourceSeverity::critical;
    }
    bool required_ready = true;
    bool any_running = false;
    for (const auto& [name, health] : services_) {
        (void)name;
        any_running |= health.running;
        if (health.policy.required && !health.running) required_ready = false;
        const bool unavailable = health.failed || health.stopped || health.heartbeat_lost;
        if (!unavailable) continue;
        if (health.heartbeat_lost || health.policy.criticality == ServiceCriticality::high) {
            critical_failure = true;
            unrecovered_critical |= !health.recovering;
        } else {
            warning = true;
        }
    }
    // A service restart cannot resolve critical resource pressure. Only a new
    // measurement from that resource producer can downgrade or clear it.
    if (critical_resource) return DeviceState::error;
    if (critical_failure) {
        if (device_recovering_ || !unrecovered_critical) return DeviceState::recovering;
        return DeviceState::error;
    }
    if (warning) return DeviceState::warning;
    // Do not claim RUNNING for an empty or wholly inactive registry.
    if (!required_ready || !any_running) return DeviceState::ready;
    return DeviceState::running;
}

void ServiceAggregation::transition(DeviceStateEventType type, DeviceState target, const RuntimeEvent& event) {
    DeviceStateEvent trigger{type, event.source, event.reason, event.at};
    trigger.health_target = target;
    if (states_.handle(trigger) != DeviceTransitionResult::transitioned)
        throw std::logic_error("aggregate health transition rejected");
}

void ServiceAggregation::reconcile(const RuntimeEvent& event) {
    const auto target = evaluate();
    const auto current = states_.query().current;
    // Runtime initialization owns BOOTING. OFFLINE is terminal.
    if (current == DeviceState::booting || current == DeviceState::offline || current == target) return;
    if (target == DeviceState::offline) {
        transition(DeviceStateEventType::recovery_failed, target, event);
    } else if (target == DeviceState::recovering) {
        if (current != DeviceState::error && current != DeviceState::recovering) {
            transition(current == DeviceState::warning ? DeviceStateEventType::failure_escalated
                                                      : DeviceStateEventType::critical_failure,
                       DeviceState::error, event);
        }
        transition(DeviceStateEventType::recovery_started, target, event);
    } else if (current == DeviceState::recovering) {
        transition(event.type == RuntimeEventType::service_failed || event.type == RuntimeEventType::heartbeat_timeout ||
                       (event.type == RuntimeEventType::resource_warning && event.active &&
                        event.severity == ResourceSeverity::critical)
                       ? DeviceStateEventType::critical_failure : DeviceStateEventType::recovery_succeeded,
                   target, event);
    } else if (target == DeviceState::error) {
        transition(current == DeviceState::warning ? DeviceStateEventType::failure_escalated
                                                  : DeviceStateEventType::critical_failure,
                   target, event);
    } else if (current == DeviceState::error || current == DeviceState::warning || target == DeviceState::ready) {
        transition(DeviceStateEventType::issue_recovered, target, event);
    } else {
        transition(target == DeviceState::warning ? DeviceStateEventType::warning
                                                 : DeviceStateEventType::required_services_ready,
                   target, event);
    }
}

void ServiceAggregation::refresh(Clock::time_point at) {
    reconcile(RuntimeEvent{RuntimeEventType::recovery_success, {}, "service_aggregation",
                           "recalculate registered service health", at});
}

} // namespace runtime
