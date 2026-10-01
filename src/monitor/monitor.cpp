#include "runtime/monitor.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

namespace runtime {

Monitor::Monitor(Sink sink, std::chrono::seconds interval)
    : sink_(std::move(sink)), interval_(interval) {
    if (!sink_ || interval_ <= std::chrono::seconds::zero())
        throw std::invalid_argument("monitor requires a sink and positive interval");
}

void Monitor::watch(const std::string& name, Clock::time_point now, std::chrono::seconds timeout) {
    if (timeout <= std::chrono::seconds::zero())
        throw std::invalid_argument("heartbeat timeout must be positive");
    std::lock_guard<std::mutex> lock(mutex_);
    watches_[name] = Watch{now, now + timeout, timeout, 0};
}

void Monitor::unwatch(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    watches_.erase(name);
}

void Monitor::heartbeat(const std::string& name, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = watches_.find(name);
    if (it == watches_.end() || now < it->second.last) return;
    it->second.last = now;
    it->second.next_miss = now + it->second.timeout;
    it->second.misses = 0;
}

void Monitor::check(Clock::time_point now) {
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, watch] : watches_) {
            if (now < watch.next_miss) continue;
            const auto elapsed = now - watch.next_miss;
            const auto periods = static_cast<unsigned>(elapsed / interval_) + 1;
            watch.misses += periods;
            watch.next_miss += interval_ * periods;
            events.push_back(Event{EventType::health_missed, name, now, -1, 0, watch.misses});
        }
    }
    for (auto& event : events) sink_(std::move(event));
}

} // namespace runtime
