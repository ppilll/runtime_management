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
    if (event.type == RuntimeEventType::component_health_changed) return handle_component(event);
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
        const auto& context = event.recovery_context;
        auto same_episode = [](const RecoveryContext& a, const RecoveryContext& b) {
            return a.recovery_generation == b.recovery_generation &&
                   a.initial_fault_generation == b.initial_fault_generation && a.origin == b.origin;
        };
        if (context && (context->recovery_generation == 0 || context->initial_fault_generation == 0 ||
                        context->initial_fault_generation > *event.generation)) return false;
        switch (event.type) {
        case RuntimeEventType::service_started:
            if (context) {
                if (!health.recovery_context || !same_episode(*context, *health.recovery_context) ||
                    !context->execution_generation || *context->execution_generation != *event.generation ||
                    (same_generation && health.candidate_generation != event.generation)) return false;
                health.generation = *event.generation;
                health.recovery_context = context;
                health.candidate_generation = event.generation;
                // RUNNING is only an exec candidate. Preserve the fault/heartbeat latch.
                break;
            }
            if (health.recovery_context) return false; // Ordinary START cannot bypass an active binding.
            if (same_generation) return health.running && !unavailable;
            health.generation = *event.generation;
            health.running = true;
            health.failed = health.stopped = health.heartbeat_lost = health.recovering = false;
            health.candidate_generation.reset();
            break;
        case RuntimeEventType::service_failed:
        case RuntimeEventType::heartbeat_timeout:
            if (same_generation) return event.type == RuntimeEventType::heartbeat_timeout ? health.heartbeat_lost
                                                                                         : health.failed && !health.heartbeat_lost;
            if (context && health.recovery_context && !same_episode(*context, *health.recovery_context)) return false;
            health.generation = *event.generation;
            device_recovering_ = false;
            health.running = false;
            health.failed = true;
            if (event.type == RuntimeEventType::heartbeat_timeout) health.heartbeat_lost = true;
            health.candidate_generation.reset();
            if (context) health.recovery_context = context; // Bound attempt failure retains its episode.
            else {
                health.recovery_context.reset();
                health.recovering = false;
            }
            break;
        case RuntimeEventType::service_stopped:
            if (same_generation) return health.stopped;
            health.generation = *event.generation;
            health.running = false;
            health.stopped = true;
            health.recovering = false;
            health.candidate_generation.reset();
            health.recovery_context = context;
            break;
        case RuntimeEventType::recovery_start:
            if (!same_generation || !unavailable) return false;
            if (!context || context->origin != RecoveryOrigin::automatic_failure || context->execution_generation)
                return false;
            if (health.recovery_context && !same_episode(*context, *health.recovery_context)) return false;
            health.recovery_context = context;
            health.recovering = true;
            break;
        case RuntimeEventType::recovery_success:
            if (!same_generation) return false;
            if (!context || !health.recovery_context || !same_episode(*context, *health.recovery_context) ||
                context->execution_generation != health.recovery_context->execution_generation ||
                !context->execution_generation || health.candidate_generation != event.generation) return false;
            health.running = true;
            health.failed = health.stopped = health.heartbeat_lost = health.recovering = false;
            health.recovery_context.reset();
            health.candidate_generation.reset();
            break;
        case RuntimeEventType::recovery_failed:
            if (!same_generation || !unavailable) return false;
            if (!context || !health.recovery_context || !same_episode(*context, *health.recovery_context) ||
                context->execution_generation != health.recovery_context->execution_generation) return false;
            health.running = false;
            health.failed = true;
            health.recovering = false;
            health.recovery_context.reset();
            health.candidate_generation.reset();
            if (health.policy.criticality == ServiceCriticality::high || health.heartbeat_lost)
                recovery_failed_ = true;
            break;
        default: return false;
        }
    }
    reconcile(event);
    return true;
}

bool ServiceAggregation::bind_recovery(const std::string& name, std::uint64_t generation,
        const RecoveryContext& context, bool recovering, Clock::time_point at) {
    const auto found = services_.find(name);
    if (found == services_.end() || generation == 0 || context.recovery_generation == 0 ||
        context.initial_fault_generation == 0 || context.initial_fault_generation > generation) return false;
    auto& health = found->second;
    if (health.generation != generation) return false;
    const bool existing = health.recovery_context &&
        health.recovery_context->recovery_generation == context.recovery_generation &&
        health.recovery_context->initial_fault_generation == context.initial_fault_generation &&
        health.recovery_context->origin == context.origin;
    if (health.recovery_context && !existing &&
        context.recovery_generation <= health.recovery_context->recovery_generation) return false;
    health.recovery_context = context;
    // First automatic admission is announced by the canonical START receipt.
    health.recovering = recovering && existing && context.origin == RecoveryOrigin::automatic_failure;
    health.candidate_generation.reset();
    if (recovering && existing)
        reconcile(RuntimeEvent{RuntimeEventType::recovery_start, name, "recovery_manager", "captured recovery binding", at});
    return true;
}

void ServiceAggregation::cancel_recovery(const std::string& name, const RecoveryContext& context, Clock::time_point at) {
    const auto found = services_.find(name);
    if (found == services_.end() || !found->second.recovery_context) return;
    auto& health = found->second;
    const auto& active = *health.recovery_context;
    if (active.recovery_generation != context.recovery_generation ||
        active.initial_fault_generation != context.initial_fault_generation || active.origin != context.origin) return;
    health.recovery_context.reset();
    health.candidate_generation.reset();
    health.recovering = false;
    reconcile(RuntimeEvent{RuntimeEventType::service_stopped, name, "recovery_manager", "recovery cancelled", at});
}

bool ServiceAggregation::complete_manual(const RecoveryResult& result) {
    if (result.origin != RecoveryOrigin::manual_restart) return false;
    const RecoveryContext context{result.recovery_generation, result.initial_fault_generation,
                                  result.execution_generation, result.origin};
    if (result.outcome != RecoveryOutcome::success) {
        cancel_recovery(result.service_name, context, result.completed_at);
        return true;
    }
    RuntimeEvent event{RuntimeEventType::recovery_success, result.service_name,
        "recovery_manager", "manual transaction completed", result.completed_at};
    event.generation = result.execution_generation;
    event.recovery_context = context;
    // Writer-local completion only; no automatic recovery trio is published.
    return handle(event);
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
    bool critical_component = false;
    for (const auto& [key, health] : components_) {
        (void)key;
        const auto status = health.fact.sample.status;
        const bool healthy = status == ComponentHealthStatus::healthy && health.fact.observation_fresh;
        if (health.entry.policy.required && !healthy) required_ready = false;
        const bool unavailable = health.adverse == ComponentHealthStatus::unhealthy ||
            (!healthy && health.ever_healthy &&
             (status == ComponentHealthStatus::unknown || status == ComponentHealthStatus::connecting));
        if (unavailable && health.entry.policy.criticality == ServiceCriticality::high) critical_component = true;
        else if (unavailable || health.adverse == ComponentHealthStatus::degraded) warning = true;
    }
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
    if (critical_component) return DeviceState::error;
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
                       (event.type == RuntimeEventType::component_health_changed && target == DeviceState::error) ||
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

void ServiceAggregation::add_component(ComponentPolicyEntry entry) {
    if (entry.component != "mcu_link" || entry.producer_service.empty() || !valid_health_policy(entry.policy) ||
        !services_.count(entry.producer_service) || !components_.empty())
        throw std::invalid_argument("invalid component registry");
    ComponentHealth health{entry};
    health.fact.sample.component = entry.component;
    health.fact.identity.service_name = entry.producer_service;
    components_.emplace(entry.component, std::move(health));
}

bool ServiceAggregation::handle_component(const RuntimeEvent& event) {
    if (!event.service_name.empty() || !event.component_health) return false;
    const auto& fact = *event.component_health;
    const auto found = components_.find(fact.sample.component);
    if (found == components_.end() || event.source != "component:" + fact.sample.component ||
        fact.identity.service_name != found->second.entry.producer_service) return false;
    auto& health = found->second;
    if (fact.identity.launched_generation < health.fact.identity.launched_generation) return false;
    if (fact.identity.launched_generation != health.fact.identity.launched_generation) {
        health.ever_healthy = false;
        health.adverse.reset();
    }
    health.fact = fact;
    const auto status = fact.sample.status;
    if (status == ComponentHealthStatus::healthy && fact.observation_fresh) {
        health.ever_healthy = true;
        health.adverse.reset(); // Only this component; process/resource/terminal remain.
    } else if (status == ComponentHealthStatus::unhealthy) {
        health.adverse = status;
    } else if (status == ComponentHealthStatus::degraded && health.adverse != ComponentHealthStatus::unhealthy) {
        health.adverse = status;
    }
    reconcile(event);
    return true;
}

} // namespace runtime
