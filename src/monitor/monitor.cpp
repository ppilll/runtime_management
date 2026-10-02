#include "runtime/monitor.hpp"
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace runtime {

Monitor::Monitor(Sink sink, std::chrono::seconds interval, ResourceSink resources, ResourceThresholds thresholds)
    : sink_(std::move(sink)), interval_(interval), resources_(std::move(resources)), thresholds_(thresholds) {
    if (!sink_ || interval_ <= std::chrono::seconds::zero())
        throw std::invalid_argument("monitor requires a sink and positive interval");
    auto valid = [](double value) { return std::isfinite(value) && value > 0.0 && value <= 100.0; };
    if (!valid(thresholds_.cpu_warning) || !valid(thresholds_.memory_warning) ||
        !valid(thresholds_.memory_critical) || thresholds_.memory_warning >= thresholds_.memory_critical)
        throw std::invalid_argument("invalid static resource thresholds");
}

void Monitor::report_resources(double cpu_percent, double memory_percent, Clock::time_point at) {
    auto valid = [](double value) { return std::isfinite(value) && value >= 0.0 && value <= 100.0; };
    if (!valid(cpu_percent) || !valid(memory_percent))
        throw std::invalid_argument("resource percentages must be finite and within 0..100");
    if (!resources_) throw std::logic_error("resource fact sink unavailable");
    RuntimeEvent cpu{RuntimeEventType::resource_warning, {}, "cpu_monitor",
                     "CPU usage=" + std::to_string(cpu_percent) + "%", at};
    cpu.active = cpu_percent >= thresholds_.cpu_warning;
    RuntimeEvent memory{RuntimeEventType::resource_warning, {}, "memory_monitor",
                        "memory usage=" + std::to_string(memory_percent) + "%", at};
    memory.active = memory_percent >= thresholds_.memory_warning;
    memory.severity = memory_percent >= thresholds_.memory_critical ? ResourceSeverity::critical
                                                                  : ResourceSeverity::warning;
    // All input validation precedes publication. Sinks queue facts; do not write states.
    resources_(std::move(cpu));
    resources_(std::move(memory));
}

void Monitor::watch(const std::string& name, Clock::time_point now, std::chrono::seconds timeout,
                    std::uint64_t instance_generation) {
    if (timeout <= std::chrono::seconds::zero())
        throw std::invalid_argument("heartbeat timeout must be positive");
    std::lock_guard<std::mutex> lock(mutex_);
    watches_[name] = Watch{now, now + timeout, timeout, 0, instance_generation};
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
            events.back().instance_generation = watch.instance_generation;
        }
    }
    for (auto& event : events) sink_(std::move(event));
}

} // namespace runtime
