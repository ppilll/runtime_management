#pragma once

#include "runtime/event.hpp"
#include "runtime/resource_snapshot.hpp"
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime {

struct ResourceThresholds {
    double cpu_warning = 80.0;
    double memory_warning = 80.0;
    double memory_critical = 95.0;
    std::optional<double> cpu_clear;
    std::optional<double> memory_clear;
    std::optional<double> memory_critical_clear;
};

class Monitor {
public:
    using Sink = std::function<void(Event)>;
    using ResourceSink = std::function<void(RuntimeEvent)>;
    explicit Monitor(Sink sink, std::chrono::seconds interval = std::chrono::seconds{5},
                     ResourceSink resources = {}, ResourceThresholds thresholds = {});
    void watch(const std::string& name, Clock::time_point now, std::chrono::seconds timeout,
               std::uint64_t instance_generation = 0);
    void unwatch(const std::string& name);
    void heartbeat(const std::string& name, Clock::time_point now);
    void check(Clock::time_point now);
    // Both inputs are validated before advancing the shared resource policy.
    void report_resources(double cpu_percent, double memory_percent, Clock::time_point at = Clock::now());
    void observeResources(const SystemResourceSnapshot& snapshot);

private:
    struct Watch {
        Clock::time_point last;
        Clock::time_point next_miss;
        std::chrono::seconds timeout;
        unsigned misses = 0;
        std::uint64_t instance_generation = 0;
    };
    Sink sink_;
    std::chrono::seconds interval_;
    const ResourceSink resources_;
    const ResourceThresholds thresholds_;
    enum class Pressure { normal, warning, critical };
    std::mutex resource_mutex_;
    std::optional<Pressure> cpu_pressure_;
    std::optional<Pressure> memory_pressure_;
    std::mutex mutex_;
    std::unordered_map<std::string, Watch> watches_;
};

} // namespace runtime
