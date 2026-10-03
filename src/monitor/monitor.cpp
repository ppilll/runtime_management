#include "runtime/monitor.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace runtime {
namespace {
bool valid_percentage(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

ResourceThresholds resolve_thresholds(ResourceThresholds thresholds) {
    if (!valid_percentage(thresholds.cpu_warning) || thresholds.cpu_warning == 0 ||
        !valid_percentage(thresholds.memory_warning) || thresholds.memory_warning == 0 ||
        !valid_percentage(thresholds.memory_critical) ||
        thresholds.memory_warning >= thresholds.memory_critical)
        throw std::invalid_argument("invalid static resource thresholds");
    if (!thresholds.cpu_clear) thresholds.cpu_clear = std::max(0.0, thresholds.cpu_warning - 5.0);
    if (!thresholds.memory_clear) thresholds.memory_clear = std::max(0.0, thresholds.memory_warning - 5.0);
    if (!thresholds.memory_critical_clear)
        thresholds.memory_critical_clear = std::max(thresholds.memory_warning, thresholds.memory_critical - 5.0);
    if (!valid_percentage(*thresholds.cpu_clear) || *thresholds.cpu_clear >= thresholds.cpu_warning ||
        !valid_percentage(*thresholds.memory_clear) || *thresholds.memory_clear >= thresholds.memory_warning ||
        !valid_percentage(*thresholds.memory_critical_clear) ||
        *thresholds.memory_critical_clear < thresholds.memory_warning ||
        *thresholds.memory_critical_clear >= thresholds.memory_critical)
        throw std::invalid_argument("invalid static resource clear thresholds");
    return thresholds;
}
}

Monitor::Monitor(Sink sink, std::chrono::seconds interval, ResourceSink resources, ResourceThresholds thresholds)
    : sink_(std::move(sink)), interval_(interval), resources_(std::move(resources)),
      thresholds_(resolve_thresholds(thresholds)) {
    if (!sink_ || interval_ <= std::chrono::seconds::zero())
        throw std::invalid_argument("monitor requires a sink and positive interval");
}

void Monitor::report_resources(double cpu_percent, double memory_percent, Clock::time_point at) {
    if (!valid_percentage(cpu_percent) || !valid_percentage(memory_percent))
        throw std::invalid_argument("resource percentages must be finite and within 0..100");
    SystemResourceSnapshot snapshot;
    snapshot.sampled_at = at;
    snapshot.cpu = {MetricQuality::valid, cpu_percent, at, 0};
    snapshot.memory = {MetricQuality::valid, MemoryResourceUsage{0, 0, memory_percent}, at, 0};
    observeResources(snapshot);
}

void Monitor::observeResources(const SystemResourceSnapshot& snapshot) {
    // Malformed typed valid inputs also fail as a whole, without partial publication.
    if ((snapshot.cpu.quality == MetricQuality::valid &&
         (!snapshot.cpu.value || !valid_percentage(*snapshot.cpu.value))) ||
        (snapshot.memory.quality == MetricQuality::valid &&
         (!snapshot.memory.value || !valid_percentage(snapshot.memory.value->used_percent))))
        throw std::invalid_argument("valid resource observation requires a finite percentage within 0..100");
    if (!resources_) throw std::logic_error("resource fact sink unavailable");
    std::vector<RuntimeEvent> facts;
    {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        auto changed = [&](std::optional<Pressure>& previous, Pressure next, const char* source,
                           double value, double activate, double clear, const std::string& extra) {
            if (previous && *previous == next) return;
            RuntimeEvent fact{RuntimeEventType::resource_warning, {}, source,
                std::string(source) + " usage=" + std::to_string(value) + "% activate=" + std::to_string(activate) +
                " clear=" + std::to_string(clear) + extra + " action=" +
                (next == Pressure::normal ? "clear" : next == Pressure::critical ? "critical" : "warning"),
                snapshot.sampled_at};
            fact.active = next != Pressure::normal;
            fact.severity = next == Pressure::critical ? ResourceSeverity::critical : ResourceSeverity::warning;
            facts.push_back(std::move(fact));
            previous = next;
        };
        if (snapshot.cpu.quality == MetricQuality::valid) {
            const double value = *snapshot.cpu.value;
            const auto next = cpu_pressure_ == Pressure::warning
                ? (value <= *thresholds_.cpu_clear ? Pressure::normal : Pressure::warning)
                : (value >= thresholds_.cpu_warning ? Pressure::warning : Pressure::normal);
            changed(cpu_pressure_, next, "cpu_monitor", value, thresholds_.cpu_warning, *thresholds_.cpu_clear, {});
        }
        if (snapshot.memory.quality == MetricQuality::valid) {
            const double value = snapshot.memory.value->used_percent;
            Pressure next;
            if (value <= *thresholds_.memory_clear) next = Pressure::normal;
            else if (memory_pressure_ == Pressure::critical)
                next = value <= *thresholds_.memory_critical_clear ? Pressure::warning : Pressure::critical;
            else if (value >= thresholds_.memory_critical) next = Pressure::critical;
            else if (memory_pressure_ == Pressure::warning || value >= thresholds_.memory_warning)
                next = Pressure::warning;
            else next = Pressure::normal;
            changed(memory_pressure_, next, "memory_monitor", value, thresholds_.memory_warning,
                    *thresholds_.memory_clear, " critical=" + std::to_string(thresholds_.memory_critical) +
                    " critical_clear=" + std::to_string(*thresholds_.memory_critical_clear));
        }
    }
    // No heartbeat or policy mutex is held while calling the queue sink.
    for (auto& fact : facts) resources_(std::move(fact));
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
