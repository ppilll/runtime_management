#include "runtime/device_state_manager.hpp"
#include <array>
#include <optional>
#include <utility>

namespace runtime {

const char* state_name(DeviceState state) {
    switch (state) {
    case DeviceState::booting: return "BOOTING";
    case DeviceState::ready: return "READY";
    case DeviceState::running: return "RUNNING";
    case DeviceState::warning: return "WARNING";
    case DeviceState::error: return "ERROR";
    case DeviceState::recovering: return "RECOVERING";
    case DeviceState::offline: return "OFFLINE";
    }
    return "UNKNOWN";
}

namespace {
struct Transition {
    DeviceState from;
    DeviceStateEventType event;
    DeviceState to;
};

// This is the complete table in docs/P3/STATE_TRANSITION.md.
constexpr std::array<Transition, 9> transitions{{
    {DeviceState::booting, DeviceStateEventType::runtime_initialized, DeviceState::ready},
    {DeviceState::ready, DeviceStateEventType::required_services_ready, DeviceState::running},
    {DeviceState::running, DeviceStateEventType::warning, DeviceState::warning},
    {DeviceState::running, DeviceStateEventType::critical_failure, DeviceState::error},
    {DeviceState::warning, DeviceStateEventType::issue_recovered, DeviceState::running},
    {DeviceState::warning, DeviceStateEventType::failure_escalated, DeviceState::error},
    {DeviceState::error, DeviceStateEventType::recovery_started, DeviceState::recovering},
    {DeviceState::recovering, DeviceStateEventType::recovery_succeeded, DeviceState::running},
    {DeviceState::recovering, DeviceStateEventType::recovery_failed, DeviceState::offline},
}};

// Additional aggregate-health edges in docs/P3/AGGREGATION_RULE.md. These require
// an explicit health_target; the original no-target state machine stays intact.
constexpr std::array<Transition, 15> aggregation_transitions{{
    {DeviceState::ready, DeviceStateEventType::warning, DeviceState::warning},
    {DeviceState::ready, DeviceStateEventType::critical_failure, DeviceState::error},
    {DeviceState::running, DeviceStateEventType::issue_recovered, DeviceState::ready},
    {DeviceState::warning, DeviceStateEventType::issue_recovered, DeviceState::ready},
    {DeviceState::error, DeviceStateEventType::issue_recovered, DeviceState::ready},
    {DeviceState::error, DeviceStateEventType::issue_recovered, DeviceState::running},
    {DeviceState::error, DeviceStateEventType::issue_recovered, DeviceState::warning},
    {DeviceState::recovering, DeviceStateEventType::recovery_succeeded, DeviceState::ready},
    {DeviceState::recovering, DeviceStateEventType::recovery_succeeded, DeviceState::warning},
    {DeviceState::recovering, DeviceStateEventType::recovery_succeeded, DeviceState::error},
    {DeviceState::recovering, DeviceStateEventType::critical_failure, DeviceState::error},
    {DeviceState::error, DeviceStateEventType::recovery_failed, DeviceState::offline},
    {DeviceState::ready, DeviceStateEventType::recovery_failed, DeviceState::offline},
    {DeviceState::running, DeviceStateEventType::recovery_failed, DeviceState::offline},
    {DeviceState::warning, DeviceStateEventType::recovery_failed, DeviceState::offline},
}};

std::optional<DeviceState> aggregate_next_state(DeviceState from, const DeviceStateEvent& event) {
    for (const auto& table : {std::make_pair(transitions.data(), transitions.size()),
                              std::make_pair(aggregation_transitions.data(), aggregation_transitions.size())}) {
        for (std::size_t i = 0; i < table.second; ++i) {
            const auto& edge = table.first[i];
            if (edge.from == from && edge.event == event.type && edge.to == *event.health_target)
                return edge.to;
        }
    }
    return std::nullopt;
}

std::optional<DeviceState> next_state(DeviceState from, DeviceStateEventType event) {
    for (const auto& transition : transitions)
        if (transition.from == from && transition.event == event) return transition.to;
    return std::nullopt;
}
} // namespace

DeviceStateManager::DeviceStateManager(StateChangeSink state_changes, DeviceStateTimestamp started_at)
    : state_changes_(std::move(state_changes)) {
    state_.timestamp = started_at;
}

DeviceTransitionResult DeviceStateManager::handle(const DeviceStateEvent& event) {
    if (event.source.empty() || event.reason.empty()) return DeviceTransitionResult::invalid_metadata;
    DeviceStateSnapshot changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto next = event.health_target ? aggregate_next_state(state_.current, event)
                                             : next_state(state_.current, event.type);
        if (!next) return DeviceTransitionResult::invalid_transition;
        changed = DeviceStateSnapshot{*next, state_.current, event.source, event.reason, event.timestamp};
        state_ = changed;
    }
    if (state_changes_) state_changes_(changed);
    return DeviceTransitionResult::transitioned;
}

DeviceStateSnapshot DeviceStateManager::query() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

} // namespace runtime
