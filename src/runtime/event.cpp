#include "runtime/event.hpp"

namespace runtime {

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
