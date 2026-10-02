#pragma once

#include "runtime/device_state.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace runtime {

using Clock = std::chrono::steady_clock;

// Internal facts, separate from service commands and IPC messages.
enum class RuntimeEventType {
    service_started, service_failed, service_stopped, heartbeat_timeout,
    resource_warning, recovery_start, recovery_success, recovery_failed
};
const char* event_name(RuntimeEventType type);

enum class ResourceSeverity { warning, critical };

struct RuntimeEvent {
    RuntimeEventType type;
    std::string service_name; // Empty only for device-wide resource/recovery events.
    std::string source;
    std::string reason;
    Clock::time_point at = Clock::now();
    bool active = true; // RESOURCE_WARNING(false) clears this source's warning.
    ResourceSeverity severity = ResourceSeverity::warning;
    // Required for named service facts. Assigned by the lifecycle owner, not
    // inferred from timestamps; recovery results must match the current fault.
    std::optional<std::uint64_t> generation;
};

enum class EventType { start, stop, heartbeat, health_check, health_missed, process_exited, shutdown, device_state, runtime_event };

struct Event {
    EventType type;
    std::string service_name;
    Clock::time_point at = Clock::now();
    int pid = -1;
    int exit_status = 0;
    unsigned missed_count = 0;
    // Internal device trigger envelope; existing service event fields retain their order.
    std::optional<DeviceStateEvent> device_state_event;
    std::optional<RuntimeEvent> runtime_event;
};

// Single event-loop writer. External threads use EventQueue, never this object.
// Subscriptions are installed before dispatch. Nested publications are appended
// and delivered after all subscribers of the current event, in registration order.
class EventDispatcher {
public:
    using Sink = std::function<void(const RuntimeEvent&)>;
    void subscribe(Sink sink);
    void publish(RuntimeEvent event);
    void drain();

private:
    std::vector<Sink> subscribers_;
    std::queue<RuntimeEvent> pending_;
    bool dispatching_ = false;
};

class EventQueue {
public:
    void push(Event event);
    bool pop_for(Event& event, std::chrono::milliseconds timeout);

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<Event> events_;
};

} // namespace runtime
