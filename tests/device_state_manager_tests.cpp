#include "runtime/device_state_manager.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace runtime;
using namespace std::chrono_literals;

const DeviceStateTimestamp base{10s};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool same_snapshot(const DeviceStateSnapshot& left, const DeviceStateSnapshot& right) {
    return left.current == right.current && left.previous == right.previous &&
        left.source == right.source && left.reason == right.reason && left.timestamp == right.timestamp;
}

DeviceStateEvent trigger(DeviceStateEventType type) {
    return {type, "test producer", "explicit test trigger", base + 1s};
}

void accept(DeviceStateManager& manager, DeviceStateEventType type) {
    require(manager.handle(trigger(type)) == DeviceTransitionResult::transitioned, "expected transition rejected");
}

void reach(DeviceStateManager& manager, DeviceState state) {
    if (state == DeviceState::booting) return;
    accept(manager, DeviceStateEventType::runtime_initialized);
    if (state == DeviceState::ready) return;
    accept(manager, DeviceStateEventType::required_services_ready);
    if (state == DeviceState::running) return;
    if (state == DeviceState::warning) {
        accept(manager, DeviceStateEventType::warning);
        return;
    }
    accept(manager, DeviceStateEventType::critical_failure);
    if (state == DeviceState::error) return;
    accept(manager, DeviceStateEventType::recovery_started);
    if (state == DeviceState::recovering) return;
    accept(manager, DeviceStateEventType::recovery_failed);
}

void test_initial_snapshot_and_names() {
    DeviceStateManager manager({}, base);
    const auto state = manager.query();
    require(state.current == DeviceState::booting && state.previous == DeviceState::booting,
            "initial state must be BOOTING");
    require(state.source == "runtime_manager" && !state.reason.empty() && state.timestamp == base,
            "initial metadata missing");
    const std::array<DeviceState, 7> states{{DeviceState::booting, DeviceState::ready, DeviceState::running,
        DeviceState::warning, DeviceState::error, DeviceState::recovering, DeviceState::offline}};
    const std::array<const char*, 7> names{{"BOOTING", "READY", "RUNNING", "WARNING", "ERROR", "RECOVERING", "OFFLINE"}};
    for (std::size_t i = 0; i < states.size(); ++i)
        require(std::string(state_name(states[i])) == names[i], "device state name mismatch");
    require(std::string(state_name(static_cast<DeviceState>(999))) == "UNKNOWN", "unknown state label");
    auto copy = manager.query();
    copy.current = DeviceState::offline;
    copy.reason.clear();
    require(same_snapshot(manager.query(), state), "query exposed mutable state");
}

void test_normal_boot() {
    DeviceStateManager manager({}, base);
    const DeviceStateEvent initialized{DeviceStateEventType::runtime_initialized,
        "runtime_manager", "initialization complete", base + 2s};
    require(manager.handle(initialized) == DeviceTransitionResult::transitioned, "BOOTING -> READY failed");
    require(same_snapshot(manager.query(), {DeviceState::ready, DeviceState::booting,
        initialized.source, initialized.reason, initialized.timestamp}), "READY metadata mismatch");
    const DeviceStateEvent ready{DeviceStateEventType::required_services_ready,
        "health aggregator", "required services healthy", base + 3s};
    require(manager.handle(ready) == DeviceTransitionResult::transitioned, "READY -> RUNNING failed");
    require(same_snapshot(manager.query(), {DeviceState::running, DeviceState::ready,
        ready.source, ready.reason, ready.timestamp}), "RUNNING metadata mismatch");
}

void test_error_and_recovery_entry() {
    DeviceStateManager manager({}, base);
    reach(manager, DeviceState::running);
    accept(manager, DeviceStateEventType::critical_failure);
    require(manager.query().current == DeviceState::error && manager.query().previous == DeviceState::running,
            "RUNNING -> ERROR failed");
    accept(manager, DeviceStateEventType::recovery_started);
    require(manager.query().current == DeviceState::recovering && manager.query().previous == DeviceState::error,
            "ERROR -> RECOVERING failed");
}

void test_warning_and_recovery_results() {
    DeviceStateManager manager({}, base);
    reach(manager, DeviceState::running);
    accept(manager, DeviceStateEventType::warning);
    require(manager.query().current == DeviceState::warning, "warning entry");
    accept(manager, DeviceStateEventType::issue_recovered);
    require(manager.query().current == DeviceState::running, "warning cleared");
    accept(manager, DeviceStateEventType::warning);
    accept(manager, DeviceStateEventType::failure_escalated);
    require(manager.query().current == DeviceState::error && manager.query().previous == DeviceState::warning,
            "warning escalation");
    accept(manager, DeviceStateEventType::recovery_started);
    accept(manager, DeviceStateEventType::recovery_succeeded);
    require(manager.query().current == DeviceState::running && manager.query().previous == DeviceState::recovering,
            "successful recovery");
    accept(manager, DeviceStateEventType::critical_failure);
    accept(manager, DeviceStateEventType::recovery_started);
    accept(manager, DeviceStateEventType::recovery_failed);
    require(manager.query().current == DeviceState::offline && manager.query().previous == DeviceState::recovering,
            "failed recovery");
}

void test_transition_matrix() {
    struct Expected { DeviceState from; DeviceStateEventType event; DeviceState to; };
    // Independent contract expectations from STATE_TRANSITION.md, not implementation access.
    const std::array<Expected, 9> allowed{{
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
    const std::array<DeviceState, 7> states{{DeviceState::booting, DeviceState::ready, DeviceState::running,
        DeviceState::warning, DeviceState::error, DeviceState::recovering, DeviceState::offline}};
    const std::array<DeviceStateEventType, 9> events{{DeviceStateEventType::runtime_initialized,
        DeviceStateEventType::required_services_ready, DeviceStateEventType::warning,
        DeviceStateEventType::critical_failure, DeviceStateEventType::issue_recovered,
        DeviceStateEventType::failure_escalated, DeviceStateEventType::recovery_started,
        DeviceStateEventType::recovery_succeeded, DeviceStateEventType::recovery_failed}};
    for (const auto state : states) {
        for (const auto event_type : events) {
            std::vector<DeviceStateSnapshot> notifications;
            DeviceStateManager manager([&](const DeviceStateSnapshot& changed) { notifications.push_back(changed); }, base);
            reach(manager, state);
            notifications.clear();
            const auto before = manager.query();
            const auto event = trigger(event_type);
            const Expected* expected = nullptr;
            for (const auto& edge : allowed)
                if (edge.from == state && edge.event == event_type) expected = &edge;
            const auto result = manager.handle(event);
            if (expected) {
                require(result == DeviceTransitionResult::transitioned, "allowed matrix edge rejected");
                require(same_snapshot(manager.query(), {expected->to, state, event.source, event.reason, event.timestamp}),
                        "matrix transition metadata mismatch");
                require(notifications.size() == 1 && same_snapshot(notifications.front(), manager.query()),
                        "accepted edge notification mismatch");
            } else {
                require(result == DeviceTransitionResult::invalid_transition, "undefined matrix edge accepted");
                require(same_snapshot(manager.query(), before) && notifications.empty(), "rejection mutated state");
            }
        }
    }
}

void test_invalid_metadata_and_unknown_event() {
    unsigned notifications = 0;
    DeviceStateManager manager([&](const DeviceStateSnapshot&) { ++notifications; }, base);
    const auto before = manager.query();
    auto event = trigger(DeviceStateEventType::runtime_initialized);
    event.source.clear();
    require(manager.handle(event) == DeviceTransitionResult::invalid_metadata, "missing source accepted");
    event.source = "runtime_manager";
    event.reason.clear();
    require(manager.handle(event) == DeviceTransitionResult::invalid_metadata, "missing reason accepted");
    event.reason = "unknown trigger";
    event.type = static_cast<DeviceStateEventType>(999);
    require(manager.handle(event) == DeviceTransitionResult::invalid_transition, "unknown event accepted");
    require(same_snapshot(manager.query(), before) && notifications == 0, "invalid event changed metadata");
}

void test_explicit_target_matrix() {
    struct Expected { DeviceState from; DeviceStateEventType event; DeviceState to; };
    // Contract oracle: STATE_TRANSITION.md plus AGGREGATION_RULE.md. Exercise
    // the public target-aware API, including every undefined target triple.
    const std::array<Expected, 24> target_allowed{{
        {DeviceState::booting, DeviceStateEventType::runtime_initialized, DeviceState::ready},
        {DeviceState::ready, DeviceStateEventType::required_services_ready, DeviceState::running},
        {DeviceState::running, DeviceStateEventType::warning, DeviceState::warning},
        {DeviceState::running, DeviceStateEventType::critical_failure, DeviceState::error},
        {DeviceState::warning, DeviceStateEventType::issue_recovered, DeviceState::running},
        {DeviceState::warning, DeviceStateEventType::failure_escalated, DeviceState::error},
        {DeviceState::error, DeviceStateEventType::recovery_started, DeviceState::recovering},
        {DeviceState::recovering, DeviceStateEventType::recovery_succeeded, DeviceState::running},
        {DeviceState::recovering, DeviceStateEventType::recovery_failed, DeviceState::offline},
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
    const std::array<DeviceState, 7> states{{DeviceState::booting, DeviceState::ready, DeviceState::running,
        DeviceState::warning, DeviceState::error, DeviceState::recovering, DeviceState::offline}};
    const std::array<DeviceStateEventType, 9> events{{DeviceStateEventType::runtime_initialized,
        DeviceStateEventType::required_services_ready, DeviceStateEventType::warning,
        DeviceStateEventType::critical_failure, DeviceStateEventType::issue_recovered,
        DeviceStateEventType::failure_escalated, DeviceStateEventType::recovery_started,
        DeviceStateEventType::recovery_succeeded, DeviceStateEventType::recovery_failed}};
    unsigned accepted = 0, rejected = 0;
    for (const auto from : states) {
        for (const auto type : events) {
            for (const auto to : states) {
                std::vector<DeviceStateSnapshot> notifications;
                DeviceStateManager manager([&](const DeviceStateSnapshot& changed) {
                    notifications.push_back(changed);
                }, base);
                reach(manager, from);
                notifications.clear();
                const auto before = manager.query();
                auto event = trigger(type);
                event.health_target = to;
                bool allowed = false;
                for (const auto& edge : target_allowed)
                    if (edge.from == from && edge.event == type && edge.to == to) allowed = true;
                const auto result = manager.handle(event);
                if (allowed) {
                    ++accepted;
                    require(result == DeviceTransitionResult::transitioned, "allowed target triple rejected");
                    require(same_snapshot(manager.query(), {to, from, event.source, event.reason, event.timestamp}),
                            "target transition metadata mismatch");
                    require(notifications.size() == 1 && same_snapshot(notifications.front(), manager.query()),
                            "target transition notification mismatch");
                } else {
                    ++rejected;
                    require(result == DeviceTransitionResult::invalid_transition, "undefined target triple accepted");
                    require(same_snapshot(manager.query(), before) && notifications.empty(),
                            "target rejection mutated snapshot or notified");
                }
            }
        }
    }
    require(accepted == 24 && rejected == 417, "explicit target matrix coverage changed");
}

void test_invalid_explicit_target_and_metadata() {
    std::vector<DeviceStateSnapshot> notifications;
    DeviceStateManager manager([&](const DeviceStateSnapshot& state) { notifications.push_back(state); }, base);
    reach(manager, DeviceState::ready);
    notifications.clear();
    const auto before = manager.query();
    auto event = trigger(DeviceStateEventType::warning);
    event.health_target = DeviceState::warning; // Valid only through the target-aware API.
    event.source.clear();
    require(manager.handle(event) == DeviceTransitionResult::invalid_metadata, "target event lacks source");
    event.source = "test producer";
    event.reason.clear();
    require(manager.handle(event) == DeviceTransitionResult::invalid_metadata, "target event lacks reason");
    event.reason = "invalid target";
    event.health_target = static_cast<DeviceState>(999);
    require(manager.handle(event) == DeviceTransitionResult::invalid_transition, "unknown target accepted");
    event.health_target = DeviceState::warning;
    event.type = static_cast<DeviceStateEventType>(999);
    require(manager.handle(event) == DeviceTransitionResult::invalid_transition, "unknown target event accepted");
    require(same_snapshot(manager.query(), before) && notifications.empty(), "invalid target event changed state");
}

void test_notification_can_query() {
    DeviceStateManager* owner = nullptr;
    unsigned notifications = 0;
    DeviceStateManager manager([&](const DeviceStateSnapshot& changed) {
        require(owner != nullptr && same_snapshot(owner->query(), changed), "observer cannot query committed state");
        ++notifications;
    }, base);
    owner = &manager;
    accept(manager, DeviceStateEventType::runtime_initialized);
    accept(manager, DeviceStateEventType::required_services_ready);
    require(notifications == 2, "duplicate or missing notifications");
}

} // namespace

int main() {
    try {
        test_initial_snapshot_and_names();
        test_normal_boot();
        test_error_and_recovery_entry();
        test_warning_and_recovery_results();
        test_transition_matrix();
        test_invalid_metadata_and_unknown_event();
        test_explicit_target_matrix();
        test_invalid_explicit_target_and_metadata();
        test_notification_can_query();
        std::cout << "device state manager tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "device state manager test failed: " << error.what() << '\n';
        return 1;
    }
}
