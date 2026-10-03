#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace runtime {

// Measurement types deliberately have no event, state or recovery dependencies.
using ResourceClock = std::chrono::steady_clock;

enum class MetricQuality { valid, warming_up, unavailable };

template <typename Value>
struct MetricObservation {
    MetricQuality quality = MetricQuality::unavailable;
    std::optional<Value> value;
    std::optional<ResourceClock::time_point> last_success_at;
    std::uint64_t consecutive_errors = 0;
};

struct MemoryResourceUsage {
    std::uint64_t total_bytes = 0;
    std::uint64_t available_bytes = 0;
    double used_percent = 0.0;
};

struct SystemResourceSnapshot {
    ResourceClock::time_point sampled_at{};
    MetricObservation<double> cpu; // Aggregate busy percent, within 0..100.
    MetricObservation<MemoryResourceUsage> memory;
};

// SM supplies this identity; instance_generation is launched_generation.
// No lifecycle status is inferred from resource observations.
struct ProcessIdentity {
    std::string service_name;
    int pid = -1;
    std::uint64_t instance_generation = 0;
};

enum class ProcessObservationStatus {
    observed, not_present, zombie, identity_changed, unavailable
};

struct ProcessResourceSnapshot {
    std::string service_name;
    int pid = -1;
    std::uint64_t instance_generation = 0;
    std::optional<std::uint64_t> proc_start_time_ticks;
    ResourceClock::time_point sampled_at{};
    ProcessObservationStatus observation_status = ProcessObservationStatus::unavailable;
    MetricQuality cpu_quality = MetricQuality::unavailable;
    std::optional<double> cpu_percent; // Single-core equivalent, may exceed 100.
    std::optional<std::uint64_t> rss_bytes; // Direct child's RSS only.
};

struct ResourceSnapshotBundle {
    SystemResourceSnapshot system;
    std::vector<ProcessResourceSnapshot> processes; // Ordered by service_name.
    ResourceClock::time_point collected_at{};
    MetricQuality process_scan_quality = MetricQuality::unavailable;
};

struct ProcessResourceScan {
    MetricQuality quality = MetricQuality::unavailable;
    std::vector<ProcessResourceSnapshot> processes;
};

// Age/stale are derived by the future query layer, not a collector state machine.
} // namespace runtime
