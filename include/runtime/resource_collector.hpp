#pragma once

#include "runtime/resource_snapshot.hpp"
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace runtime {

enum class ProcReadError { none, not_present, permission_denied, io_error, too_large };

struct ProcReadResult {
    std::string text;
    ProcReadError error = ProcReadError::none;
};

struct ProcessPlatform {
    long clock_ticks_per_second = 0;
    long page_size_bytes = 0;
};

// Single-caller measurement object. No worker, policy, publication or proc writes.
class ResourceCollector {
public:
    // Expected read failures are results. Unexpected exceptions propagate to the
    // existing worker's shutdown boundary; callers must not silently mark good.
    using Reader = std::function<ProcReadResult(const std::string&, std::size_t)>;
    using Now = std::function<ResourceClock::time_point()>;
    using Identities = std::optional<std::vector<ProcessIdentity>>;
    using ValidateIdentities = std::function<Identities(const std::vector<ProcessIdentity>&)>;
    using Continue = std::function<bool()>;
    static constexpr std::size_t system_file_limit = 64 * 1024;
    static constexpr std::size_t process_file_limit = 4 * 1024;

    // Without a platform seam, reads HZ/pagesize once at construction.
    explicit ResourceCollector(Reader reader = {}, Now now = {}, std::optional<ProcessPlatform> platform = {});
    // Production clock is consulted after reading/parsing both fixed files.
    SystemResourceSnapshot collectSystem();
    // Test/caller seam: completed_at must represent actual collection completion.
    SystemResourceSnapshot collectSystem(ResourceClock::time_point completed_at);
    // SM capture and revalidation are separate, short try-lock calls. Reader is
    // used between them, outside the registry lock. Failed validation/gate
    // discards all rows and pending cache changes; unavailable capture does no I/O.
    ProcessResourceScan collectProcesses(const Identities& captured, const ValidateIdentities& validate,
        std::chrono::seconds interval = std::chrono::seconds{2}, Continue keep_running = {});

private:
    struct CpuCounters {
        std::array<std::uint64_t, 8> components{};
        std::size_t field_count = 0;
        std::uint64_t total = 0;
        std::uint64_t idle_all = 0;
    };
    struct ProcessStat {
        char state = '\0';
        std::uint64_t cpu_ticks = 0;
        std::uint64_t starttime = 0;
        std::uint64_t rss_bytes = 0;
    };
    struct ProcessPrevious {
        int pid = -1;
        std::uint64_t generation = 0;
        // Anchor survives read errors, zombie and repeated starttime mismatch.
        std::optional<std::uint64_t> starttime;
        std::optional<std::uint64_t> cpu_ticks;
        ResourceClock::time_point sampled_at{};
    };

    static std::optional<CpuCounters> parseCpu(const std::string& text);
    std::optional<ProcessStat> parseProcess(const std::string& text, int expected_pid) const;
    static std::optional<MemoryResourceUsage> parseMemory(const std::string& text);
    SystemResourceSnapshot collectSystemImpl(std::optional<ResourceClock::time_point> completed_at);
    void observeCpu(const std::optional<CpuCounters>& counters, ResourceClock::time_point at);
    void observeMemory(const std::optional<MemoryResourceUsage>& memory, ResourceClock::time_point at);

    Reader reader_;
    Now now_;
    std::optional<CpuCounters> previous_cpu_;
    MetricObservation<double> cpu_;
    MetricObservation<MemoryResourceUsage> memory_;
    ProcessPlatform platform_;
    std::unordered_map<std::string, ProcessPrevious> previous_processes_;
};

} // namespace runtime
