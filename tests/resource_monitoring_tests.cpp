#include "runtime/config_manager.hpp"
#include "runtime/resource_collector.hpp"
#include "runtime/runtime_manager.hpp"
#include <atomic>
#include "runtime/service_manager.hpp"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <cerrno>
#include <csignal>
#include <sys/wait.h>

using namespace runtime;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Call>
void rejects(Call call, const char* message, const std::string& diagnostic = {}) {
    bool caught = false;
    try { call(); }
    catch (const std::exception& error) {
        caught = true;
        require(diagnostic.empty() || std::string(error.what()).find(diagnostic) != std::string::npos,
                "configuration diagnostic did not name conflicting fields");
    }
    require(caught, message);
}

struct ConfigFile {
    std::filesystem::path directory;
    std::filesystem::path path;
    ConfigFile() {
        std::string pattern = (std::filesystem::temp_directory_path() / "p5-config-XXXXXX").string();
        auto* created = ::mkdtemp(pattern.data());
        require(created != nullptr, "temporary config directory failed");
        directory = created;
        path = directory / "runtime.json";
    }
    ~ConfigFile() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    void write(const std::string& text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        require(bool(output), "fixture config write failed");
    }
    RuntimeConfig load() const { return ConfigManager::load_runtime_file(path.string()); }
};

const std::string single = R"({"service_name":"idle","executable":"/bin/true","autostart":false)";
const std::vector<std::string> monitoring_keys{
    "sample_interval_seconds", "cpu_warning", "cpu_clear", "memory_warning", "memory_clear",
    "memory_critical", "memory_critical_clear"
};

void require_defaults(const MonitoringConfig& config) {
    require(config.sample_interval_seconds == 2s && config.cpu_warning == 80 && config.cpu_clear == 75 &&
            config.memory_warning == 80 && config.memory_clear == 75 && config.memory_critical == 95 &&
            config.memory_critical_clear == 90, "fixed monitoring defaults changed");
}

void test_config_legacy_roots_defaults_wrapper_and_service_fields() {
    ConfigFile f;
    f.write(single + R"(,"startup_timeout":21,"heartbeat_timeout":22,"shutdown_timeout":6,
        "recovery_timeout":101,"restart_policy":"on-failure","arguments":["a"],
        "environment":["K=V"],"working_directory":"/tmp","dependency":["other"],"old_extra":7})");
    const auto loaded = f.load();
    require_defaults(loaded.monitoring);
    const auto services = ConfigManager::load_file(f.path.string());
    require(loaded.services.size() == 1 && services.size() == 1, "single root/wrapper service count");
    const auto& service = services.front();
    require(service.service_name == "idle" && service.executable == "/bin/true" && !service.autostart &&
            service.startup_timeout == 21s && service.heartbeat_timeout == 22s && service.shutdown_timeout == 6s &&
            service.recovery_timeout == 101s && service.restart_policy == RestartPolicy::on_failure &&
            service.arguments == std::vector<std::string>{"a"} && service.environment == std::vector<std::string>{"K=V"} &&
            service.working_directory == "/tmp" && service.dependency == std::vector<std::string>{"other"},
            "load_file wrapper changed legacy service semantics");
    f.write("{\"services\":[" + single + "}],\"root_extra\":true}");
    require(f.load().services.front().service_name == "idle", "services root or unknown root field rejected");
    require_defaults(f.load().monitoring);
    f.write("{\"services\":[" + single + "}],\"monitoring\":{}}");
    require_defaults(f.load().monitoring);
}

void test_config_seven_fields_and_partial_fixed_defaults() {
    ConfigFile f;
    f.write(single + R"(,"monitoring":{"sample_interval_seconds":5,"cpu_warning":85,"cpu_clear":70,
        "memory_warning":82,"memory_clear":72,"memory_critical":98,"memory_critical_clear":92}})");
    const auto all = f.load();
    require(all.services.size() == 1 && all.monitoring.sample_interval_seconds == 5s &&
            all.monitoring.cpu_warning == 85 && all.monitoring.cpu_clear == 70 &&
            all.monitoring.memory_warning == 82 && all.monitoring.memory_clear == 72 &&
            all.monitoring.memory_critical == 98 && all.monitoring.memory_critical_clear == 92,
            "complete global monitoring not loaded");
    f.write("{\"services\":[" + single + "}],\"monitoring\":{\"cpu_warning\":85}}");
    const auto partial = f.load().monitoring;
    require(partial.cpu_warning == 85 && partial.cpu_clear == 75 && partial.memory_warning == 80 &&
            partial.sample_interval_seconds == 2s, "partial config adjusted defaults");
    f.write(single + R"(,"monitoring":{"cpu_warning":70}})");
    rejects([&] { f.load(); }, "partial CPU warning secretly derived clear", "cpu_clear");
    f.write(single + R"(,"monitoring":{"memory_warning":70}})");
    rejects([&] { f.load(); }, "partial memory warning secretly derived clear", "memory_clear");
}

void test_config_json_boundaries_and_threshold_order() {
    ConfigFile f;
    for (const int interval : {1, 60}) {
        f.write(single + ",\"monitoring\":{\"sample_interval_seconds\":" + std::to_string(interval) +
            R"(,"cpu_clear":0,"cpu_warning":100,"memory_clear":0,"memory_warning":1,
                "memory_critical_clear":1,"memory_critical":100}})");
        const auto c = f.load().monitoring;
        require(c.sample_interval_seconds.count() == interval && c.cpu_clear == 0 && c.cpu_warning == 100 &&
                c.memory_warning == c.memory_critical_clear && c.memory_critical == 100, "valid boundary rejected");
    }
    for (const auto& key : monitoring_keys) {
        for (const auto& number : {std::string("-1"), std::string("101"), std::string("9223372036854775807")}) {
            f.write(single + ",\"monitoring\":{\"" + key + "\":" + number + "}}");
            rejects([&] { f.load(); }, "range violation accepted");
        }
    }
    const std::vector<std::string> invalid{
        "\"sample_interval_seconds\":0", "\"sample_interval_seconds\":61",
        "\"cpu_clear\":80", "\"cpu_clear\":81", "\"cpu_warning\":75",
        "\"memory_clear\":80", "\"memory_clear\":81", "\"memory_warning\":75",
        "\"memory_warning\":91", "\"memory_critical_clear\":79",
        "\"memory_critical_clear\":95", "\"memory_critical\":90"
    };
    for (const auto& fields : invalid) {
        f.write(single + ",\"monitoring\":{" + fields + "}}");
        rejects([&] { f.load(); }, "equal/reversed thresholds or interval violation accepted");
    }
}

void test_config_wrong_types_unknown_duplicate_float_and_overflow() {
    ConfigFile f;
    for (const auto& key : monitoring_keys) {
        for (const auto& value : {"true", "false", "null", "\"80\"", "[]", "{}", "80.5", "8e1", "9223372036854775808"}) {
            f.write(single + ",\"monitoring\":{\"" + key + "\":" + value + "}}");
            rejects([&] { f.load(); }, "monitoring accepted bool/noninteger/overflow/wrong type");
        }
        f.write(single + ",\"monitoring\":{\"" + key + "\":80,\"" + key + "\":80}}");
        rejects([&] { f.load(); }, "duplicate monitoring field accepted", "duplicate key");
    }
    for (const auto& value : {"null", "true", "\"text\"", "[]", "2"}) {
        f.write(single + ",\"monitoring\":" + value + "}");
        rejects([&] { f.load(); }, "nonobject monitoring accepted", "monitoring");
    }
    for (const auto& key : {"enabled", "process", "thermal", "disk", "cpu_warnng"}) {
        f.write(single + ",\"monitoring\":{\"" + key + "\":1}}");
        rejects([&] { f.load(); }, "unknown monitoring field accepted", key);
    }
    f.write(single + R"(,"monitoring":{},"monitoring":{}})");
    rejects([&] { f.load(); }, "duplicate root monitoring accepted", "duplicate key");
    f.write("[" + single + "}]");
    rejects([&] { f.load(); }, "bare array root accepted", "root");
}

void test_config_programmatic_fractional_finite_and_legacy_thresholds() {
    MonitoringConfig c;
    c.cpu_warning = 80.5;
    c.cpu_clear = 75.5;
    c.memory_warning = 80.5;
    c.memory_clear = 75.5;
    c.memory_critical_clear = 90.5;
    c.memory_critical = 95.5;
    ConfigManager::validate(c);
    require(c.cpu_warning == 80.5, "programmatic fractional percentage rejected/adjusted");
    const std::vector<double MonitoringConfig::*> percentages{
        &MonitoringConfig::cpu_warning, &MonitoringConfig::cpu_clear, &MonitoringConfig::memory_warning,
        &MonitoringConfig::memory_clear, &MonitoringConfig::memory_critical, &MonitoringConfig::memory_critical_clear
    };
    for (const auto member : percentages) {
        for (const double value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(), -0.1, 100.1}) {
            auto invalid = c;
            invalid.*member = value;
            rejects([&] { ConfigManager::validate(invalid); }, "nonfinite/out of range programmatic percent accepted");
        }
    }
    for (const auto interval : {0s, 61s}) {
        auto invalid = c;
        invalid.sample_interval_seconds = interval;
        rejects([&] { ConfigManager::validate(invalid); }, "programmatic interval out of range");
    }
    const ResourceThresholds legacy{81, 82, 96};
    require(legacy.cpu_warning == 81 && legacy.memory_warning == 82 && legacy.memory_critical == 96,
            "legacy three-field aggregate order changed");
}

void test_config_preserves_service_validation_and_file_limit() {
    ConfigFile f;
    for (const auto& suffix : {",\"recovery_timeout\":0}", ",\"recovery_timeout\":86401}",
                               ",\"startup_timeout\":0}", ",\"environment\":[\"broken\"]}"}) {
        f.write(single + suffix);
        rejects([&] { f.load(); }, "existing service validation bypassed");
        rejects([&] { ConfigManager::load_file(f.path.string()); }, "wrapper bypassed service validation");
    }
    f.write("{\"services\":[" + single + "}," + single + "}]}");
    rejects([&] { f.load(); }, "duplicate service names accepted");
    f.write("{\"services\":[]}");
    rejects([&] { f.load(); }, "empty services accepted");
    f.write(single + "}" + std::string(1024 * 1024, ' '));
    rejects([&] { f.load(); }, "1 MiB file limit bypassed", "1 MiB");
    f.write(single + R"(,"monitoring":{"cpu_clear":80}})");
    rejects([&] { ConfigManager::load_file(f.path.string()); }, "wrapper did not validate global monitoring");
}

struct LaunchGate {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false;
    bool released = false;
    void pause() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        ready.notify_all();
        require(ready.wait_for(lock, 5s, [&] { return released; }), "fake launch gate release timed out");
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(ready.wait_for(lock, 3s, [&] { return entered; }), "launch did not reach gate");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        ready.notify_all();
    }
};

struct FakeProcesses final : ProcessSupervisor {
    LaunchGate* gate = nullptr;
    unsigned launches = 0, stops = 0, forced = 0, reaps = 0;
    int start(const ServiceConfig&) override { ++launches; if (gate) gate->pause(); return 41; }
    void stop(int) override { ++stops; }
    void force_stop(int) override { ++forced; }
    std::vector<ProcessExit> reap() override { ++reaps; return {}; }
};

struct IdentityFixture {
    FakeProcesses processes;
    Monitor monitor{[](Event) {}};
    std::ostringstream output;
    Logger logger{output};
    unsigned changes = 0;
    ServiceManager services{processes, monitor, logger, [this](const ServiceStateChange&) { ++changes; }};
    void add(const std::string& name) {
        ServiceConfig config;
        config.service_name = name;
        config.executable = "/fixture/unused";
        services.add(config);
    }
};

void test_sm_identities_launch_token_cleanup_and_read_only_validation() {
    IdentityFixture f;
    f.add("worker");
    f.add("idle");
    const auto empty = f.services.trySnapshotProcessIdentities();
    require(empty && empty->empty(), "unlaunched services created process rows");
    f.services.startService("worker");
    const auto initial = f.services.query("worker").value();
    const auto captured = f.services.trySnapshotProcessIdentities();
    require(captured && captured->size() == 1 && captured->front().service_name == "worker" &&
            captured->front().pid == 41 && captured->front().instance_generation == initial.launched_generation,
            "SM snapshot did not capture launched_generation");
    f.services.stopService("worker");
    const auto stopping = f.services.query("worker").value();
    require(stopping.generation != stopping.launched_generation && stopping.pid == 41,
            "fixture did not separate STOP and launched token");
    const auto changes = f.changes;
    const auto validated = f.services.tryValidateProcessIdentities(*captured);
    require(validated && validated->size() == 1 && f.services.trySnapshotProcessIdentities()->size() == 1,
            "STOPPING old PID not eligible or compared lifecycle token");
    auto invalid = *captured;
    ++invalid.front().instance_generation;
    require(f.services.tryValidateProcessIdentities(invalid)->empty(), "wrong launch token accepted");
    invalid = *captured;
    ++invalid.front().pid;
    require(f.services.tryValidateProcessIdentities(invalid)->empty(), "wrong PID accepted");
    invalid.front().service_name = "unknown";
    require(f.services.tryValidateProcessIdentities(invalid)->empty(), "unknown name accepted");
    const auto after = f.services.query("worker").value();
    require(after.generation == stopping.generation && after.launched_generation == stopping.launched_generation &&
            after.pid == stopping.pid && after.state == stopping.state && after.restart_count == stopping.restart_count &&
            f.changes == changes && f.processes.launches == 1 && f.processes.stops == 1 &&
            f.processes.forced == 0 && f.processes.reaps == 0, "read-only interfaces performed lifecycle work");
    Event exit{EventType::process_exited, "worker", Clock::now(), 41, 0};
    exit.instance_generation = initial.launched_generation;
    f.services.handle(exit); // Existing writer-owned lifecycle seam, no collector reaping.
    require(f.services.trySnapshotProcessIdentities()->empty(), "cleaned PID remained eligible");
    f.services.startService("worker"); // Fake backend intentionally reuses the same PID.
    require(f.services.tryValidateProcessIdentities(*captured)->empty(), "same PID new launch accepted old token");
    require(f.services.trySnapshotProcessIdentities()->front().instance_generation != initial.launched_generation,
            "new launch did not use SM generation");
    const auto running = f.services.query("worker").value();
    Event miss{EventType::health_missed, "worker", running.start_time.value() + 15s};
    miss.pid = running.pid;
    miss.missed_count = 1;
    miss.instance_generation = running.launched_generation;
    f.services.handle(miss);
    const auto failed = f.services.query("worker").value();
    require(failed.state == ServiceState::failed && failed.pid == 41 &&
            failed.generation != failed.launched_generation &&
            f.services.trySnapshotProcessIdentities()->front().instance_generation == running.launched_generation,
            "FAILED unreaped child omitted or relabeled with fault generation");
}

void test_sm_try_lock_busy_skips_io_and_recovers_after_launch() {
    IdentityFixture f;
    f.add("worker");
    LaunchGate gate;
    f.processes.gate = &gate;
    std::exception_ptr writer_error;
    std::thread writer([&] { try { f.services.startService("worker"); } catch (...) { writer_error = std::current_exception(); } });
    struct ReleaseAndJoin {
        LaunchGate& gate;
        std::thread& writer;
        ~ReleaseAndJoin() { gate.release(); if (writer.joinable()) writer.join(); }
    } release{gate, writer};
    gate.wait(); // start() holds the real registry mutex here.
    const auto before = ResourceClock::now();
    const auto busy = f.services.trySnapshotProcessIdentities();
    const auto validation = f.services.tryValidateProcessIdentities({{"worker", 41, 1}});
    require(!busy && !validation && ResourceClock::now() - before < 1s, "try identity calls blocked on launch lock");
    unsigned reads = 0, validations = 0;
    ResourceCollector collector([&](const std::string&, std::size_t) { ++reads; return ProcReadResult{}; },
                                {}, ProcessPlatform{100, 4096});
    const auto scan = collector.collectProcesses(busy, [&](const auto& identities) {
        ++validations;
        return f.services.tryValidateProcessIdentities(identities);
    });
    require(scan.quality == MetricQuality::unavailable && scan.processes.empty() && reads == 0 && validations == 0,
            "busy registry scan performed I/O or repeated owner query");
    gate.release();
    writer.join();
    if (writer_error) std::rethrow_exception(writer_error);
    const auto captured = f.services.trySnapshotProcessIdentities();
    require(captured && captured->size() == 1 && f.services.tryValidateProcessIdentities(*captured)->size() == 1,
            "try identities did not recover after lock release");
}

void test_sm_proc_io_outside_lock_and_post_capture_launch_change() {
    IdentityFixture f;
    f.add("worker");
    f.services.startService("worker");
    const auto captured = f.services.trySnapshotProcessIdentities();
    require(captured && captured->size() == 1, "fixture capture failed");
    unsigned reads = 0;
    ResourceCollector collector([&](const std::string&, std::size_t) {
        ++reads;
        require(f.services.trySnapshotProcessIdentities().has_value(), "proc reader ran under SM lock");
        Event exit{EventType::process_exited, "worker", Clock::now(), 41, 0};
        exit.instance_generation = captured->front().instance_generation;
        f.services.handle(exit);
        f.services.startService("worker"); // Same PID, changed launch during I/O.
        return ProcReadResult{"41 (worker) S 0 0 0 0 0 0 0 0 0 0 100 50 0 0 0 0 0 0 1234 0 3\n", ProcReadError::none};
    }, {}, ProcessPlatform{100, 4096});
    const auto scan = collector.collectProcesses(captured, [&](const auto& identities) {
        return f.services.tryValidateProcessIdentities(identities);
    });
    require(reads == 1 && scan.quality == MetricQuality::unavailable && scan.processes.empty() &&
            f.processes.launches == 2 && f.processes.reaps == 0,
            "late old launch resource rows accepted or collector reaped children");
}


SystemResourceSnapshot measured(double cpu, double memory, Clock::time_point at = Clock::time_point{}) {
    SystemResourceSnapshot snapshot;
    snapshot.sampled_at = at;
    snapshot.cpu = {MetricQuality::valid, cpu, at, 0};
    snapshot.memory = {MetricQuality::valid, MemoryResourceUsage{1000, 0, memory}, at, 0};
    return snapshot;
}

void test_policy_hysteresis_change_only_metadata_and_direct_clear() {
    std::vector<RuntimeEvent> facts;
    Monitor monitor([](Event) {}, 5s, [&](RuntimeEvent fact) { facts.push_back(std::move(fact)); });
    const auto at = Clock::time_point{} + 10s;
    monitor.observeResources(measured(79, 79, at));
    require(facts.size() == 2 && !facts[0].active && !facts[1].active, "first normal sample must initialize both sources");
    monitor.observeResources(measured(80, 80, at));
    require(facts.size() == 4 && facts[2].active && facts[3].active, "inclusive warning thresholds");
    for (double value : {79.9, 80.1, 79.8, 80.2, 100.0}) monitor.observeResources(measured(value, 80, at));
    require(facts.size() == 4 && facts[2].severity == ResourceSeverity::warning, "CPU jitter/full usage must retain WARNING only");
    monitor.observeResources(measured(79, 95, at));
    require(facts.size() == 5 && facts.back().source == "memory_monitor" && facts.back().active &&
            facts.back().severity == ResourceSeverity::critical, "95 must replace same source with one critical fact");
    monitor.observeResources(measured(79, 94.9, at));
    require(facts.size() == 5, "94.9 must preserve critical latch");
    monitor.observeResources(measured(79, 90, at));
    require(facts.size() == 6 && facts.back().active && facts.back().severity == ResourceSeverity::warning,
            "90 must downgrade without an intermediate clear");
    monitor.observeResources(measured(79, 76, at));
    require(facts.size() == 6, "76 must retain memory warning");
    monitor.observeResources(measured(75, 75, at));
    require(facts.size() == 8 && !facts[6].active && !facts[7].active, "inclusive clear thresholds");
    monitor.observeResources(measured(75, 95, at));
    monitor.observeResources(measured(75, 75, at));
    require(facts.size() == 10 && facts[8].severity == ResourceSeverity::critical && !facts[9].active,
            "critical to direct clear must publish just one memory clear");
    for (const auto& fact : facts)
        require(fact.type == RuntimeEventType::resource_warning && fact.service_name.empty() && fact.at == at &&
                !fact.generation && !fact.recovery_context && !fact.reason.empty() &&
                (fact.source == "cpu_monitor" || fact.source == "memory_monitor") &&
                (fact.active || fact.severity == ResourceSeverity::warning), "resource fact metadata/ownership");
}

void test_policy_partial_validity_invalid_latch_and_atomic_input_validation() {
    std::vector<RuntimeEvent> facts;
    Monitor monitor([](Event) {}, 5s, [&](RuntimeEvent fact) { facts.push_back(std::move(fact)); });
    auto snapshot = measured(0, 95);
    snapshot.cpu = {MetricQuality::warming_up, {}, {}, 0};
    monitor.observeResources(snapshot);
    require(facts.size() == 1 && facts.front().source == "memory_monitor" && facts.front().active,
            "first CPU baseline must not publish clear; memory must remain independent");
    snapshot.cpu.quality = MetricQuality::unavailable;
    snapshot.memory = {MetricQuality::unavailable, {}, {}, 3};
    for (int i = 0; i < 3; ++i) monitor.observeResources(snapshot);
    require(facts.size() == 1, "invalid samples cleared confirmed latch or invented initialization");
    snapshot = measured(100, 0);
    snapshot.memory.value->used_percent = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { monitor.observeResources(snapshot); }, "malformed typed valid input accepted");
    snapshot.memory.value.reset();
    rejects([&] { monitor.observeResources(snapshot); }, "valid metric without value accepted");
    for (double invalid : {-1.0, 101.0, std::numeric_limits<double>::infinity()}) {
        rejects([&] { monitor.report_resources(100, invalid); }, "invalid memory accepted after valid CPU");
        rejects([&] { monitor.report_resources(invalid, 0); }, "invalid CPU accepted before valid memory");
    }
    require(facts.size() == 1, "invalid pair partially advanced/published CPU policy");
    monitor.observeResources(measured(100, 94.9));
    require(facts.size() == 2 && facts.back().source == "cpu_monitor" && facts.back().active,
            "failed validation advanced CPU state or invalid sample cleared memory critical");
    snapshot = measured(0, 75);
    snapshot.cpu = {MetricQuality::warming_up, {}, {}, 0};
    monitor.observeResources(snapshot);
    require(facts.size() == 3 && !facts.back().active && facts.back().source == "memory_monitor",
            "valid memory recovery must not clear warming CPU latch");
    monitor.observeResources(measured(75, 75));
    require(facts.size() == 4 && facts.back().source == "cpu_monitor" && !facts.back().active,
            "valid CPU recovery did not clear retained latch");
}

void test_policy_legacy_derived_clear_explicit_validation_and_unlocked_sink() {
    std::vector<RuntimeEvent> facts;
    Monitor* observer = nullptr;
    Monitor monitor([](Event) {}, 5s, [&](RuntimeEvent fact) {
        facts.push_back(fact);
        observer->heartbeat("idle", fact.at);
        // Same policy state: this reentrant query/observation cannot acquire a held policy mutex.
        observer->report_resources(60, 90, fact.at);
    }, ResourceThresholds{60, 70, 90});
    observer = &monitor;
    monitor.watch("idle", Clock::time_point{}, 15s);
    monitor.report_resources(60, 90);
    require(facts.size() == 2, "sink must run outside heartbeat and resource mutexes");
    std::vector<RuntimeEvent> transitions;
    Monitor derived([](Event) {}, 5s, [&](RuntimeEvent fact) { transitions.push_back(std::move(fact)); },
                    ResourceThresholds{60, 70, 90});
    derived.report_resources(60, 90);
    derived.report_resources(59.9, 89.9);
    require(transitions.size() == 2, "legacy clear derivation lost hysteresis");
    derived.report_resources(55, 85);
    require(transitions.size() == 4 && !transitions[2].active && transitions[3].active &&
            transitions[3].severity == ResourceSeverity::warning, "legacy derived clear/downgrade boundaries");
    derived.report_resources(55, 65);
    require(transitions.size() == 5 && !transitions.back().active, "legacy memory derived clear");
    Monitor low([](Event) {}, 5s, [](RuntimeEvent) {}, ResourceThresholds{1, 1, 2});
    low.report_resources(0, 0);
    rejects([] { Monitor invalid([](Event) {}, 5s, {}, ResourceThresholds{80, 80, 95, 80, 75, 90}); },
            "equal CPU clear accepted");
    rejects([] { Monitor invalid([](Event) {}, 5s, {}, ResourceThresholds{80, 80, 95, 75, 80, 90}); },
            "equal memory clear accepted");
    rejects([] { Monitor invalid([](Event) {}, 5s, {}, ResourceThresholds{80, 80, 95, 75, 75, 79}); },
            "critical clear below warning accepted");
    rejects([] { Monitor invalid([](Event) {}, 5s, {}, ResourceThresholds{80, 80, 95, 75, 75, 95}); },
            "critical clear equal critical accepted");
    rejects([] { Monitor invalid([](Event) {}, 5s, {}, ResourceThresholds{80, 80, 95,
        std::numeric_limits<double>::quiet_NaN(), 75, 90}); }, "nonfinite clear accepted");
}

void test_sampling_deadlines_boundaries_backwards_and_slow_completion() {
    ResourceSamplingSchedule schedule(2s);
    const auto at = Clock::time_point{};
    require(schedule.due(at), "first legal tick must be due");
    schedule.completed(at);
    require(!schedule.due(at) && !schedule.due(at - 1s) && !schedule.due(at + 1999ms) && schedule.due(at + 2s),
            "due boundary/zero elapsed/backwards clock");
    require(schedule.due(at + 100s), "missed cycles must permit one current sample");
    schedule.completed(at + 109s); // Completion after a slow cycle, not old queued tick time.
    require(!schedule.due(at + 109s) && !schedule.due(at + 110s) && schedule.due(at + 111s),
            "slow cycle must postpone instead of catching up");
    unsigned admitted = 0;
    for (int tick = 0; tick < 100; ++tick) if (schedule.tryQueueTick()) ++admitted;
    require(admitted == 1, "blocked worker admitted more than one pending timer tick");
    schedule.consumeTick();
    require(schedule.tryQueueTick() && !schedule.tryQueueTick(), "consumed tick did not release pending slot");
    rejects([] { ResourceSamplingSchedule invalid(0s); }, "zero interval accepted");
    rejects([] { ResourceSamplingSchedule invalid(61s); }, "out of range interval accepted");
}

RuntimeConfig idle_runtime_config() {
    RuntimeConfig config;
    ServiceConfig idle;
    idle.service_name = "idle";
    idle.executable = "/bin/true";
    config.services.push_back(idle);
    config.monitoring.sample_interval_seconds = 1s;
    return config;
}

void test_runtime_modes_reserved_sources_envelope_and_pre_run_fifo() {
    unsigned reads = 0;
    auto reader = [&](const std::string&, std::size_t) { ++reads; return ProcReadResult{}; };
    auto config = idle_runtime_config();
    config.monitoring.memory_warning = 85;
    RuntimeManager native(config, {}, {}, ResourceInputMode::native, reader);
    require(!native.queryResourceSnapshot(), "native cache must be absent before first collection");
    rejects([&] { native.reportResourceUsage(0, 95); }, "native external report accepted");
    for (const auto* source : {"cpu_monitor", "memory_monitor"}) {
        RuntimeEvent fact{RuntimeEventType::resource_warning, {}, source, "forged native source", Clock::now()};
        rejects([&] { native.post(fact); }, "native public RuntimeEvent reserved source accepted");
        Event envelope{EventType::runtime_event, {}, Clock::now()};
        envelope.runtime_event = fact;
        rejects([&] { native.post(envelope); }, "native public Event envelope bypassed source reservation");
        envelope.type = EventType::health_check;
        rejects([&] { native.post(envelope); }, "alternate envelope bypassed source reservation");
    }
    RuntimeEvent custom{RuntimeEventType::resource_warning, {}, "deployment_pressure", "independent source", Clock::now()};
    custom.severity = ResourceSeverity::critical;
    native.post(custom);
    native.post(Event{EventType::shutdown, {}, Clock::now()});
    native.run();
    require(reads == 0 && !native.queryResourceSnapshot() && native.queryDeviceState().current == DeviceState::error &&
            native.queryDeviceState().source == "deployment_pressure", "early shutdown/custom ingress/native ownership");
    RuntimeManager external(config, {}, {}, ResourceInputMode::external, reader);
    external.reportResourceUsage(0, 84); // Full config warning=85, no legacy override accepted by this overload.
    external.reportResourceUsage(0, 85);
    external.post(Event{EventType::shutdown, {}, Clock::now()});
    external.run();
    require(reads == 0 && external.queryDeviceState().current == DeviceState::warning && !external.queryResourceSnapshot(),
            "external mode auto-collected or ignored complete config thresholds/FIFO");
    ConfigFile file;
    file.write(single + R"(,"monitoring":{"cpu_warning":99,"memory_warning":90}})");
    RuntimeManager legacy(file.path.string(), {}, {}, ResourceThresholds{60, 70, 90});
    legacy.reportResourceUsage(60, 70);
    legacy.post(Event{EventType::shutdown, {}, Clock::now()});
    legacy.run();
    require(legacy.queryDeviceState().current == DeviceState::warning && !legacy.queryResourceSnapshot(),
            "legacy external threshold argument was overridden by file monitoring");
    config.monitoring.cpu_clear = config.monitoring.cpu_warning;
    rejects([&] { RuntimeManager invalid(config); }, "programmatic native config not validated");
}

template <typename Predicate>
void eventually(Predicate predicate, const char* message) {
    const auto deadline = Clock::now() + 4s;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(5ms);
    }
}

// Release is guaranteed before join on every failure path, including assertions.
struct RuntimeRunner {
    RuntimeManager& manager;
    std::function<void()> release;
    std::exception_ptr failure;
    std::atomic<bool> finished{false};
    std::thread writer;
    RuntimeRunner(RuntimeManager& core, std::function<void()> unblock = {}) : manager(core), release(std::move(unblock)),
        writer([this] { try { manager.run(); } catch (...) { failure = std::current_exception(); } finished = true; }) {}
    ~RuntimeRunner() {
        manager.post(Event{EventType::shutdown, {}, Clock::now()});
        if (release) release();
        if (writer.joinable()) writer.join();
    }
    void join() { if (writer.joinable()) writer.join(); if (failure) std::rethrow_exception(failure); }
};

struct CycleReader {
    std::mutex mutex;
    std::condition_variable ready;
    unsigned entered = 0, permitted = 0;
    bool released = false;
    std::atomic<int> seconds{2};
    std::atomic<unsigned> reads{0};
    unsigned completion_clocks = 0;
    std::atomic<unsigned> finished_cycle{0};
    Clock::time_point now() {
        const auto at = Clock::time_point{} + std::chrono::seconds{seconds.load()};
        std::lock_guard<std::mutex> lock(mutex);
        // After stat entry: system completion, bundle completion, then schedule completion.
        if (entered && ++completion_clocks == 3) finished_cycle = entered;
        return at;
    }
    void wait(unsigned cycle) {
        seconds = static_cast<int>(cycle * 2);
        std::unique_lock<std::mutex> lock(mutex);
        require(ready.wait_for(lock, 4s, [&] { return entered >= cycle; }), "worker did not reach collection gate");
        require(entered == cycle, "unexpected catch-up sample burst");
    }
    void allow(unsigned cycle) {
        std::lock_guard<std::mutex> lock(mutex);
        permitted = cycle;
        ready.notify_all();
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        ready.notify_all();
    }
    ProcReadResult read(const std::string& path, std::size_t) {
        ++reads;
        if (path == "/proc/stat") {
            std::unique_lock<std::mutex> lock(mutex);
            const auto cycle = ++entered;
            completion_clocks = 0;
            ready.notify_all();
            require(ready.wait_for(lock, 8s, [&] { return released || permitted >= cycle; }), "reader gate not released");
            if (cycle == 2) return {{}, ProcReadError::permission_denied};
            if (cycle == 1 || cycle == 3) return {"cpu 100 0 0 100\n", ProcReadError::none};
            if (cycle == 4) return {"cpu 200 0 0 100\n", ProcReadError::none};
            return {"cpu 275 0 0 125\n", ProcReadError::none};
        }
        require(path == "/proc/meminfo", "unmanaged process or extra proc path read");
        if (entered == 2) return {{}, ProcReadError::io_error};
        const int available = entered == 1 ? 50 : entered == 3 ? 100 : 250;
        return {"MemTotal: 1000 kB\nMemAvailable: " + std::to_string(available) + " kB\n", ProcReadError::none};
    }
};

void test_native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown() {
    CycleReader reader;
    RuntimeManager manager(idle_runtime_config(), {}, {}, ResourceInputMode::native,
        [&](const auto& path, auto limit) { return reader.read(path, limit); }, [&] { return reader.now(); });
    RuntimeRunner run(manager, [&] { reader.release(); });
    auto collect = [&](unsigned cycle) {
        reader.wait(cycle);
        reader.allow(cycle);
        eventually([&] { const auto cache = manager.queryResourceSnapshot();
            return cache && reader.finished_cycle >= cycle &&
                cache->snapshot.collected_at == Clock::time_point{} + std::chrono::seconds{cycle * 2};
        }, "native cache not submitted");
    };
    collect(1);
    eventually([&] { return manager.queryDeviceState().current == DeviceState::error; }, "native critical memory not aggregated");
    auto first = manager.queryResourceSnapshot(Clock::time_point{} + 5s).value();
    require(first.snapshot.system.cpu.quality == MetricQuality::warming_up && !first.snapshot.system.cpu.value &&
            !first.cpu_age && !first.cpu_stale && first.memory_age == 3s && !first.memory_stale &&
            first.snapshot.process_scan_quality == MetricQuality::valid && first.snapshot.processes.empty(),
            "first baseline/empty eligible scan/query age boundary");
    require(manager.queryResourceSnapshot(Clock::time_point{} + 5001ms)->memory_stale &&
            manager.queryResourceSnapshot(Clock::time_point{})->memory_age == Clock::duration::zero(),
            "stale must use strict >3 intervals; backwards query must clamp age");
    collect(2);
    const auto invalid = manager.queryResourceSnapshot(Clock::time_point{} + 6s).value();
    require(invalid.snapshot.system.cpu.quality == MetricQuality::unavailable &&
            invalid.snapshot.system.memory.quality == MetricQuality::unavailable && !invalid.snapshot.system.memory.value &&
            invalid.snapshot.system.memory.consecutive_errors == 1 && invalid.memory_stale &&
            manager.queryDeviceState().current == DeviceState::error, "failed collection cleared latch or reused last good value");
    collect(3);
    eventually([&] { return manager.queryDeviceState().current == DeviceState::warning; }, "valid memory recovery did not downgrade");
    require(manager.queryResourceSnapshot()->snapshot.system.cpu.quality == MetricQuality::warming_up,
            "invalid CPU baseline not cleared");
    collect(4);
    eventually([&] { return manager.queryDeviceState().current == DeviceState::warning; }, "CPU activation/memory clear not drained");
    require(manager.queryDeviceState().current == DeviceState::warning && manager.queryResourceSnapshot()->snapshot.system.cpu.value == 100,
            "memory clear erased another source or CPU became critical");
    collect(5);
    eventually([&] { return manager.queryDeviceState().current == DeviceState::ready; }, "clear must reaggregate inactive services to READY");
    reader.wait(6); // Hold the next cycle in proc I/O; last submitted snapshot is immutable.
    const auto reads = reader.reads.load();
    std::atomic<bool> good{true};
    std::vector<std::thread> queries;
    for (int i = 0; i < 4; ++i) queries.emplace_back([&] {
        for (int j = 0; j < 50; ++j) {
            auto copy = manager.queryResourceSnapshot(Clock::time_point{} + 10s);
            if (!copy || copy->snapshot.collected_at != Clock::time_point{} + 10s || copy->snapshot.system.cpu.value != 75)
                good = false;
            if (copy) copy->snapshot.system.cpu.value = 0;
        }
    });
    for (auto& query : queries) query.join();
    require(good && reader.reads == reads && manager.queryResourceSnapshot()->snapshot.system.cpu.value == 75,
            "concurrent query performed I/O or changed cached measurement");
    const auto before = manager.queryDeviceState();
    manager.post(Event{EventType::shutdown, {}, Clock::now()});
    reader.release();
    run.join();
    require(manager.queryResourceSnapshot()->snapshot.collected_at == Clock::time_point{} + 10s &&
            manager.queryDeviceState().current == before.current && manager.queryDeviceState().previous == before.previous &&
            manager.queryDeviceState().timestamp == before.timestamp && manager.queryDeviceState().source == before.source &&
            manager.queryDeviceState().reason == before.reason &&
            manager.query("idle")->pid <= 0, "inflight shutdown committed late snapshot/facts or skipped cleanup");
}

void test_native_process_not_present_is_measurement_only_and_shutdown_discards_row() {
    auto config = idle_runtime_config();
    auto& service = config.services.front();
    service.executable = "/bin/sleep";
    service.arguments = {"30"};
    service.autostart = true;
    service.heartbeat_timeout = 3600s;
    service.shutdown_timeout = 1s;
    service.restart_policy = RestartPolicy::on_failure;
    LaunchGate gate;
    unsigned process_reads = 0;
    RuntimeManager manager(config, {}, {}, ResourceInputMode::native,
        [&](const std::string& path, std::size_t limit) -> ProcReadResult {
            if (path == "/proc/stat") return {"cpu 1 0 0 1\n", ProcReadError::none};
            if (path == "/proc/meminfo") return {"MemTotal: 1000 kB\nMemAvailable: 1000 kB\n", ProcReadError::none};
            require(limit == ResourceCollector::process_file_limit, "process read exceeded frozen bound");
            if (++process_reads == 2) gate.pause();
            return {{}, ProcReadError::not_present}; // Reader observation does not imply real child exit.
        });
    RuntimeRunner run(manager, [&] { gate.release(); });
    eventually([&] { auto view = manager.queryResourceSnapshot();
        return view && view->snapshot.process_scan_quality == MetricQuality::valid &&
            view->snapshot.processes.size() == 1;
    }, "not_present process row was not published");
    const auto first = manager.queryResourceSnapshot().value();
    const auto initial = manager.query("idle").value();
    const auto& row = first.snapshot.processes.front();
    require(initial.state == ServiceState::running && initial.pid > 0 && initial.restart_count == 0 &&
            row.pid == initial.pid && row.instance_generation == initial.launched_generation &&
            row.observation_status == ProcessObservationStatus::not_present && !row.cpu_percent && !row.rss_bytes,
            "proc ENOENT changed lifecycle or retained stale values");
    gate.wait(); // Next process read is in flight, after system read and SM capture.
    const auto before = manager.queryDeviceState();
    manager.post(Event{EventType::shutdown, {}, Clock::now()});
    gate.release();
    run.join();
    require(process_reads == 2 && manager.queryResourceSnapshot()->snapshot.collected_at == first.snapshot.collected_at &&
            manager.queryDeviceState().timestamp == before.timestamp && manager.query("idle")->restart_count == 0 &&
            manager.query("idle")->pid == -1, "inflight process row published after stop or created recovery work");
    int raw_status = 0;
    errno = 0;
    require(::waitpid(initial.pid, &raw_status, WNOHANG) == -1 && errno == ECHILD,
            "native shutdown did not leave exclusive owner reaping intact");
}

void test_native_identity_change_query_unavailable_and_signal_shutdown() {
    auto config = idle_runtime_config();
    config.monitoring.sample_interval_seconds = 60s;
    auto& service = config.services.front();
    service.executable = "/bin/sleep";
    service.arguments = {"30"};
    service.autostart = true;
    service.heartbeat_timeout = 3600s;
    service.shutdown_timeout = 1s;
    LaunchGate gate;
    unsigned reads = 0;
    struct sigaction before{}, after{};
    require(::sigaction(SIGTERM, nullptr, &before) == 0, "cannot inspect handler before native signal run");
    RuntimeManager manager(config, {}, {}, ResourceInputMode::native,
        [&](const std::string& path, std::size_t) -> ProcReadResult {
            ++reads;
            if (path == "/proc/stat") return {"cpu 1 0 0 1\n", ProcReadError::none};
            if (path == "/proc/meminfo") return {"MemTotal: 1000 kB\nMemAvailable: 1000 kB\n", ProcReadError::none};
            gate.pause();
            return {{}, ProcReadError::not_present};
        });
    RuntimeRunner run(manager, [&] { gate.release(); });
    gate.wait();
    const auto captured = manager.query("idle").value();
    require(captured.pid > 0 && !manager.queryResourceSnapshot(), "scan gate did not precede cache commit");
    manager.post(Event{EventType::stop, "idle", Clock::now()});
    eventually([&] { return manager.query("idle")->pid == -1; }, "writer could not reap while proc reader was blocked");
    gate.release();
    eventually([&] { return manager.queryResourceSnapshot().has_value(); }, "rejected identity scan did not publish unavailable quality");
    const auto view = manager.queryResourceSnapshot().value();
    require(view.snapshot.process_scan_quality == MetricQuality::unavailable && view.snapshot.processes.empty() &&
            view.snapshot.system.memory.quality == MetricQuality::valid && manager.query("idle")->restart_count == 0,
            "identity revalidation published late row or corrupted independent system sample");
    require(::raise(SIGTERM) == 0, "cannot inject native shutdown signal");
    eventually([&] { return run.finished.load(); }, "native signal shutdown did not join workers");
    run.join();
    const auto count = reads;
    require(manager.queryResourceSnapshot()->snapshot.collected_at == view.snapshot.collected_at && reads == count &&
            manager.query("idle")->state == ServiceState::stopped && manager.query("idle")->pid == -1,
            "native signal teardown changed frozen snapshot or left child alive");
    require(::sigaction(SIGTERM, nullptr, &after) == 0 && before.sa_handler == after.sa_handler,
            "native signal run did not restore old handler");
}

void test_native_observer_failure_stops_sampling_and_restores_signal_handler() {
    struct sigaction before{}, after{};
    require(::sigaction(SIGTERM, nullptr, &before) == 0, "cannot inspect handler before native run");
    std::atomic<unsigned> reads{0};
    RuntimeManager manager(idle_runtime_config(), {}, [](const DeviceStateSnapshot&) {
        throw std::runtime_error("native initialization observer failure");
    }, ResourceInputMode::native, [&](const std::string&, std::size_t) -> ProcReadResult {
        ++reads;
        return {{}, ProcReadError::io_error};
    });
    rejects([&] { manager.run(); }, "native observer failure was swallowed");
    const auto count = reads.load();
    const auto snapshot = manager.queryResourceSnapshot();
    require(manager.query("idle")->state == ServiceState::stopped && manager.query("idle")->pid == -1 &&
            (!snapshot || (snapshot->snapshot.system.cpu.quality == MetricQuality::unavailable &&
                           snapshot->snapshot.system.memory.quality == MetricQuality::unavailable)) && reads == count,
            "native observer failure skipped joined cleanup or published a fake snapshot");
    require(::sigaction(SIGTERM, nullptr, &after) == 0 && before.sa_handler == after.sa_handler,
            "native failure left signal handler installed");
}

void test_native_unexpected_worker_exception_shutdown_and_reader_lifetime() {
    std::weak_ptr<int> lifetime;
    std::atomic<unsigned> reads{0};
    {
        auto token = std::make_shared<int>(1);
        lifetime = token;
        RuntimeManager manager(idle_runtime_config(), {}, {}, ResourceInputMode::native,
            [token, &reads](const std::string&, std::size_t) -> ProcReadResult {
                ++reads;
                throw std::runtime_error("unexpected injected reader failure");
            });
        token.reset();
        require(!lifetime.expired(), "reader capture destroyed before worker");
        RuntimeRunner run(manager);
        eventually([&] { return run.finished.load(); }, "worker exception did not request joined shutdown");
        rejects([&] { run.join(); }, "unexpected worker failure was swallowed");
        require(reads == 1 && !manager.queryResourceSnapshot() && !lifetime.expired() && manager.query("idle")->pid <= 0,
                "worker exception published fake snapshot or destroyed reader before join");
    }
    require(lifetime.expired(), "collector reader capture leaked after runtime destruction");
}

} // namespace

int main() {
    try {
        test_config_legacy_roots_defaults_wrapper_and_service_fields();
        test_config_seven_fields_and_partial_fixed_defaults();
        test_config_json_boundaries_and_threshold_order();
        test_config_wrong_types_unknown_duplicate_float_and_overflow();
        test_config_programmatic_fractional_finite_and_legacy_thresholds();
        test_config_preserves_service_validation_and_file_limit();
        test_sm_identities_launch_token_cleanup_and_read_only_validation();
        test_sm_try_lock_busy_skips_io_and_recovers_after_launch();
        test_sm_proc_io_outside_lock_and_post_capture_launch_change();
        test_policy_hysteresis_change_only_metadata_and_direct_clear();
        test_policy_partial_validity_invalid_latch_and_atomic_input_validation();
        test_policy_legacy_derived_clear_explicit_validation_and_unlocked_sink();
        test_sampling_deadlines_boundaries_backwards_and_slow_completion();
        test_runtime_modes_reserved_sources_envelope_and_pre_run_fifo();
        test_native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown();
        test_native_unexpected_worker_exception_shutdown_and_reader_lifetime();
        test_native_process_not_present_is_measurement_only_and_shutdown_discards_row();
        test_native_observer_failure_stops_sampling_and_restores_signal_handler();
        test_native_identity_change_query_unavailable_and_signal_shutdown();
        std::cout << "resource monitoring config/identity/policy/runtime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
