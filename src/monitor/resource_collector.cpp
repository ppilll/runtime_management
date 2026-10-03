#include "runtime/resource_collector.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace runtime {
namespace {

bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string_view token(std::string_view& remaining) {
    while (!remaining.empty() && whitespace(remaining.front())) remaining.remove_prefix(1);
    std::size_t end = 0;
    while (end < remaining.size() && !whitespace(remaining[end])) ++end;
    const auto result = remaining.substr(0, end);
    remaining.remove_prefix(end);
    return result;
}

std::string_view line(std::string_view& remaining) {
    const auto end = remaining.find('\n');
    if (end == std::string_view::npos) {
        const auto result = remaining;
        remaining = {};
        return result;
    }
    const auto result = remaining.substr(0, end);
    remaining.remove_prefix(end + 1);
    return result;
}

bool unsignedNumber(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool checkedAdd(std::uint64_t value, std::uint64_t& sum) {
    if (value > std::numeric_limits<std::uint64_t>::max() - sum) return false;
    sum += value;
    return true;
}

template <typename Value>
void unavailable(MetricObservation<Value>& observation) {
    observation.quality = MetricQuality::unavailable;
    observation.value.reset();
    if (observation.consecutive_errors < std::numeric_limits<std::uint64_t>::max())
        ++observation.consecutive_errors;
    // last_success_at deliberately survives errors, without retaining old values.
}

ProcReadError classifyReadError(int error) {
    if (error == ENOENT || error == ESRCH) return ProcReadError::not_present;
    if (error == EACCES || error == EPERM) return ProcReadError::permission_denied;
    return ProcReadError::io_error;
}

// Bounded read-only text reader for system files and captured direct child PIDs.
ProcReadResult readProcText(const std::string& path, std::size_t limit) {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) return {{}, classifyReadError(errno)};
    std::string text;
    std::array<char, 4096> buffer{};
    for (;;) {
        // One additional byte detects oversize even when length equals the cap.
        const auto amount = std::min(buffer.size(), limit - text.size() + 1);
        errno = 0;
        const auto count = std::fread(buffer.data(), 1, amount, file.get());
        if (std::ferror(file.get())) return {{}, classifyReadError(errno)};
        if (count > limit - text.size()) return {{}, ProcReadError::too_large};
        text.append(buffer.data(), count);
        if (std::feof(file.get())) return {std::move(text), ProcReadError::none};
        if (count == 0) return {{}, ProcReadError::io_error};
    }
}

bool usable(const ProcReadResult& result) {
    return result.error == ProcReadError::none && result.text.size() <= ResourceCollector::system_file_limit;
}

} // namespace

ResourceCollector::ResourceCollector(Reader reader, Now now, std::optional<ProcessPlatform> platform)
    : reader_(reader ? std::move(reader) : Reader{readProcText}),
      now_(now ? std::move(now) : Now{[] { return ResourceClock::now(); }}),
      platform_(platform ? *platform : ProcessPlatform{::sysconf(_SC_CLK_TCK), ::sysconf(_SC_PAGESIZE)}) {}

std::optional<ResourceCollector::ProcessStat> ResourceCollector::parseProcess(
        const std::string& text, int expected_pid) const {
    std::string_view fields{text};
    std::uint64_t pid = 0;
    if (!unsignedNumber(token(fields), pid) || expected_pid <= 0 ||
        pid != static_cast<std::uint64_t>(expected_pid)) return std::nullopt;
    while (!fields.empty() && whitespace(fields.front())) fields.remove_prefix(1);
    if (fields.empty() || fields.front() != '(') return std::nullopt;
    const auto closing = fields.rfind(')');
    if (closing == std::string_view::npos || closing + 1 >= fields.size() ||
        !whitespace(fields[closing + 1])) return std::nullopt;
    fields.remove_prefix(closing + 1); // comm can contain spaces and right parentheses.
    const auto state = token(fields);
    if (state.size() != 1 || !((state.front() >= 'A' && state.front() <= 'Z') ||
                             (state.front() >= 'a' && state.front() <= 'z'))) return std::nullopt;
    ProcessStat result;
    result.state = state.front();
    std::uint64_t user = 0, system = 0, pages = 0;
    for (unsigned field = 4; field <= 24; ++field) {
        const auto value = token(fields);
        if (value.empty()) return std::nullopt;
        // Intervening fields are integers too (e.g. tpgid/nice may be signed).
        auto digits = value;
        if (digits.front() == '-') digits.remove_prefix(1);
        std::uint64_t ignored = 0;
        if (!unsignedNumber(digits, ignored)) return std::nullopt;
        if (field == 14 && !unsignedNumber(value, user)) return std::nullopt;
        if (field == 15 && !unsignedNumber(value, system)) return std::nullopt;
        if (field == 22 && !unsignedNumber(value, result.starttime)) return std::nullopt;
        if (field == 24 && (!unsignedNumber(value, pages) ||
            pages > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) return std::nullopt;
    }
    result.cpu_ticks = user;
    if (!checkedAdd(system, result.cpu_ticks) || platform_.page_size_bytes <= 0 ||
        pages > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(platform_.page_size_bytes))
        return std::nullopt;
    result.rss_bytes = pages * static_cast<std::uint64_t>(platform_.page_size_bytes);
    return result;
}

ProcessResourceScan ResourceCollector::collectProcesses(const Identities& captured,
        const ValidateIdentities& validate, std::chrono::seconds interval, Continue keep_running) {
    if (interval.count() < 1 || interval.count() > 60)
        throw std::invalid_argument("process sample interval must be between 1 and 60 seconds");
    if (!validate) throw std::invalid_argument("process identity validator required");
    if (!captured || platform_.clock_ticks_per_second <= 0 || platform_.page_size_bytes <= 0 ||
        (keep_running && !keep_running())) return {};
    // Build a new eligible-only cache, and commit once after owner validation.
    std::unordered_map<std::string, ProcessPrevious> pending;
    pending.reserve(captured->size());
    ProcessResourceScan scan;
    scan.processes.reserve(captured->size());
    for (const auto& identity : *captured) {
        if (identity.service_name.empty() || identity.pid <= 0 || identity.instance_generation == 0 ||
            !pending.emplace(identity.service_name, ProcessPrevious{}).second) return {};
    }
    for (const auto& identity : *captured) {
        if (keep_running && !keep_running()) return {};
        auto& previous = pending.at(identity.service_name);
        const auto old = previous_processes_.find(identity.service_name);
        if (old != previous_processes_.end() && old->second.pid == identity.pid &&
            old->second.generation == identity.instance_generation) previous = old->second;
        previous.pid = identity.pid;
        previous.generation = identity.instance_generation;
        ProcessResourceSnapshot row;
        row.service_name = identity.service_name;
        row.pid = identity.pid;
        row.instance_generation = identity.instance_generation;
        std::optional<ProcessStat> stat;
        ProcReadError error = ProcReadError::io_error;
        const auto read = reader_("/proc/" + std::to_string(identity.pid) + "/stat", process_file_limit);
        error = read.error;
        if (error == ProcReadError::none && read.text.size() <= process_file_limit)
            stat = parseProcess(read.text, identity.pid);
        row.sampled_at = now_(); // Actual per-row completion, never producer tick time.
        if (!stat) {
            row.observation_status = error == ProcReadError::not_present ?
                ProcessObservationStatus::not_present : ProcessObservationStatus::unavailable;
            previous.cpu_ticks.reset();
        } else if (previous.starttime && *previous.starttime != stat->starttime) {
            row.observation_status = ProcessObservationStatus::identity_changed;
            previous.cpu_ticks.reset(); // Retain anchor, never admit replacement in this launch.
        } else {
            previous.starttime = stat->starttime;
            row.proc_start_time_ticks = stat->starttime;
            if (stat->state == 'Z') {
                row.observation_status = ProcessObservationStatus::zombie;
                previous.cpu_ticks.reset();
            } else {
                row.observation_status = ProcessObservationStatus::observed;
                row.rss_bytes = stat->rss_bytes;
                row.cpu_quality = MetricQuality::warming_up;
                if (previous.cpu_ticks) {
                    const double elapsed = std::chrono::duration<double>(row.sampled_at - previous.sampled_at).count();
                    if (elapsed <= 0 || stat->cpu_ticks < *previous.cpu_ticks) {
                        row.cpu_quality = MetricQuality::unavailable;
                    } else if (elapsed <= 3.0 * static_cast<double>(interval.count())) {
                        const double percent = (static_cast<double>(stat->cpu_ticks - *previous.cpu_ticks) /
                            static_cast<double>(platform_.clock_ticks_per_second)) / elapsed * 100.0;
                        if (std::isfinite(percent) && percent >= 0) {
                            row.cpu_quality = MetricQuality::valid;
                            row.cpu_percent = percent; // Multi-thread CPU can exceed 100%.
                        } else row.cpu_quality = MetricQuality::unavailable;
                    }
                }
                previous.cpu_ticks = stat->cpu_ticks;
                previous.sampled_at = row.sampled_at;
            }
        }
        scan.processes.push_back(std::move(row));
    }
    if (keep_running && !keep_running()) return {};
    const auto validated = validate(*captured);
    if (!validated || validated->size() != captured->size()) return {};
    // O(N) identity verification; do not rely on return ordering or count alone.
    std::unordered_map<std::string, ProcessIdentity> matches;
    matches.reserve(validated->size());
    for (const auto& identity : *validated)
        if (!matches.emplace(identity.service_name, identity).second) return {};
    for (const auto& identity : *captured) {
        const auto match = matches.find(identity.service_name);
        if (match == matches.end() || match->second.pid != identity.pid ||
            match->second.instance_generation != identity.instance_generation) return {};
    }
    if (keep_running && !keep_running()) return {};
    std::sort(scan.processes.begin(), scan.processes.end(), [](const auto& left, const auto& right) {
        return left.service_name < right.service_name;
    });
    previous_processes_ = std::move(pending); // Retired PID/token caches are evicted here.
    scan.quality = MetricQuality::valid;
    return scan;
}

std::optional<ResourceCollector::CpuCounters> ResourceCollector::parseCpu(const std::string& text) {
    std::optional<CpuCounters> found;
    std::string_view remaining{text};
    while (!remaining.empty()) {
        auto fields = line(remaining);
        if (token(fields) != "cpu") continue; // Never substitute a cpuN row.
        if (found) return std::nullopt;
        CpuCounters counters;
        for (std::size_t i = 0; i < counters.components.size(); ++i) {
            const auto field = token(fields);
            if (field.empty()) break;
            if (!unsignedNumber(field, counters.components[i]) ||
                !checkedAdd(counters.components[i], counters.total)) return std::nullopt;
            ++counters.field_count;
        }
        if (counters.field_count < 4) return std::nullopt;
        counters.idle_all = counters.components[3];
        if (!checkedAdd(counters.components[4], counters.idle_all)) return std::nullopt;
        // guest and guest_nice are already included in user/nice. Ignore tails.
        found = counters;
    }
    return found;
}

std::optional<MemoryResourceUsage> ResourceCollector::parseMemory(const std::string& text) {
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> available;
    std::string_view remaining{text};
    while (!remaining.empty()) {
        auto fields = line(remaining);
        const auto name = token(fields);
        if (name != "MemTotal:" && name != "MemAvailable:") continue;
        auto& destination = name == "MemTotal:" ? total : available;
        std::uint64_t kib = 0;
        if (destination || !unsignedNumber(token(fields), kib) || token(fields) != "kB" ||
            !token(fields).empty() || kib > std::numeric_limits<std::uint64_t>::max() / 1024)
            return std::nullopt;
        destination = kib * 1024;
    }
    if (!total || !available || *total == 0 || *available > *total) return std::nullopt;
    const double used = (static_cast<double>(*total - *available) / static_cast<double>(*total)) * 100.0;
    if (!std::isfinite(used) || used < 0.0 || used > 100.0) return std::nullopt;
    return MemoryResourceUsage{*total, *available, used};
}

void ResourceCollector::observeCpu(const std::optional<CpuCounters>& counters, ResourceClock::time_point at) {
    if (!counters) {
        previous_cpu_.reset(); // Read/parse failure requires a fresh warm-up.
        unavailable(cpu_);
        return;
    }
    if (!previous_cpu_) {
        previous_cpu_ = counters;
        cpu_.quality = MetricQuality::warming_up;
        cpu_.value.reset();
        cpu_.consecutive_errors = 0;
        return; // A baseline is not a successful percentage measurement.
    }
    const auto previous = *previous_cpu_;
    previous_cpu_ = counters; // Complete valid counters always rebaseline.
    if (counters->field_count != previous.field_count) {
        unavailable(cpu_);
        return;
    }
    for (std::size_t i = 0; i < counters->field_count; ++i) {
        if (counters->components[i] < previous.components[i]) {
            unavailable(cpu_);
            return;
        }
    }
    const auto total_delta = counters->total - previous.total;
    const auto idle_delta = counters->idle_all - previous.idle_all;
    if (total_delta == 0 || idle_delta > total_delta) {
        unavailable(cpu_);
        return;
    }
    const double busy = (static_cast<double>(total_delta - idle_delta) / static_cast<double>(total_delta)) * 100.0;
    if (!std::isfinite(busy) || busy < 0.0 || busy > 100.0) {
        unavailable(cpu_);
        return;
    }
    cpu_.quality = MetricQuality::valid;
    cpu_.value = busy;
    cpu_.last_success_at = at;
    cpu_.consecutive_errors = 0;
}

void ResourceCollector::observeMemory(const std::optional<MemoryResourceUsage>& memory, ResourceClock::time_point at) {
    if (!memory) {
        unavailable(memory_);
        return;
    }
    memory_.quality = MetricQuality::valid;
    memory_.value = memory;
    memory_.last_success_at = at;
    memory_.consecutive_errors = 0;
}

SystemResourceSnapshot ResourceCollector::collectSystemImpl(std::optional<ResourceClock::time_point> completed_at) {
    const auto stat = reader_("/proc/stat", system_file_limit);
    const auto meminfo = reader_("/proc/meminfo", system_file_limit);
    const auto counters = usable(stat) ? parseCpu(stat.text) : std::nullopt;
    const auto memory = usable(meminfo) ? parseMemory(meminfo.text) : std::nullopt;
    const auto at = completed_at ? *completed_at : now_();
    observeCpu(counters, at);
    observeMemory(memory, at);
    return {at, cpu_, memory_};
}

SystemResourceSnapshot ResourceCollector::collectSystem() {
    return collectSystemImpl(std::nullopt);
}

SystemResourceSnapshot ResourceCollector::collectSystem(ResourceClock::time_point completed_at) {
    return collectSystemImpl(completed_at);
}

} // namespace runtime
