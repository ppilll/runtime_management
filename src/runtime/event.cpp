#include "runtime/event.hpp"
#include <stdexcept>
#include <utility>

namespace runtime {

const char* event_name(RuntimeEventType type) {
    switch (type) {
    case RuntimeEventType::service_started: return "SERVICE_STARTED";
    case RuntimeEventType::service_failed: return "SERVICE_FAILED";
    case RuntimeEventType::service_stopped: return "SERVICE_STOPPED";
    case RuntimeEventType::heartbeat_timeout: return "HEARTBEAT_TIMEOUT";
    case RuntimeEventType::resource_warning: return "RESOURCE_WARNING";
    case RuntimeEventType::recovery_start: return "RECOVERY_START";
    case RuntimeEventType::recovery_success: return "RECOVERY_SUCCESS";
    case RuntimeEventType::recovery_failed: return "RECOVERY_FAILED";
    }
    return "UNKNOWN";
}

void EventDispatcher::subscribe(Sink sink) {
    if (!sink || dispatching_) throw std::logic_error("invalid event subscription");
    subscribers_.push_back(std::move(sink));
}

void EventDispatcher::publish(RuntimeEvent event) { pending_.push(std::move(event)); }

void EventDispatcher::drain() {
    if (dispatching_) return;
    dispatching_ = true;
    try {
        while (!pending_.empty()) {
            auto event = std::move(pending_.front());
            pending_.pop();
            for (const auto& subscriber : subscribers_) subscriber(event);
        }
    } catch (...) {
        dispatching_ = false;
        throw;
    }
    dispatching_ = false;
}

void EventQueue::push(Event event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push(std::move(event));
    }
    ready_.notify_one();
}

bool EventQueue::pop_for(Event& event, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [this] { return !events_.empty(); })) return false;
    event = std::move(events_.front());
    events_.pop();
    return true;
}

} // namespace runtime
