#include "runtime/resource_collector.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

using namespace runtime;
using namespace std::chrono_literals;

namespace {

const auto start = ResourceClock::time_point{} + 10s;
const std::string first_cpu = "cpu 300 100 150 300 100 20 20 10\ncpu0 9 9 9 9\nintr 99\n";
const std::string second_cpu = "cpu 400 100 180 340 110 30 30 10\ncpu0 0 0 0 0\n";
const std::string good_memory = "MemTotal: 1000 kB\nMemFree: 1 kB\nMemAvailable: 200 kB\nCached: 999 kB\n";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_percent(const MetricObservation<double>& observation, double expected) {
    require(observation.quality == MetricQuality::valid && observation.value.has_value(), "CPU percentage absent");
    require(std::isfinite(*observation.value) && std::abs(*observation.value - expected) < 1e-9,
            "CPU percentage differs from expected aggregate delta");
}

void expect_unavailable(const MetricObservation<double>& observation) {
    require(observation.quality == MetricQuality::unavailable && !observation.value, "invalid CPU reported a value");
}

struct Fixture {
    ProcReadResult stat{first_cpu, ProcReadError::none};
    ProcReadResult meminfo{good_memory, ProcReadError::none};
    std::vector<std::string> reads;
    ResourceCollector collector;

    Fixture() : collector([this](const std::string& path, std::size_t limit) {
        require(limit == 64 * 1024, "system reader limit changed");
        reads.push_back(path);
        if (path == "/proc/stat") return stat;
        if (path == "/proc/meminfo") return meminfo;
        throw std::runtime_error("unexpected proc path");
    }) {}
};

void test_first_sample_and_aggregate_delta() {
    Fixture fixture;
    const auto first = fixture.collector.collectSystem(start);
    require(first.sampled_at == start && first.cpu.quality == MetricQuality::warming_up && !first.cpu.value,
            "first CPU counters must warm up without a fake zero");
    require(!first.cpu.last_success_at && first.cpu.consecutive_errors == 0, "baseline is not a successful CPU measurement");
    require(first.memory.quality == MetricQuality::valid && first.memory.value &&
            first.memory.value->total_bytes == 1024000 && first.memory.value->available_bytes == 204800 &&
            first.memory.value->used_percent == 80.0 && first.memory.last_success_at == start,
            "memory must be independently valid during CPU warm-up");
    fixture.stat.text = second_cpu;
    const auto second = fixture.collector.collectSystem(start + 2s);
    expect_percent(second.cpu, 75.0);
    require(second.cpu.last_success_at == start + 2s && second.cpu.consecutive_errors == 0,
            "successful CPU timestamp/error count");
    require(fixture.reads == std::vector<std::string>{"/proc/stat", "/proc/meminfo", "/proc/stat", "/proc/meminfo"},
            "collectSystem must only read two fixed files per sample");
}

void test_cpu_optional_fields_and_guest_exclusion() {
    for (std::size_t fields = 4; fields <= 8; ++fields) {
        Fixture fixture;
        const std::vector<std::string> before{"100", "20", "30", "400", "100", "10", "10", "10"};
        const std::vector<std::string> after{"150", "20", "30", "450", "150", "20", "20", "20"};
        std::string first = "cpu", second = "cpu";
        double total_delta = 100.0, idle_delta = 50.0;
        for (std::size_t i = 0; i < fields; ++i) {
            first += " " + before[i];
            second += " " + after[i];
            if (i == 4) { total_delta += 50.0; idle_delta += 50.0; }
            if (i >= 5) total_delta += 10.0;
        }
        fixture.stat.text = first + "\n";
        require(fixture.collector.collectSystem(start).cpu.quality == MetricQuality::warming_up, "optional CPU fields rejected");
        fixture.stat.text = second + "\n";
        expect_percent(fixture.collector.collectSystem(start + 2s).cpu, 100.0 * (total_delta - idle_delta) / total_delta);
    }
    Fixture fixture;
    fixture.stat.text = "cpu 300 100 150 300 100 20 20 10 0 0\n";
    fixture.collector.collectSystem(start);
    // Guest tails must not enter the eight-counter sum, validation or shape.
    fixture.stat.text = "cpu 400 100 180 340 110 30 30 10 18446744073709551615 ignored future\n";
    expect_percent(fixture.collector.collectSystem(start + 2s).cpu, 75.0);
}

void test_cpu_zero_full_and_large_valid_delta() {
    Fixture fixture;
    fixture.stat.text = "cpu 100 0 0 1000\n";
    fixture.collector.collectSystem(start);
    fixture.stat.text = "cpu 100 0 0 1100\n";
    expect_percent(fixture.collector.collectSystem(start + 2s).cpu, 0.0);
    fixture.stat.text = "cpu 2000 0 0 1100\n";
    expect_percent(fixture.collector.collectSystem(start + 4s).cpu, 100.0);
    Fixture large;
    large.stat.text = "cpu 0 0 0 1\n";
    large.collector.collectSystem(start);
    large.stat.text = "cpu 18446744073709551614 0 0 1\n";
    expect_percent(large.collector.collectSystem(start + 2s).cpu, 100.0);
}

void test_cpu_each_component_busy_definition() {
    for (std::size_t changed = 0; changed < 8; ++changed) {
        Fixture fixture;
        fixture.stat.text = "cpu 0 0 0 0 0 0 0 0\n";
        fixture.collector.collectSystem(start);
        std::string current = "cpu";
        for (std::size_t i = 0; i < 8; ++i) current += i == changed ? " 100" : " 0";
        fixture.stat.text = current + "\n";
        expect_percent(fixture.collector.collectSystem(start + 2s).cpu, changed == 3 || changed == 4 ? 0.0 : 100.0);
    }
}

void test_each_cpu_counter_regression_and_rebaseline() {
    for (std::size_t regressed = 0; regressed < 8; ++regressed) {
        Fixture fixture;
        fixture.stat.text = "cpu 100 100 100 100 100 100 100 100\n";
        fixture.collector.collectSystem(start);
        std::string current = "cpu", next = "cpu";
        for (std::size_t i = 0; i < 8; ++i) {
            const auto value = i == regressed ? 99 : 200;
            current += " " + std::to_string(value);
            next += " " + std::to_string(value + 10);
        }
        fixture.stat.text = current + "\n";
        const auto invalid = fixture.collector.collectSystem(start + 2s);
        expect_unavailable(invalid.cpu);
        require(invalid.cpu.consecutive_errors == 1 && !invalid.cpu.last_success_at, "regression metadata");
        fixture.stat.text = next + "\n";
        expect_percent(fixture.collector.collectSystem(start + 4s).cpu, 75.0);
    }
}

void test_cpu_zero_delta_shape_change_and_wrap() {
    Fixture fixture;
    fixture.collector.collectSystem(start);
    expect_unavailable(fixture.collector.collectSystem(start + 2s).cpu);
    fixture.stat.text = second_cpu;
    expect_percent(fixture.collector.collectSystem(start + 4s).cpu, 75.0);
    fixture.stat.text = "cpu 400 100 180 340\n";
    expect_unavailable(fixture.collector.collectSystem(start + 6s).cpu);
    fixture.stat.text = "cpu 410 100 180 350\n";
    expect_percent(fixture.collector.collectSystem(start + 8s).cpu, 50.0);
    fixture.stat.text = "cpu 410 100 180 350 0\n"; // Absent versus present zero is a shape change.
    expect_unavailable(fixture.collector.collectSystem(start + 10s).cpu);
    fixture.stat.text = "cpu 420 100 180 360 0\n";
    expect_percent(fixture.collector.collectSystem(start + 12s).cpu, 50.0);

    Fixture wrap;
    wrap.stat.text = "cpu 18446744073709551615 0 0 0\n";
    wrap.collector.collectSystem(start);
    wrap.stat.text = "cpu 0 0 0 0\n";
    expect_unavailable(wrap.collector.collectSystem(start + 2s).cpu);
    wrap.stat.text = "cpu 10 0 0 10\n";
    expect_percent(wrap.collector.collectSystem(start + 4s).cpu, 50.0);

    Fixture idle;
    idle.stat.text = "cpu 100 0 0 100 100\n";
    idle.collector.collectSystem(start);
    idle.stat.text = "cpu 0 0 0 200 200\n"; // Naive aggregate-only validation has idle_delta > total_delta.
    expect_unavailable(idle.collector.collectSystem(start + 2s).cpu);
    idle.stat.text = "cpu 10 0 0 205 205\n";
    expect_percent(idle.collector.collectSystem(start + 4s).cpu, 50.0);
}

void test_invalid_cpu_text_clears_baseline() {
    const std::vector<std::string> invalid{
        "", "cpu0 1 2 3 4\n", "cpu\n", "cpu 1 2 3\n", "cpu -1 2 3 4\n", "cpu +1 2 3 4\n",
        "cpu 1.5 2 3 4\n", "cpu 1x 2 3 4\n", "cpu 18446744073709551616 0 0 0\n",
        "cpu 18446744073709551615 1 0 0\n", "cpu 0 0 0 18446744073709551615 1\n",
        "cpu 1 2 3 4 bad\n", "cpu 1 2 3 4 5 6 7 -8\n", "cpu 1 2 3 4\ncpu 1 2 3 4\n"
    };
    for (const auto& text : invalid) {
        Fixture fixture;
        fixture.collector.collectSystem(start);
        fixture.stat.text = text;
        const auto failed = fixture.collector.collectSystem(start + 2s);
        expect_unavailable(failed.cpu);
        require(failed.cpu.consecutive_errors == 1 && failed.memory.quality == MetricQuality::valid,
                "CPU parse failure must not invalidate memory");
        fixture.stat.text = first_cpu;
        const auto warming = fixture.collector.collectSystem(start + 4s);
        require(warming.cpu.quality == MetricQuality::warming_up && !warming.cpu.value && !warming.cpu.last_success_at,
                "CPU parse failure must discard old baseline");
        fixture.stat.text = second_cpu;
        expect_percent(fixture.collector.collectSystem(start + 6s).cpu, 75.0);
    }
}

void test_memory_available_formula_order_whitespace_and_boundaries() {
    Fixture fixture;
    fixture.meminfo.text = "MemAvailable:\t200 kB\r\nMemFree: 900 kB\nMemTotal: 1000 kB\n";
    const auto sample = fixture.collector.collectSystem(start);
    require(sample.memory.value && sample.memory.value->used_percent == 80.0, "MemFree influenced usage or order mattered");
    fixture.meminfo.text = "MemTotal: 1000 kB\nMemAvailable: 0 kB\n";
    require(fixture.collector.collectSystem(start + 2s).memory.value->used_percent == 100.0, "zero available is valid");
    fixture.meminfo.text = "MemTotal: 1000 kB\nMemAvailable: 1000 kB\n";
    require(fixture.collector.collectSystem(start + 4s).memory.value->used_percent == 0.0, "all available is valid");
    fixture.meminfo.text = "MemTotal: 18014398509481983 kB\nMemAvailable: 0 kB\n";
    const auto large = fixture.collector.collectSystem(start + 6s);
    require(large.memory.quality == MetricQuality::valid && large.memory.value &&
            large.memory.value->total_bytes == 18446744073709550592ULL && large.memory.value->used_percent == 100.0,
            "largest convertible kB count must remain valid");
}

void test_invalid_memory_text_and_no_free_fallback() {
    const std::vector<std::string> invalid{
        "", "MemTotal: 1000 kB\nMemFree: 200 kB\n", "MemAvailable: 200 kB\n",
        "MemTotal: 1000 kB\nMemTotal: 1000 kB\nMemAvailable: 200 kB\n",
        "MemTotal: 1000 kB\nMemAvailable: 200 kB\nMemAvailable: 200 kB\n",
        "MemTotal: 0 kB\nMemAvailable: 0 kB\n", "MemTotal: 1000 kB\nMemAvailable: 1001 kB\n",
        "MemTotal: -1 kB\nMemAvailable: 0 kB\n", "MemTotal: 1000 kB\nMemAvailable: -1 kB\n",
        "MemTotal: +1000 kB\nMemAvailable: 0 kB\n", "MemTotal: 1.5 kB\nMemAvailable: 0 kB\n",
        "MemTotal: 1000 MB\nMemAvailable: 200 kB\n", "MemTotal: 1000 kB\nMemAvailable: 200 KB\n",
        "MemTotal: 1000\nMemAvailable: 200 kB\n", "MemTotal: 1000 kB\nMemAvailable: 200\n",
        "MemTotal: 1000 kB extra\nMemAvailable: 200 kB\n",
        "MemTotal: 1000x kB\nMemAvailable: 0 kB\n", "MemTotal: 1000 kB\nMemAvailable: nope kB\n",
        "MemTotal: 18446744073709551616 kB\nMemAvailable: 0 kB\n",
        "MemTotal: 1000 kB\nMemAvailable: 18446744073709551616 kB\n",
        "MemTotal: 18014398509481984 kB\nMemAvailable: 0 kB\n",
        "MemTotal: 1000 kB\nMemAvailable: 18014398509481984 kB\n"
    };
    for (const auto& text : invalid) {
        Fixture fixture;
        fixture.collector.collectSystem(start);
        fixture.meminfo.text = text;
        fixture.stat.text = second_cpu;
        const auto sample = fixture.collector.collectSystem(start + 2s);
        require(sample.memory.quality == MetricQuality::unavailable && !sample.memory.value &&
                sample.memory.last_success_at == start && sample.memory.consecutive_errors == 1,
                "invalid memory retained a fake/current value or lost diagnostics");
        expect_percent(sample.cpu, 75.0);
    }
}

void test_read_errors_partial_validity_and_recovery_metadata() {
    for (const auto error : {ProcReadError::not_present, ProcReadError::permission_denied,
                             ProcReadError::io_error, ProcReadError::too_large}) {
        Fixture fixture;
        fixture.collector.collectSystem(start);
        fixture.stat.text = second_cpu;
        fixture.collector.collectSystem(start + 2s);
        fixture.stat.error = error; // Even plausible text cannot override a read error.
        const auto failed = fixture.collector.collectSystem(start + 4s);
        expect_unavailable(failed.cpu);
        require(failed.cpu.last_success_at == start + 2s && failed.cpu.consecutive_errors == 1 &&
                failed.memory.quality == MetricQuality::valid, "CPU read failure metadata/partial validity");
        fixture.meminfo.error = error;
        const auto both_failed = fixture.collector.collectSystem(start + 6s);
        require(both_failed.cpu.consecutive_errors == 2 && !both_failed.cpu.value &&
                both_failed.memory.quality == MetricQuality::unavailable && !both_failed.memory.value &&
                both_failed.memory.consecutive_errors == 1 && both_failed.memory.last_success_at == start + 4s,
                "independent failure counters and last-success times");
        fixture.stat.error = ProcReadError::none;
        fixture.meminfo.error = ProcReadError::none;
        fixture.stat.text = first_cpu;
        const auto recovered = fixture.collector.collectSystem(start + 8s);
        require(recovered.cpu.quality == MetricQuality::warming_up && !recovered.cpu.value &&
                recovered.cpu.last_success_at == start + 2s && recovered.cpu.consecutive_errors == 0 &&
                recovered.memory.quality == MetricQuality::valid && recovered.memory.consecutive_errors == 0 &&
                recovered.memory.last_success_at == start + 8s, "recovery must warm CPU and reset errors independently");
        fixture.stat.text = second_cpu;
        expect_percent(fixture.collector.collectSystem(start + 10s).cpu, 75.0);
    }
    Fixture memory_only;
    memory_only.collector.collectSystem(start);
    memory_only.meminfo.error = ProcReadError::permission_denied;
    memory_only.stat.text = second_cpu;
    const auto sample = memory_only.collector.collectSystem(start + 2s);
    expect_percent(sample.cpu, 75.0);
    require(sample.memory.quality == MetricQuality::unavailable && !sample.memory.value,
            "memory read failure must not invalidate CPU");
}

void test_first_read_failure_has_no_success_or_value() {
    Fixture fixture;
    fixture.stat.error = ProcReadError::io_error;
    fixture.meminfo.error = ProcReadError::permission_denied;
    const auto sample = fixture.collector.collectSystem(start);
    expect_unavailable(sample.cpu);
    require(!sample.cpu.last_success_at && sample.cpu.consecutive_errors == 1 &&
            sample.memory.quality == MetricQuality::unavailable && !sample.memory.value &&
            !sample.memory.last_success_at && sample.memory.consecutive_errors == 1,
            "first failed sample must not claim success");
}

void test_reader_size_limit_exact_and_oversize() {
    Fixture fixture;
    fixture.stat.text = first_cpu;
    fixture.stat.text.resize(ResourceCollector::system_file_limit, ' ');
    fixture.meminfo.text.resize(ResourceCollector::system_file_limit, ' ');
    const auto exact = fixture.collector.collectSystem(start);
    require(exact.cpu.quality == MetricQuality::warming_up && exact.memory.quality == MetricQuality::valid,
            "exact 64KiB files should be accepted");
    fixture.stat.text.push_back(' ');
    const auto cpu_oversize = fixture.collector.collectSystem(start + 2s);
    expect_unavailable(cpu_oversize.cpu);
    require(cpu_oversize.memory.quality == MetricQuality::valid, "oversize CPU must leave memory valid");
    fixture.stat.text = first_cpu;
    fixture.meminfo.text.push_back(' ');
    const auto memory_oversize = fixture.collector.collectSystem(start + 4s);
    require(memory_oversize.cpu.quality == MetricQuality::warming_up &&
            memory_oversize.memory.quality == MetricQuality::unavailable && !memory_oversize.memory.value,
            "collector must enforce cap even for a noncompliant injected reader");
}

void test_completion_clock_and_cpu_without_elapsed_time() {
    unsigned reads = 0;
    ResourceCollector collector([&](const std::string& path, std::size_t limit) {
        require(limit == ResourceCollector::system_file_limit, "unexpected clock fixture limit");
        ++reads;
        return ProcReadResult{path == "/proc/stat" ? first_cpu : good_memory, ProcReadError::none};
    }, [&] {
        require(reads == 2, "completion clock consulted before both reads finished");
        return start + 5s;
    });
    const auto sample = collector.collectSystem();
    require(sample.sampled_at == start + 5s && sample.memory.last_success_at == start + 5s,
            "actual completion time was not used");

    Fixture fixture;
    fixture.collector.collectSystem(start);
    fixture.stat.text = second_cpu;
    // System CPU uses counter deltas, independently of wall-clock intervals.
    expect_percent(fixture.collector.collectSystem(start).cpu, 75.0);
}

void test_unexpected_reader_exception_reaches_caller_boundary() {
    ResourceCollector collector([](const std::string&, std::size_t) -> ProcReadResult {
        throw std::runtime_error("unexpected injected reader failure");
    });
    bool caught = false;
    try {
        collector.collectSystem(start);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    require(caught, "unexpected reader exceptions must reach the future worker shutdown boundary");
}

std::string process_stat(int pid = 41, const std::string& user = "100", const std::string& system = "50",
                         const std::string& birth = "1234", const std::string& pages = "3", char state = 'S',
                         const std::string& comm = "worker ) (with spaces)") {
    std::string result = std::to_string(pid) + " (" + comm + ") " + state;
    for (unsigned field = 4; field <= 24; ++field) {
        result += " ";
        if (field == 14) result += user;
        else if (field == 15) result += system;
        else if (field == 16 || field == 17) result += "999999"; // Children never enter own CPU.
        else if (field == 22) result += birth;
        else if (field == 24) result += pages;
        else if (field == 8 || field == 19) result += "-1";
        else result += "0";
    }
    return result + "\n";
}

struct ProcessFixture {
    ResourceClock::time_point at = start;
    ProcReadResult text{process_stat(), ProcReadError::none};
    ResourceCollector::Identities identities{std::vector<ProcessIdentity>{{"worker", 41, 7}}};
    std::vector<std::string> reads;
    unsigned validations = 0;
    bool busy = false;
    bool mismatch = false;
    ResourceCollector collector;
    explicit ProcessFixture(ProcessPlatform platform = {250, 16384}) : collector(
        [this](const std::string& path, std::size_t limit) {
            reads.push_back(path);
            if (path == "/proc/stat") return ProcReadResult{first_cpu, ProcReadError::none};
            if (path == "/proc/meminfo") return ProcReadResult{good_memory, ProcReadError::none};
            require(limit == 4096, "process size cap changed");
            return text;
        }, [this] { return at; }, platform) {}
    ProcessResourceScan scan(ResourceCollector::Continue gate = {}) {
        return collector.collectProcesses(identities, [this](const auto& captured) -> ResourceCollector::Identities {
            ++validations;
            if (busy) return std::nullopt;
            auto result = captured;
            if (mismatch && !result.empty()) ++result.front().instance_generation;
            return result;
        }, 2s, std::move(gate));
    }
};

void test_process_comm_fields_units_and_multicore_cpu() {
    ProcessFixture f;
    auto first = f.scan();
    require(first.quality == MetricQuality::valid && first.processes.size() == 1, "process scan unavailable");
    const auto& row = first.processes.front();
    require(row.service_name == "worker" && row.pid == 41 && row.instance_generation == 7 &&
            row.proc_start_time_ticks == 1234 && row.sampled_at == start && row.rss_bytes == 49152 &&
            row.cpu_quality == MetricQuality::warming_up && !row.cpu_percent,
            "comm offsets, launch identity, injected pagesize or first CPU failed");
    f.at += 2s;
    f.text.text = process_stat(41, "1350", "50");
    const auto next = f.scan().processes.front();
    require(next.cpu_quality == MetricQuality::valid && next.cpu_percent && *next.cpu_percent == 250.0 &&
            next.rss_bytes == 49152, "HZ/real elapsed CPU, child exclusion or >100 CPU failed");
    require(f.reads == std::vector<std::string>{"/proc/41/stat", "/proc/41/stat"} && f.validations == 2,
            "process reads/validation count changed");
}

void test_process_read_failures_preserve_anchor_and_clear_cpu() {
    for (const auto error : {ProcReadError::not_present, ProcReadError::permission_denied,
                            ProcReadError::io_error, ProcReadError::too_large}) {
        ProcessFixture f;
        f.scan();
        f.text = {"", error};
        f.at += 2s;
        const auto failed = f.scan().processes.front();
        require(failed.observation_status == (error == ProcReadError::not_present ?
                    ProcessObservationStatus::not_present : ProcessObservationStatus::unavailable) &&
                !failed.cpu_percent && !failed.rss_bytes && !failed.proc_start_time_ticks,
                "process read failure reused stale resource values");
        f.text = {process_stat(), ProcReadError::none};
        f.at += 2s;
        require(f.scan().processes.front().cpu_quality == MetricQuality::warming_up, "read error kept CPU baseline");
        f.text.text = process_stat(41, "100", "50", "9999");
        require(f.scan().processes.front().observation_status == ProcessObservationStatus::identity_changed,
                "read failure erased launch starttime anchor");
    }
}

void test_process_reused_pid_token_and_repeated_starttime_mismatch() {
    ProcessFixture f;
    f.scan();
    f.text.text = process_stat(41, "200", "50", "9999");
    for (int i = 0; i < 3; ++i) {
        f.at += 2s;
        const auto row = f.scan().processes.front();
        require(row.observation_status == ProcessObservationStatus::identity_changed &&
                !row.rss_bytes && !row.cpu_percent && !row.proc_start_time_ticks,
                "same launch accepted reused PID on repeated mismatch");
    }
    f.identities->front().instance_generation = 8;
    auto replacement = f.scan().processes.front();
    require(replacement.proc_start_time_ticks == 9999 && replacement.cpu_quality == MetricQuality::warming_up &&
            replacement.instance_generation == 8 && replacement.rss_bytes, "new SM launch did not reset identity");
    f.identities->front().pid = 42;
    f.text.text = process_stat(42);
    require(f.scan().processes.front().cpu_quality == MetricQuality::warming_up, "new PID inherited baseline");
    f.identities = std::vector<ProcessIdentity>{};
    require(f.scan().processes.empty(), "empty eligible set generated fake rows");
    f.identities = std::vector<ProcessIdentity>{{"worker", 41, 7}};
    f.text.text = process_stat();
    require(f.scan().processes.front().cpu_quality == MetricQuality::warming_up, "retired launch cache survived");
}

void test_process_validation_busy_mismatch_and_unavailable_capture() {
    ProcessFixture f;
    f.scan();
    f.at += 2s;
    f.text.text = process_stat(41, "350", "50");
    f.busy = true;
    const auto busy = f.scan();
    require(busy.quality == MetricQuality::unavailable && busy.processes.empty(), "busy validator published rows");
    f.busy = false;
    f.mismatch = true;
    const auto changed = f.scan();
    require(changed.quality == MetricQuality::unavailable && changed.processes.empty(), "mismatch published rows");
    f.mismatch = false;
    f.at += 2s;
    f.text.text = process_stat(41, "600", "50");
    const auto row = f.scan().processes.front();
    require(row.cpu_percent && *row.cpu_percent == 50.0, "rejected scan committed pending CPU counters/time");
    const auto count = f.reads.size();
    const auto validation_count = f.validations;
    f.identities.reset();
    require(f.scan().quality == MetricQuality::unavailable && f.reads.size() == count &&
            f.validations == validation_count, "unavailable capture did I/O or owner query");
    f.identities = std::vector<ProcessIdentity>{{"worker", 41, 7}};
    f.at += 2s;
    f.text.text = process_stat(41, "850", "50");
    require(f.scan().processes.front().cpu_percent == 50.0, "capture skip mutated old baseline");
}

void test_process_zombie_and_malformed_stat() {
    ProcessFixture zombie;
    zombie.scan();
    zombie.text.text = process_stat(41, "200", "50", "1234", "3", 'Z');
    const auto row = zombie.scan().processes.front();
    require(row.observation_status == ProcessObservationStatus::zombie && row.proc_start_time_ticks == 1234 &&
            !row.cpu_percent && !row.rss_bytes, "zombie reported active resource values");
    zombie.text.text = process_stat();
    require(zombie.scan().processes.front().cpu_quality == MetricQuality::warming_up, "zombie preserved CPU baseline");
    const std::vector<std::string> invalid{
        "", "41 worker S", "41 (worker) S 0", process_stat(42),
        process_stat(41, "-1"), process_stat(41, "oops"), process_stat(41, "1", "-1"),
        process_stat(41, "1", "1", "-1"), process_stat(41, "1", "1", "bad"),
        process_stat(41, "1", "1", "1234", "-1"), process_stat(41, "1", "1", "1234", "oops"),
        process_stat(41, "18446744073709551615", "1"), process_stat(41, "18446744073709551616"),
        process_stat(41, "1", "1", "1234", "9223372036854775808"),
        process_stat(41, "1", "1", "1234", "1125899906842624"), // 16KiB product overflows.
        process_stat() + std::string(4096, ' ')
    };
    for (const auto& text : invalid) {
        ProcessFixture f;
        f.scan();
        f.text.text = text;
        const auto bad = f.scan().processes.front();
        require(bad.observation_status == ProcessObservationStatus::unavailable &&
                !bad.rss_bytes && !bad.cpu_percent, "malformed/overflow/PID/oversize stat accepted");
        f.text.text = process_stat();
        require(f.scan().processes.front().cpu_quality == MetricQuality::warming_up, "parse failure kept CPU baseline");
    }
}

void test_process_actual_elapsed_long_gap_and_counter_regression() {
    ProcessFixture f;
    f.scan();
    f.at += 3s;
    f.text.text = process_stat(41, "850", "50");
    require(f.scan().processes.front().cpu_percent == 100.0, "normal late sample forced configured interval");
    f.at += 7s;
    f.text.text = process_stat(41, "1000", "50");
    const auto gap = f.scan().processes.front();
    require(gap.cpu_quality == MetricQuality::warming_up && !gap.cpu_percent && gap.rss_bytes,
            "long gap did not rebaseline independently of RSS");
    f.at += 6s; // Exactly 3*interval remains eligible.
    f.text.text = process_stat(41, "2500", "50");
    require(f.scan().processes.front().cpu_percent == 100.0, "long gap boundary wrong");
    f.text.text = process_stat(41, "2600", "50");
    require(f.scan().processes.front().cpu_quality == MetricQuality::unavailable, "elapsed zero accepted");
    f.at -= 1s;
    require(f.scan().processes.front().cpu_quality == MetricQuality::unavailable, "backwards time accepted");
    f.at += 2s;
    f.text.text = process_stat(41, "1", "0");
    require(f.scan().processes.front().cpu_quality == MetricQuality::unavailable, "negative CPU delta accepted");
    f.at += 2s;
    f.text.text = process_stat(41, "501", "0");
    require(f.scan().processes.front().cpu_percent == 100.0, "valid current counters did not rebaseline");
}

void test_process_platform_failure_isolated_from_system() {
    for (const auto platform : {ProcessPlatform{0, 4096}, ProcessPlatform{-1, 4096},
                                ProcessPlatform{100, 0}, ProcessPlatform{100, -1}}) {
        ProcessFixture f(platform);
        const auto scan = f.scan();
        require(scan.quality == MetricQuality::unavailable && scan.processes.empty() && f.reads.empty(),
                "invalid HZ/pagesize produced a process value/read");
        const auto system = f.collector.collectSystem(start);
        require(system.memory.quality == MetricQuality::valid && system.cpu.quality == MetricQuality::warming_up,
                "sysconf failure contaminated system metrics");
    }
}

void test_process_validation_rejects_missing_duplicate_pid_and_anchor_commit() {
    ProcessFixture f;
    unsigned calls = 0;
    auto validate = [&](const auto& captured) -> ResourceCollector::Identities {
        auto result = captured;
        if (calls == 0) result.clear();
        else if (calls == 1) ++result.front().pid;
        else if (calls == 2) result = {{"different", 41, 7}};
        ++calls;
        return result;
    };
    for (int i = 0; i < 3; ++i) {
        const auto rejected = f.collector.collectProcesses(f.identities, validate);
        require(rejected.quality == MetricQuality::unavailable && rejected.processes.empty(),
                "missing/wrong PID/name post-validation result accepted");
    }
    f.text.text = process_stat(41, "200", "50", "9999");
    const auto first = f.scan().processes.front();
    require(first.proc_start_time_ticks == 9999 && first.cpu_quality == MetricQuality::warming_up,
            "rejected first scan committed starttime anchor or CPU baseline");
    const std::vector<ProcessIdentity> pair{{"worker", 41, 7}, {"other", 42, 8}};
    const auto duplicate = f.collector.collectProcesses(pair, [](const auto& captured) -> ResourceCollector::Identities {
        return std::vector<ProcessIdentity>{captured.front(), captured.front()};
    });
    require(duplicate.quality == MetricQuality::unavailable && duplicate.processes.empty(),
            "same-sized duplicate validator result accepted");
}

void test_process_states_read_limit_exceptions_and_post_validation_gate() {
    ProcessFixture f;
    for (const char state : {'S', 'D', 'R', 'T', 't', 'I', 'x'}) {
        f.text.text = process_stat(41, "100", "50", "1234", "0", state);
        const auto row = f.scan().processes.front();
        require(row.observation_status == ProcessObservationStatus::observed && row.rss_bytes == 0,
                "non-zombie state inferred service failure or zero RSS invalid");
    }
    f.text.text = process_stat();
    f.text.text.resize(4096, ' ');
    require(f.scan().processes.front().rss_bytes == 49152, "exact process size cap rejected");
    f.at += 2s;
    f.text.text = process_stat(41, "350", "50");
    unsigned gates = 0;
    const auto stopped = f.scan([&] { return ++gates < 4; });
    require(stopped.quality == MetricQuality::unavailable && stopped.processes.empty(),
            "gate closed after owner validation still committed/published");
    f.at += 2s;
    f.text.text = process_stat(41, "600", "50");
    require(f.scan().processes.front().cpu_percent == 50.0, "post-validation gate committed CPU baseline");

    bool fail = true;
    ResourceCollector collector([&](const std::string&, std::size_t) -> ProcReadResult {
        if (fail) throw std::runtime_error("unexpected process reader failure");
        return {process_stat(), ProcReadError::none};
    }, [] { return start; }, ProcessPlatform{250, 16384});
    auto validate = [](const auto& captured) -> ResourceCollector::Identities { return captured; };
    bool caught = false;
    try { collector.collectProcesses(f.identities, validate); }
    catch (const std::runtime_error&) { caught = true; }
    require(caught, "unexpected process exception was swallowed");
    fail = false;
    require(collector.collectProcesses(f.identities, validate).processes.front().cpu_quality == MetricQuality::warming_up,
            "reader exception committed process state");
}

void test_process_batch_sort_partial_failure_completion_and_gate() {
    for (const unsigned count : {1u, 8u, 32u}) {
        std::vector<ProcessIdentity> identities;
        for (unsigned i = count; i > 0; --i) identities.push_back({"service_" + std::to_string(100 + i), int(100 + i), i});
        unsigned reads = 0, validations = 0;
        auto at = start;
        ResourceCollector collector([&](const std::string& path, std::size_t limit) {
            ++reads;
            at += 1ms;
            if (path == "/proc/stat") return ProcReadResult{first_cpu, ProcReadError::none};
            if (path == "/proc/meminfo") return ProcReadResult{good_memory, ProcReadError::none};
            require(limit == 4096, "batch read cap");
            const int pid = std::stoi(path.substr(6));
            if (pid == 101) return ProcReadResult{"", ProcReadError::permission_denied};
            return ProcReadResult{process_stat(pid), ProcReadError::none};
        }, [&] { return at; }, ProcessPlatform{100, 4096});
        collector.collectSystem();
        auto validate = [&](const auto& captured) -> ResourceCollector::Identities { ++validations; return captured; };
        const auto scan = collector.collectProcesses(identities, validate);
        require(scan.quality == MetricQuality::valid && scan.processes.size() == count &&
                reads == 2 + count && validations == 1, "batch count or owner query changed");
        for (std::size_t i = 0; i < scan.processes.size(); ++i) {
            const auto& row = scan.processes[i];
            require(row.sampled_at == start + 2ms + std::chrono::milliseconds(count - i), "completion clock not per row");
            require(i == 0 || scan.processes[i - 1].service_name < row.service_name, "rows not sorted");
            require(row.observation_status == (i == 0 ? ProcessObservationStatus::unavailable :
                        ProcessObservationStatus::observed), "one read error contaminated batch");
        }
    }
    ProcessFixture f;
    f.scan();
    f.at += 2s;
    f.text.text = process_stat(41, "350", "50");
    unsigned calls = 0;
    const auto stopped = f.scan([&] { return ++calls < 3; }); // Stop after I/O, before validation.
    require(stopped.quality == MetricQuality::unavailable && stopped.processes.empty() && f.validations == 1,
            "stop gate published/validated an in-flight scan");
    f.at += 2s;
    f.text.text = process_stat(41, "600", "50");
    require(f.scan().processes.front().cpu_percent == 50.0, "stop gate committed pending baseline");
    const auto read_count = f.reads.size();
    require(f.scan([] { return false; }).quality == MetricQuality::unavailable && f.reads.size() == read_count,
            "closed gate performed I/O");
}

} // namespace

int main() {
    try {
        test_first_sample_and_aggregate_delta();
        test_cpu_optional_fields_and_guest_exclusion();
        test_cpu_zero_full_and_large_valid_delta();
        test_cpu_each_component_busy_definition();
        test_each_cpu_counter_regression_and_rebaseline();
        test_cpu_zero_delta_shape_change_and_wrap();
        test_invalid_cpu_text_clears_baseline();
        test_memory_available_formula_order_whitespace_and_boundaries();
        test_invalid_memory_text_and_no_free_fallback();
        test_read_errors_partial_validity_and_recovery_metadata();
        test_first_read_failure_has_no_success_or_value();
        test_reader_size_limit_exact_and_oversize();
        test_completion_clock_and_cpu_without_elapsed_time();
        test_unexpected_reader_exception_reaches_caller_boundary();
        test_process_comm_fields_units_and_multicore_cpu();
        test_process_read_failures_preserve_anchor_and_clear_cpu();
        test_process_reused_pid_token_and_repeated_starttime_mismatch();
        test_process_validation_busy_mismatch_and_unavailable_capture();
        test_process_zombie_and_malformed_stat();
        test_process_actual_elapsed_long_gap_and_counter_regression();
        test_process_platform_failure_isolated_from_system();
        test_process_validation_rejects_missing_duplicate_pid_and_anchor_commit();
        test_process_states_read_limit_exceptions_and_post_validation_gate();
        test_process_batch_sort_partial_failure_completion_and_gate();
        std::cout << "resource collector fixture tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
