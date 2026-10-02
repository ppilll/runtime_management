#include "runtime/runtime_manager.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace runtime;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string quoted(const std::string& value) {
    std::string result = "\"";
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') result += '\\';
        result += ch;
    }
    return result + '"';
}

struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("phase4_coordination_" + std::to_string(::getpid()) + "_" +
         std::to_string(Clock::now().time_since_epoch().count()));
    std::vector<DeviceStateSnapshot> changes; // Written by writer, read after join.
    std::vector<int> running_pids;
    std::unique_ptr<RuntimeManager> manager;
    std::thread writer;
    std::exception_ptr error;
    std::string service_name;
    std::function<void(const DeviceStateSnapshot&)> observer;

    explicit Fixture(const std::string& mode, unsigned timeout = 20, unsigned heartbeat = 3600,
                     std::string name = "control_service") : service_name(std::move(name)) {
        std::filesystem::create_directory(directory);
        const auto config_path = directory / "config.json";
        const auto executable = mode == "missing" ? "/missing/phase4/service" :
            std::filesystem::read_symlink("/proc/self/exe").string();
        std::ofstream config(config_path);
        config << "{\"service_name\":" << quoted(service_name) << ",\"executable\":" << quoted(executable)
               << ",\"arguments\":[\"--child\"," << quoted(mode) << ',' << quoted((directory / "launches").string())
               << "],\"autostart\":false,\"restart_policy\":\"on-failure\",\"shutdown_timeout\":1,"
               << "\"heartbeat_timeout\":" << heartbeat << ",\"recovery_timeout\":" << timeout << '}';
        config.close();
        require(static_cast<bool>(config), "cannot write coordination config");
        manager = std::make_unique<RuntimeManager>(config_path.string(), AggregationOptions{},
            [this](const DeviceStateSnapshot& change) {
                changes.push_back(change);
                if (change.current == DeviceState::running) running_pids.push_back(status().pid);
                if (observer) observer(change);
            });
    }
    void run() { writer = std::thread([this] { try { manager->run(); } catch (...) { error = std::current_exception(); } }); }
    void finish() {
        manager->post(Event{EventType::shutdown, {}, Clock::now()});
        if (writer.joinable()) writer.join();
        if (error) std::rethrow_exception(error);
    }
    ~Fixture() {
        if (writer.joinable()) {
            manager->post(Event{EventType::shutdown, {}, Clock::now()});
            writer.join();
        }
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    template <typename Predicate>
    void wait(Predicate predicate, std::chrono::seconds budget = 15s) {
        const auto deadline = Clock::now() + budget;
        while (!predicate()) {
            require(Clock::now() < deadline, "coordination condition timed out");
            std::this_thread::sleep_for(20ms);
        }
    }
    ServiceStatus status() const { return *manager->query(service_name); }
    void start() { manager->post(Event{EventType::start, service_name, Clock::now()}); }
};

// Explicit observer checkpoint; release guard must outlive all test operations
// and be destroyed before Fixture joins the writer on an assertion failure.
struct WriterGate {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false;
    bool released = false;
    void pause() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        ready.notify_all();
        require(ready.wait_for(lock, 15s, [&] { return released; }), "writer gate release timed out");
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(ready.wait_for(lock, 12s, [&] { return entered; }), "writer did not reach gate");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        ready.notify_all();
    }
};
struct GateRelease {
    WriterGate& gate;
    ~GateRelease() { gate.release(); }
};

void test_real_crash_candidate_success_and_reap() {
    Fixture f("crash_once");
    f.start();
    f.run();
    f.wait([&] { return f.status().restart_count == 1 && f.status().state == ServiceState::running &&
                        f.manager->queryDeviceState().current == DeviceState::running; });
    const auto replacement = f.status();
    require(replacement.generation == replacement.launched_generation && replacement.generation == 3 &&
            replacement.pid > 0, "fault-to-launch generation bridge failed");
    f.finish();
    const std::vector<DeviceState> expected{DeviceState::ready, DeviceState::running, DeviceState::error,
                                           DeviceState::recovering, DeviceState::running};
    std::vector<DeviceState> actual;
    for (const auto& change : f.changes) actual.push_back(change.current);
    require(actual == expected && f.changes.back().source == "recovery_manager",
            "candidate emitted premature RUNNING or canonical recovery path changed");
    require(f.status().pid == -1 && f.status().launched_generation == 0,
            "shutdown returned before the replacement was reaped");
    int raw_status = 0;
    require(f.running_pids.size() == 2 && ::waitpid(f.running_pids.front(), &raw_status, WNOHANG) == -1 &&
            errno == ECHILD, "old crash child was not reaped by its exclusive owner");
    require(f.manager->queryDeviceState().current == DeviceState::running, "shutdown mutated device health");
}

void test_real_retry_exhaustion_terminal_once() {
    Fixture f("missing", 100);
    f.start();
    f.run();
    f.wait([&] { return f.manager->queryDeviceState().current == DeviceState::offline; }, 90s);
    const auto terminal = f.status();
    require(terminal.state == ServiceState::failed && terminal.restart_count == 5 && terminal.generation == 12,
            "six launches/five reservations did not terminate exactly at the cap");
    f.manager->post(Event{EventType::start, "control_service", Clock::now()});
    f.finish();
    require(std::count_if(f.changes.begin(), f.changes.end(), [](const auto& change) {
        return change.current == DeviceState::offline && change.reason == "automatic restart budget exhausted";
    }) == 1, "exhaustion missing or duplicated");
    require(f.status().restart_count == 5 && f.manager->queryDeviceState().current == DeviceState::offline,
            "explicit START reset budget or exited OFFLINE");
}

void test_real_timeout_before_due_without_launch() {
    Fixture f("missing", 1);
    f.start();
    f.run();
    f.wait([&] { return f.manager->queryDeviceState().current == DeviceState::offline; });
    require(f.status().generation == 3 && f.status().restart_count == 1 && f.status().pid == -1,
            "deadline did not finalize the backoff before replacement launch");
    f.finish();
    require(f.changes.back().reason == "recovery timeout" && f.changes.back().source == "recovery_manager",
            "timeout did not originate from canonical RM receipt");
}

void test_real_heartbeat_cleanup_before_replacement() {
    Fixture f("stubborn", 20, 1);
    f.start();
    f.run();
    f.wait([&] {
        unsigned ready = 0;
        std::ifstream(f.directory / "launches") >> ready;
        return ready > 0 && f.status().state == ServiceState::running;
    });
    const auto initial = f.status();
    f.wait([&] { return f.status().restart_count == 1 && f.status().state == ServiceState::recovering; });
    const auto fault = f.status();
    require(fault.generation > initial.generation &&
            (fault.pid <= 0 || fault.launched_generation == initial.launched_generation),
            "heartbeat fault relabelled an unreaped child");
    f.wait([&] { return f.status().state == ServiceState::running && f.status().restart_count == 1 &&
                        f.manager->queryDeviceState().current == DeviceState::running; });
    require(f.status().launched_generation > initial.launched_generation,
            "replacement reused old instance generation");
    f.finish();
    require(f.status().pid == -1 && f.manager->queryDeviceState().current == DeviceState::running,
            "stubborn child cleanup or heartbeat recovery latch failed");
}

void test_shutdown_backoff_and_manual_stop_fifo() {
    Fixture backoff("missing");
    backoff.start();
    backoff.run();
    backoff.wait([&] { return backoff.status().state == ServiceState::recovering; });
    backoff.finish();
    require(backoff.status().generation == 3 && backoff.status().restart_count == 1 &&
            backoff.status().pid == -1 && backoff.manager->queryDeviceState().current == DeviceState::recovering,
            "shutdown launched a replacement or changed the last snapshot");

    Fixture manual("stubborn");
    manual.manager->post(Event{EventType::restart_request, "control_service", Clock::now()});
    manual.manager->post(Event{EventType::stop, "control_service", Clock::now()});
    manual.manager->post(Event{EventType::shutdown, {}, Clock::now()});
    manual.run();
    manual.finish();
    // CREATED manual restart is legitimately executed before the next queued
    // STOP. After STOP is accepted it must be reaped without any second launch.
    require(manual.status().state == ServiceState::stopped && manual.status().pid == -1 &&
            manual.status().restart_count == 0 && manual.status().generation <= 3,
            "STOP failed to revoke the manual binding or consumed automatic budget");
}

void test_real_manual_restart_and_stale_instance_facts() {
    Fixture f("stubborn");
    f.start();
    f.run();
    f.wait([&] {
        unsigned ready = 0;
        std::ifstream input(f.directory / "launches");
        input >> ready;
        return ready > 0 && f.status().state == ServiceState::running;
    });
    const auto initial = f.status();
    f.manager->post(Event{EventType::restart_request, "control_service", Clock::now()});
    f.wait([&] { return f.status().generation > initial.generation && f.status().state == ServiceState::running &&
                        f.manager->queryDeviceState().current == DeviceState::running; });
    const auto replacement = f.status();
    require(replacement.generation == 3 && replacement.restart_count == 0,
            "manual transaction duplicated its launch or changed automatic budget");
    Event late_exit{EventType::process_exited, "control_service", Clock::now() + 1h, replacement.pid, 9};
    late_exit.instance_generation = initial.launched_generation;
    Event late_health{EventType::health_missed, "control_service", Clock::now() + 1h, replacement.pid, 0, 100};
    late_health.instance_generation = initial.launched_generation;
    f.manager->post(late_exit);
    f.manager->post(late_health);
    f.finish();
    int raw_status = 0;
    require(::waitpid(initial.pid, &raw_status, WNOHANG) == -1 && errno == ECHILD,
            "manual restart did not reap its old child");
    require(f.status().restart_count == 0 && f.manager->queryDeviceState().current == DeviceState::running &&
            std::none_of(f.changes.begin(), f.changes.end(), [](const auto& change) {
                return change.current == DeviceState::recovering;
            }), "stale instance affected replacement or manual restart emitted automatic START");
}

void test_manual_launch_failure_creates_one_automatic_episode() {
    Fixture f("missing");
    f.manager->post(Event{EventType::restart_request, "control_service", Clock::now()});
    f.run();
    f.wait([&] { return f.status().state == ServiceState::recovering && f.status().restart_count == 1; });
    f.manager->post(Event{EventType::stop, "control_service", Clock::now()});
    f.finish();
    require(f.status().generation == 4 && f.status().restart_count == 1 && f.status().pid == -1 &&
            f.manager->queryDeviceState().current == DeviceState::error,
            "manual failure did not create exactly one separately budgeted automatic episode");
    require(std::count_if(f.changes.begin(), f.changes.end(), [](const auto& change) {
        return change.current == DeviceState::recovering;
    }) == 1 && std::none_of(f.changes.begin(), f.changes.end(), [](const auto& change) {
        return change.current == DeviceState::offline;
    }), "manual failure emitted duplicate START/automatic terminal");
}

void test_real_dependency_stop_closure_requires_explicit_start() {
    Fixture f("stubborn");
    const auto config_path = f.directory / "dependencies.json";
    const auto executable = quoted(std::filesystem::read_symlink("/proc/self/exe").string());
    const auto definition = [&](const char* name, const std::string& dependencies) {
        return "{\"service_name\":" + quoted(name) + ",\"executable\":" + executable +
            ",\"arguments\":[\"--child\",\"stubborn\"," + quoted((f.directory / name).string()) +
            "],\"heartbeat_timeout\":3600,\"shutdown_timeout\":1,\"recovery_timeout\":20,"
            "\"restart_policy\":\"on-failure\",\"dependency\":" + dependencies + '}';
    };
    { std::ofstream config(config_path);
      config << "{\"services\":[" << definition("control_service", "[\"left\",\"right\",\"left\"]")
             << ',' << definition("left", "[\"vision_service\",\"vision_service\"]")
             << ',' << definition("right", "[\"vision_service\"]")
             << ',' << definition("vision_service", "[]") << ',' << definition("unrelated", "[]") << "]}"; }
    f.manager = std::make_unique<RuntimeManager>(config_path.string(), AggregationOptions{},
        [&](const DeviceStateSnapshot& change) { f.changes.push_back(change); });
    f.start();
    f.manager->post(Event{EventType::start, "unrelated", Clock::now()});
    f.run();
    f.wait([&] { return f.status().state == ServiceState::running &&
                        f.manager->query("vision_service")->state == ServiceState::running &&
                        f.manager->query("unrelated")->state == ServiceState::running; });
    f.wait([&] {
        for (const auto* name : {"control_service", "left", "right", "vision_service", "unrelated"}) {
            unsigned count = 0;
            std::ifstream(f.directory / name) >> count;
            if (count != 1) return false;
        }
        return true;
    });
    const auto unrelated = *f.manager->query("unrelated");
    const auto old = *f.manager->query("vision_service");
    require(::kill(old.pid, SIGKILL) == 0, "cannot crash prerequisite");
    f.wait([&] { const auto vision = f.manager->query("vision_service");
        return vision->restart_count == 1 && vision->state == ServiceState::running &&
               f.status().state == ServiceState::stopped &&
               f.manager->query("left")->state == ServiceState::stopped &&
               f.manager->query("right")->state == ServiceState::stopped; });
    require(f.manager->query("vision_service")->launched_generation > old.launched_generation &&
            f.status().restart_count == 0 && f.manager->queryDeviceState().current == DeviceState::error &&
            f.manager->query("unrelated")->pid == unrelated.pid &&
            f.manager->query("unrelated")->restart_count == 0,
            "prerequisite recovery revived a dependent or erased its critical unavailability");
    f.start();
    f.wait([&] { return f.status().state == ServiceState::running &&
                        f.manager->queryDeviceState().current == DeviceState::running; });
    f.wait([&] {
        for (const auto* name : {"control_service", "left", "right", "vision_service", "unrelated"}) {
            unsigned count = 0;
            std::ifstream(f.directory / name) >> count;
            if (count != (std::string(name) == "unrelated" ? 1u : 2u)) return false;
        }
        return true;
    }); // Each re-exec child has installed its TERM handler and written readiness.
    f.finish();
    for (const auto* name : {"control_service", "left", "right", "vision_service", "unrelated"}) {
        unsigned launches = 0;
        std::ifstream(f.directory / name) >> launches;
        require(launches == (std::string(name) == "unrelated" ? 1u : 2u) && f.manager->query(name)->pid == -1,
                "diamond duplicated/revived a node or did not reap its closure");
    }
    require(f.status().pid == -1 && f.manager->query("vision_service")->pid == -1,
            "dependency shutdown did not reap the full closure");
}

void test_untrusted_recovery_ingress_cannot_clear_resource() {
    Fixture f("stubborn");
    for (const auto type : {RuntimeEventType::recovery_start, RuntimeEventType::recovery_success,
                           RuntimeEventType::recovery_failed, RuntimeEventType::service_started}) {
        RuntimeEvent named{type, "control_service", "recovery_manager", "spoofed owner"};
        named.generation = 100;
        named.recovery_context = RecoveryContext{1, 1, 100, RecoveryOrigin::automatic_failure};
        f.manager->post(named);
        named.service_name.clear();
        f.manager->post(named);
    }
    RecoveryResult result;
    result.service_name = "control_service";
    result.recovery_generation = 1;
    result.outcome = RecoveryOutcome::success;
    f.manager->post(result);
    f.manager->post(RuntimeEvent{RuntimeEventType::resource_warning, {}, "memory_monitor",
        "critical pressure", Clock::now(), true, ResourceSeverity::critical});
    f.manager->post(DeviceStateEvent{DeviceStateEventType::recovery_failed, "recovery_manager", "bypass"});
    f.manager->post(Event{EventType::shutdown, {}, Clock::now()});
    f.run();
    f.finish();
    require(f.manager->queryDeviceState().current == DeviceState::error &&
            f.manager->queryDeviceState().source == "memory_monitor" && f.status().restart_count == 0,
            "unowned recovery bypassed the writer gate or resource ingress was lost");
}

void test_queue_late_success_duplicate_faults_and_terminal_gate() {
    WriterGate gate;
    Fixture f("crash_twice");
    GateRelease release{gate};
    unsigned recoveries = 0; // Only the writer touches this counter.
    f.observer = [&](const DeviceStateSnapshot& change) {
        if (change.current == DeviceState::recovering && ++recoveries == 2) gate.pause();
    };
    f.start();
    f.run();
    gate.wait(); // A recovered at e3, independent fault B is r2/f4.
    const auto before = f.manager->queryDeviceState();
    const auto fault = f.status();
    require(fault.generation == 4 && fault.restart_count == 2 && f.running_pids.size() == 2,
            "queue fixture did not reach the second independent fault");
    RecoveryResult old;
    old.service_name = f.service_name;
    old.recovery_generation = 1;
    old.initial_fault_generation = old.latest_fault_generation = 2;
    old.execution_generation = 3;
    old.launched_pid = f.running_pids.back();
    old.attempts_reserved_total = 1;
    old.outcome = RecoveryOutcome::success;
    old.terminal_reason = RecoveryTerminalReason::completed;
    old.completed_at = Clock::now() + 1h;
    for (const auto outcome : {RecoveryOutcome::success, RecoveryOutcome::failed, RecoveryOutcome::timeout}) {
        auto late = old;
        late.outcome = outcome;
        f.manager->post(late);
        f.manager->post(late); // Duplicate terminal cannot notify or close B.
    }
    auto future = old;
    future.recovery_generation = 3;
    f.manager->post(future);
    future.execution_generation.reset();
    f.manager->post(future);
    Event duplicate{EventType::process_exited, f.service_name, Clock::now() - 1h, old.launched_pid.value(), 7 << 8};
    duplicate.instance_generation = 3;
    for (unsigned count = 0; count < 32; ++count) f.manager->post(duplicate);
    // Source spelling and current fault token cannot authenticate a direct fact.
    RuntimeEvent forged{RuntimeEventType::recovery_success, f.service_name, "recovery_manager", "late success"};
    forged.generation = fault.generation;
    forged.recovery_context = RecoveryContext{2, 4, 5, RecoveryOrigin::automatic_failure};
    f.manager->post(forged);
    f.manager->post(Event{EventType::shutdown, {}, Clock::now()});
    gate.release();
    f.finish(); // FIFO drains every invalid envelope before accepting shutdown.
    const auto after = f.manager->queryDeviceState();
    require(after.current == before.current && after.previous == before.previous && after.source == before.source &&
            after.reason == before.reason && after.timestamp == before.timestamp && f.status().restart_count == 2 &&
            f.status().pid == -1 && f.status().generation == 5,
            "late/duplicate/future result cleared B, reserved again or launched during shutdown");
    require(std::count_if(f.changes.begin(), f.changes.end(), [](const auto& change) {
        return change.current == DeviceState::recovering;
    }) == 2, "duplicate failures created another START/notification");
}

void test_real_optional_terminal_and_partial_resource_recovery() {
    Fixture ordinary("missing", 1, 3600, "ota_service");
    ordinary.start();
    ordinary.run();
    ordinary.wait([&] { return ordinary.status().state == ServiceState::failed && ordinary.status().generation == 3; });
    ordinary.finish();
    require(ordinary.manager->queryDeviceState().current == DeviceState::warning && ordinary.status().restart_count == 1 &&
            std::none_of(ordinary.changes.begin(), ordinary.changes.end(), [](const auto& change) {
                return change.current == DeviceState::offline;
            }), "optional ordinary terminal escalated to OFFLINE");

    Fixture heartbeat("stubborn", 1, 1, "ota_service");
    heartbeat.start();
    heartbeat.run();
    heartbeat.wait([&] { return heartbeat.manager->queryDeviceState().current == DeviceState::offline; });
    heartbeat.finish();
    require(heartbeat.status().restart_count == 1 && heartbeat.status().pid == -1 &&
            heartbeat.changes.back().reason == "recovery timeout", "optional heartbeat terminal lost OFFLINE/reap");

    Fixture resource("crash_once");
    resource.manager->reportResourceUsage(0, 95);
    resource.start();
    resource.run();
    resource.wait([&] { return resource.status().restart_count == 1 && resource.status().state == ServiceState::running; });
    resource.finish();
    require(resource.manager->queryDeviceState().current == DeviceState::error &&
            resource.manager->queryDeviceState().source == "memory_monitor" &&
            std::none_of(resource.changes.begin(), resource.changes.end(), [](const auto& change) {
                return change.current == DeviceState::running;
            }), "successful service recovery cleared another source's critical resource");
}

void test_signal_and_observer_failure_during_recovery_cleanup() {
    struct sigaction before{}, after{};
    require(::sigaction(SIGTERM, nullptr, &before) == 0, "cannot inspect initial signal handler");
    {
        WriterGate gate;
        Fixture f("missing");
        GateRelease release{gate};
        f.observer = [&](const DeviceStateSnapshot& change) {
            if (change.current == DeviceState::recovering) gate.pause();
        };
        f.start(); f.run(); gate.wait();
        require(::raise(SIGTERM) == 0, "cannot inject shutdown signal");
        gate.release(); f.finish();
        require(f.status().restart_count == 1 && f.status().generation == 3 && f.status().pid == -1 &&
                f.manager->queryDeviceState().current == DeviceState::recovering,
                "accepted signal launched replacement or changed final health");
    }
    {
        Fixture f("stubborn", 20, 1);
        f.observer = [](const DeviceStateSnapshot& change) {
            if (change.current == DeviceState::recovering) throw std::runtime_error("recovery observer failure");
        };
        f.start(); f.run();
        f.wait([&] { return f.status().state == ServiceState::stopped && f.status().pid == -1; });
        bool propagated = false;
        try { f.finish(); } catch (const std::runtime_error& error) {
            propagated = std::string(error.what()) == "recovery observer failure";
        }
        require(propagated && f.status().restart_count == 1 && f.status().pid == -1,
                "observer exception swallowed or cleanup/reap failed");
    }
    require(::sigaction(SIGTERM, nullptr, &after) == 0 && before.sa_handler == after.sa_handler,
            "recovery exception/signal cleanup did not restore the handler");
}

void test_offline_cancels_other_task_and_leaves_no_recovering_service() {
    Fixture f("missing", 1);
    const auto path = f.directory / "offline-pending.json";
    {
        std::ofstream config(path);
        config << R"({"services":[{"service_name":"control_service","executable":"/missing/phase4/critical","restart_policy":"on-failure","recovery_timeout":1},{"service_name":"ota_service","executable":"/missing/phase4/optional","restart_policy":"on-failure","recovery_timeout":20}]})";
    }
    f.manager = std::make_unique<RuntimeManager>(path.string(), AggregationOptions{},
        [&](const DeviceStateSnapshot& change) { f.changes.push_back(change); });
    f.start();
    f.manager->post(Event{EventType::start, "ota_service", Clock::now()});
    f.run();
    f.wait([&] { return f.manager->queryDeviceState().current == DeviceState::offline &&
                        f.manager->query("ota_service")->state == ServiceState::stopped; });
    const auto cancelled = *f.manager->query("ota_service");
    require(cancelled.generation == 3 && cancelled.pid == -1 && cancelled.restart_count == 1,
            "OFFLINE cancelled RM task but left a permanent RECOVERING lifecycle");
    f.manager->post(Event{EventType::start, "ota_service", Clock::now()});
    f.wait([&] { return f.manager->query("ota_service")->generation == 5; });
    f.finish();
    require(f.manager->queryDeviceState().current == DeviceState::offline &&
            f.manager->query("ota_service")->restart_count == 1 &&
            std::count_if(f.changes.begin(), f.changes.end(), [](const auto& change) {
                return change.current == DeviceState::offline;
            }) == 1, "OFFLINE continued auto launch, reset budget or emitted another terminal");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--child") {
        if (std::string(argv[2]) == "stubborn") std::signal(SIGTERM, SIG_IGN);
        unsigned count = 0;
        { std::ifstream input(argv[3]); input >> count; }
        { std::ofstream output(argv[3]); output << count + 1; }
        if (std::string(argv[2]) == "crash_once" && count == 0) return 7;
        if (std::string(argv[2]) == "crash_twice" && count < 2) return 7;
        for (;;) ::pause();
    }
    try {
        test_real_crash_candidate_success_and_reap();
        test_real_retry_exhaustion_terminal_once();
        test_real_timeout_before_due_without_launch();
        test_real_heartbeat_cleanup_before_replacement();
        test_shutdown_backoff_and_manual_stop_fifo();
        test_real_manual_restart_and_stale_instance_facts();
        test_manual_launch_failure_creates_one_automatic_episode();
        test_real_dependency_stop_closure_requires_explicit_start();
        test_untrusted_recovery_ingress_cannot_clear_resource();
        test_queue_late_success_duplicate_faults_and_terminal_gate();
        test_real_optional_terminal_and_partial_resource_recovery();
        test_signal_and_observer_failure_during_recovery_cleanup();
        test_offline_cancels_other_task_and_leaves_no_recovering_service();
        std::cout << "recovery coordination tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery coordination test failed: " << error.what() << '\n';
        return 1;
    }
}
