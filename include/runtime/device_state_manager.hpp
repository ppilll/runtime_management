#pragma once

#include "runtime/device_state.hpp"
#include <functional>
#include <mutex>

namespace runtime {

class DeviceStateManager {
public:
    using StateChangeSink = std::function<void(const DeviceStateSnapshot&)>;

    explicit DeviceStateManager(StateChangeSink state_changes = {},
                               DeviceStateTimestamp started_at = std::chrono::steady_clock::now());
    // Invalid events leave all metadata unchanged and do not notify observers.
    // Accepted events preserve the producer's timestamp (including equal times).
    // An optional health_target is checked against the explicit aggregate edges;
    // without it, only the original nine Thread 1 edges are accepted.
    DeviceTransitionResult handle(const DeviceStateEvent& event);
    // Returns an independent, consistent copy, safe for concurrent readers.
    DeviceStateSnapshot query() const;

private:
    // The callback runs after releasing mutex_ and may query this manager.
    // Writers must use the runtime event loop for ordered notification delivery.
    // Callback exceptions propagate after commit; observers must not throw.
    const StateChangeSink state_changes_;
    mutable std::mutex mutex_;
    DeviceStateSnapshot state_;
};

} // namespace runtime
