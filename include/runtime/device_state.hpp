#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace runtime {

// Device health and service lifecycle are separate models.
enum class DeviceState { booting, ready, running, warning, error, recovering, offline };
const char* state_name(DeviceState state);

enum class DeviceStateEventType {
    runtime_initialized,
    required_services_ready,
    warning,
    critical_failure,
    issue_recovered,
    failure_escalated,
    recovery_started,
    recovery_succeeded,
    recovery_failed
};

// Same monotonic clock as runtime::Clock; not a wall-clock/UTC timestamp.
using DeviceStateTimestamp = std::chrono::steady_clock::time_point;

struct DeviceStateEvent {
    DeviceStateEventType type;
    std::string source;
    std::string reason;
    DeviceStateTimestamp timestamp = std::chrono::steady_clock::now();
    // Only the aggregation adapter supplies a target. The manager still checks
    // explicit state/event/target edges; this is not an unrestricted state setter.
    std::optional<DeviceState> health_target;
};

struct DeviceStateSnapshot {
    DeviceState current = DeviceState::booting;
    DeviceState previous = DeviceState::booting;
    std::string source = "runtime_manager";
    std::string reason = "runtime starting";
    DeviceStateTimestamp timestamp = std::chrono::steady_clock::now();
};

enum class DeviceTransitionResult { transitioned, invalid_transition, invalid_metadata };

} // namespace runtime
