#include "runtime/recovery_manager.hpp"
#include <memory>
#include "runtime/runtime_manager.hpp"
#include "runtime/service_manager.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace runtime;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

volatile std::sig_atomic_t fixture_running = 1;
void fixture_stop(int) { fixture_running = 0; }

// Re-exec this binary: no shell, external service, pidfile or production hook.
int child_fixture(int argc, char** argv) {
    if (argc != 4) return 90;
    const std::string mode(argv[2]);
    if (mode != "wait" && mode != "ignore") return 91;
    struct sigaction action{};
    action.sa_handler = mode == "ignore" ? SIG_IGN : fixture_stop;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGTERM, &action, nullptr) != 0) return 92;
    const int fd = std::stoi(argv[3]);
    const char ready = 'R';
    ssize_t count;
    do { count = ::write(fd, &ready, 1); } while (count < 0 && errno == EINTR);
    ::close(fd);
    if (count != 1) return 93;
    while (fixture_running) std::this_thread::sleep_for(5ms);
    return 0;
}

struct ReadyPipe {
    int fds[2];
    ReadyPipe() { require(::pipe(fds) == 0, "readiness pipe failed"); }
    ~ReadyPipe() { ::close(fds[0]); ::close(fds[1]); }
    ReadyPipe(const ReadyPipe&) = delete;
    ReadyPipe& operator=(const ReadyPipe&) = delete;
    void wait() {
        const auto deadline = Clock::now() + 3s;
        while (Clock::now() < deadline) {
            pollfd ready{fds[0], POLLIN, 0};
            const int result = ::poll(&ready, 1, 20);
            if (result < 0 && errno == EINTR) continue;
            require(result >= 0, "readiness poll failed");
            if (result == 0) continue;
            char value = 0;
            require((ready.revents & POLLIN) && ::read(fds[0], &value, 1) == 1 && value == 'R',
                    "invalid readiness acknowledgement");
            return;
        }
        throw std::runtime_error("child readiness timeout");
    }
};

ServiceConfig definition(const std::string& name, const ReadyPipe& ready,
                         const std::string& mode = "wait") {
    ServiceConfig config;
    config.service_name = name;
    config.executable = "/proc/self/exe";
    config.arguments = {"--fixture", mode, std::to_string(ready.fds[1])};
    config.startup_timeout = 3s;
    config.shutdown_timeout = 1s;
    config.heartbeat_timeout = 3s;
    config.restart_policy = RestartPolicy::on_failure;
    return config;
}

// Record calls while delegating every process operation to the real backend.
struct RecordedProcesses final : ProcessSupervisor {
    PosixProcessSupervisor real;
    std::map<int, std::string> names;
    std::vector<std::string> launches;
    std::vector<std::string> stops;
    std::vector<std::string> forced;
    int start(const ServiceConfig& config) override {
        const int pid = real.start(config);
        names[pid] = config.service_name;
        launches.push_back(config.service_name);
        return pid;
    }
    void stop(int pid) override {
        stops.push_back(names.at(pid));
        real.stop(pid);
    }
    void force_stop(int pid) override {
        forced.push_back(names.at(pid));
        real.force_stop(pid);
    }
    std::vector<ProcessExit> reap() override { return real.reap(); }
};

// Test-only writer adapter. Lifecycle callbacks append work; draining is explicit.
struct RecoveryHarness {
    ServiceManager& services;
    std::vector<ServiceStateChange>& changes;
    std::unique_ptr<RecoveryManager> recovery;
    std::size_t cursor = 0;
    Clock::time_point checkpoint{};
    RecoveryHarness(ServiceManager& sm, std::vector<ServiceStateChange>& work) : services(sm), changes(work) {}
    void connect() {
        if (recovery) return;
        std::vector<ServiceConfig> definitions;
        for (const auto& name : services.startup_order()) definitions.push_back(*services.queryServiceDefinition(name));
        recovery = std::make_unique<RecoveryManager>(std::move(definitions), services, [this] { return checkpoint; });
    }
    void drain(Clock::time_point now) {
        connect();
        checkpoint = now;
        while (cursor < changes.size()) {
            const auto change = changes[cursor++]; // prepare/finalize can append more work.
            if (change.cause == ServiceChangeCause::recovery_finalization) continue;
            if (change.to == ServiceState::stopping || change.to == ServiceState::stopped) {
                if (change.cause != ServiceChangeCause::recovery_preparation)
                    recovery->cancel(change.service_name, change.cause == ServiceChangeCause::dependency_stop ?
                        RecoveryTerminalReason::dependency_stop : RecoveryTerminalReason::explicit_stop, now);
            } else if (change.to == ServiceState::failed) {
                if (change.operation) {
                    recovery->observeFailure(change.service_name, change.operation->context, change.generation,
                        change.failure_type.value_or(FailureType::startup_failure), "captured attempt failure", now);
                } else {
                    RecoveryRequest request;
                    request.service_name = change.service_name;
                    request.service_generation = change.generation;
                    request.failure_type = change.failure_type.value_or(FailureType::startup_failure);
                    request.reason = "captured failure";
                    recovery->submit(std::move(request), now);
                }
            }
        }
    }
    void handle(const Event& event) {
        connect();
        checkpoint = event.at;
        if (event.type == EventType::process_exited && recovery->query(event.service_name))
            services.handleProcessExit(event, recovery->classifyExit(event.service_name, event.exit_status));
        else services.handle(event);
        drain(event.at);
    }
    void tick(Clock::time_point now) {
        services.tick(now);
        drain(now);
        recovery->tick(now);
        drain(now);
    }
};

struct Fixture {
    RecordedProcesses processes;
    std::vector<Event> health;
    Monitor monitor{[this](Event event) { health.push_back(std::move(event)); }};
    std::ostringstream output;
    Logger logger{output};
    std::vector<ServiceStateChange> changes;
    ServiceManager services{processes, monitor, logger,
        [this](const ServiceStateChange& change) { changes.push_back(change); }};
    RecoveryHarness coordinator{services, changes};
    // Logical deadlines avoid sleeping through the 2/4/8/16/32 second backoff.
    // Process launch, signal delivery, readiness and waitpid remain real.
    const Clock::time_point base = Clock::now() + 10s;
    ServiceStatus status(const std::string& name) {
        const auto snapshot = services.queryServiceStatus(name);
        require(snapshot.has_value(), "service snapshot missing");
        return *snapshot;
    }
    void check(Clock::time_point at) {
        health.clear();
        monitor.check(at);
        for (const auto& event : health) coordinator.handle(event);
    }
    ProcessExit reap_one(const std::string& name, Clock::time_point at) {
        const int pid = status(name).pid;
        const auto deadline = Clock::now() + 3s;
        while (Clock::now() < deadline) {
            const auto exits = processes.reap();
            if (!exits.empty()) {
                require(exits.size() == 1 && exits[0].pid == pid, "unexpected child exit");
                coordinator.handle(Event{EventType::process_exited, name, at, pid, exits[0].status});
                return exits[0];
            }
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("child reap timeout");
    }
};

void require_reaped(int pid) {
    int status = 0;
    errno = 0;
    require(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD,
            "child remains waitable after reap");
}

void test_start_and_graceful_shutdown() {
    ReadyPipe ready;
    Fixture f;
    auto config = definition("worker", ready);
    config.restart_policy = RestartPolicy::always;
    f.services.registerService(config);
    f.services.startService("worker", f.base);
    ready.wait();
    const int pid = f.status("worker").pid;
    require(pid > 0 && f.processes.real.getPid("worker") == pid &&
            f.processes.real.checkProcessAlive(pid) && f.status("worker").state == ServiceState::running,
            "startup did not synchronize live process and RUNNING state");
    require(f.changes.size() == 2 && f.changes[0].from == ServiceState::created &&
            f.changes[0].to == ServiceState::starting && f.changes[1].from == ServiceState::starting &&
            f.changes[1].to == ServiceState::running, "startup lifecycle edges missing");
    f.services.startService("worker", f.base);
    f.services.stopService("worker", f.base + 1s);
    f.services.stopService("worker", f.base + 1s);
    require(f.status("worker").state == ServiceState::stopping &&
            f.processes.stops == std::vector<std::string>{"worker"}, "duplicate stop or missing STOPPING");
    const auto exit = f.reap_one("worker", f.base + 1s);
    require(WIFEXITED(exit.status) && WEXITSTATUS(exit.status) == 0,
            "SIGTERM did not produce a graceful exit");
    f.check(f.base + 1h);
    f.coordinator.tick(f.base + 1h);
    require(f.status("worker").state == ServiceState::stopped && f.status("worker").pid == -1 &&
            !f.status("worker").start_time && f.processes.launches.size() == 1 &&
            f.processes.forced.empty() && f.health.empty(), "explicit stop restarted or retained service");
    require_reaped(pid);
}

void test_real_crash_backoff_and_restart_limit() {
    ReadyPipe ready;
    Fixture f;
    f.services.registerService(definition("crasher", ready));
    f.services.startService("crasher", f.base);
    ready.wait();
    auto now = f.base;
    unsigned attempt = 0;
    for (const auto delay : {2s, 4s, 8s, 16s, 32s}) {
        const int old_pid = f.status("crasher").pid;
        require(::kill(old_pid, SIGKILL) == 0, "crash injection failed");
        const auto exit = f.reap_one("crasher", now);
        require(WIFSIGNALED(exit.status) && WTERMSIG(exit.status) == SIGKILL,
                "crash wait status was lost");
        require_reaped(old_pid);
        ++attempt;
        require(f.status("crasher").state == ServiceState::recovering &&
                f.status("crasher").pid == -1 && f.status("crasher").restart_count == attempt,
                "real crash did not schedule one retry");
        f.services.handle(Event{EventType::process_exited, "crasher", now, old_pid, exit.status});
        f.services.startService("crasher", now);
        f.coordinator.tick(now + delay - 1ms);
        require(f.processes.launches.size() == attempt && f.status("crasher").restart_count == attempt,
                "duplicate exit/start bypassed backoff");
        f.coordinator.tick(now + delay);
        ready.wait();
        const int replacement = f.status("crasher").pid;
        require(replacement > 0 && f.processes.real.checkProcessAlive(replacement) &&
                f.status("crasher").state == ServiceState::running &&
                f.processes.launches.size() == attempt + 1, "replacement did not launch at deadline");
        // The old PID may be reused by Linux; waitpid proved its generation reaped.
        now += delay + 1s;
    }
    const int last_pid = f.status("crasher").pid;
    require(::kill(last_pid, SIGKILL) == 0, "final crash injection failed");
    f.reap_one("crasher", now);
    f.coordinator.tick(now + 1h);
    require(f.status("crasher").state == ServiceState::failed && f.status("crasher").pid == -1 &&
            f.status("crasher").restart_count == 5 && f.processes.launches.size() == 6,
            "sixth crash did not exhaust five-retry budget");
    require_reaped(last_pid);
}

void test_heartbeat_timeout_escalation_and_recovery() {
    ReadyPipe ready;
    Fixture f;
    f.services.registerService(definition("silent", ready, "ignore"));
    f.services.startService("silent", f.base);
    ready.wait();
    const int old_pid = f.status("silent").pid;
    // Feed one accepted heartbeat through the public API, then stop delivering.
    // IPC transport is covered separately by ipc_integration_tests.cpp.
    f.services.handle(Event{EventType::heartbeat, "silent", f.base + 1s, old_pid});
    f.check(f.base + 4s - 1ms);
    require(f.health.empty() && f.status("silent").state == ServiceState::running,
            "heartbeat expired before the configured deadline");
    f.check(f.base + 4s);
    require(f.health.size() == 1 && f.health[0].missed_count == 1 &&
            f.status("silent").state == ServiceState::recovering &&
            f.processes.stops == std::vector<std::string>{"silent"}, "timeout did not trigger recovery");
    f.coordinator.tick(f.base + 5s - 1ms);
    require(f.processes.real.checkProcessAlive(old_pid) && f.processes.forced.empty(),
            "SIGKILL preceded the shutdown grace deadline");
    f.coordinator.tick(f.base + 5s);
    f.coordinator.tick(f.base + 6s);
    require(f.processes.forced == std::vector<std::string>{"silent"} &&
            f.processes.launches.size() == 1 && f.status("silent").pid == old_pid,
            "recovery launched before old child reaping");
    const auto exit = f.reap_one("silent", f.base + 6s);
    require(WIFSIGNALED(exit.status) && WTERMSIG(exit.status) == SIGKILL,
            "unresponsive child was not force stopped");
    f.coordinator.tick(f.base + 6s);
    ready.wait();
    require(f.status("silent").state == ServiceState::running && f.status("silent").restart_count == 1 &&
            f.processes.real.checkProcessAlive(f.status("silent").pid) &&
            f.processes.launches.size() == 2, "heartbeat recovery did not create a live replacement");
}

void test_real_dependency_start_stop_order() {
    ReadyPipe app, left, right, base;
    Fixture f;
    auto add = [&](const std::string& name, const ReadyPipe& ready, std::vector<std::string> dependencies) {
        auto config = definition(name, ready);
        config.dependency = std::move(dependencies);
        f.services.registerService(std::move(config));
    };
    add("app", app, {"left", "right"});
    add("right", right, {"base", "base"});
    add("left", left, {"base"});
    add("base", base, {});
    f.services.startService("app", f.base);
    base.wait(); left.wait(); right.wait(); app.wait();
    require(f.processes.launches == std::vector<std::string>{"base", "left", "right", "app"},
            "real launches did not follow dependency order");
    for (const auto& name : {"base", "left", "right", "app"})
        require(f.status(name).state == ServiceState::running &&
                f.processes.real.checkProcessAlive(f.status(name).pid), "dependency child is not RUNNING/alive");
    f.services.stop_all(f.base + 1s);
    require(f.processes.stops == std::vector<std::string>{"app", "right", "left", "base"},
            "shutdown signals did not follow reverse dependency order");
    const auto deadline = Clock::now() + 3s;
    unsigned reaped = 0;
    while (reaped < 4 && Clock::now() < deadline) {
        for (const auto& exit : f.processes.reap()) {
            require(WIFEXITED(exit.status) && WEXITSTATUS(exit.status) == 0, "dependency shutdown was not graceful");
            f.services.handle(Event{EventType::process_exited, f.processes.names.at(exit.pid),
                                   f.base + 1s, exit.pid, exit.status});
            require_reaped(exit.pid);
            ++reaped;
        }
        if (reaped < 4) std::this_thread::sleep_for(5ms);
    }
    require(reaped == 4, "dependency children were not all reaped");
    f.coordinator.tick(f.base + 1h);
    for (const auto& name : {"base", "left", "right", "app"})
        require(f.status(name).state == ServiceState::stopped && f.status(name).pid == -1,
                "dependency shutdown did not clear registry");
    require(f.processes.launches.size() == 4, "dependency shutdown scheduled recovery");
}

struct TemporaryConfig {
    std::string path;
    explicit TemporaryConfig(const ReadyPipe& ready) {
        char pattern[] = "/tmp/runtime_phase2_shutdown_XXXXXX";
        const int fd = ::mkstemp(pattern);
        require(fd >= 0, "temporary configuration failed");
        path = pattern;
        ::close(fd);
        std::ofstream file(path);
        file << "{\"service_name\":\"stubborn\",\"executable\":\"/proc/self/exe\","
                "\"arguments\":[\"--fixture\",\"ignore\",\"" << ready.fds[1] << "\"],"
                "\"autostart\":true,\"shutdown_timeout\":6,\"heartbeat_timeout\":60}";
        file.close();
        if (!file) { std::remove(path.c_str()); throw std::runtime_error("configuration write failed"); }
    }
    ~TemporaryConfig() { std::remove(path.c_str()); }
};

struct RuntimeThread {
    RuntimeManager& manager;
    std::exception_ptr error;
    std::thread worker;
    explicit RuntimeThread(RuntimeManager& runtime) : manager(runtime), worker([this] {
        try { manager.run(); } catch (...) { error = std::current_exception(); }
    }) {}
    ~RuntimeThread() {
        if (worker.joinable()) {
            manager.post(Event{EventType::shutdown, {}, Clock::now()});
            worker.join();
        }
    }
    void finish() {
        manager.post(Event{EventType::shutdown, {}, Clock::now()});
        worker.join();
        if (error) std::rethrow_exception(error);
    }
};

void test_p5_native_default_reader_system_process_and_shutdown() {
    ReadyPipe ready;
    TemporaryConfig file(ready);
    auto config = ConfigManager::load_runtime_file(file.path);
    config.monitoring.sample_interval_seconds = 1s;
    config.services.front().shutdown_timeout = 1s;
    RuntimeManager manager(config); // Default native mode and real bounded proc reader.
    RuntimeThread thread(manager);
    ready.wait();
    const auto initial = manager.query("stubborn").value();
    std::optional<ResourceSnapshotView> view;
    const auto deadline = Clock::now() + 8s;
    do {
        view = manager.queryResourceSnapshot();
        if (view && view->snapshot.system.cpu.quality == MetricQuality::valid &&
            view->snapshot.system.memory.quality == MetricQuality::valid &&
            view->snapshot.process_scan_quality == MetricQuality::valid &&
            view->snapshot.processes.size() == 1 && view->snapshot.processes.front().cpu_quality == MetricQuality::valid)
            break;
        require(Clock::now() < deadline, "native real procfs did not produce system/process values");
        std::this_thread::sleep_for(10ms);
    } while (true);
    const auto& system = view->snapshot.system;
    const auto& memory = system.memory.value.value();
    const auto& row = view->snapshot.processes.front();
    require(system.cpu.value && *system.cpu.value >= 0 && *system.cpu.value <= 100 &&
            memory.total_bytes > 0 && memory.available_bytes <= memory.total_bytes &&
            memory.used_percent >= 0 && memory.used_percent <= 100,
            "real native system values outside frozen units/bounds");
    require(row.service_name == "stubborn" && row.pid == initial.pid &&
            row.instance_generation == initial.launched_generation && row.proc_start_time_ticks &&
            row.observation_status == ProcessObservationStatus::observed && row.rss_bytes &&
            row.cpu_percent && *row.cpu_percent >= 0 && row.sampled_at <= view->snapshot.collected_at,
            "real process measurement lost launch identity/units/completion time");
    const auto status = manager.query("stubborn").value();
    require(status.pid == initial.pid && status.restart_count == 0 && status.generation == initial.generation,
            "real resource sampling changed service lifecycle");
    thread.finish();
    const auto final = manager.queryResourceSnapshot().value();
    const auto state = manager.queryDeviceState();
    for (unsigned query = 0; query < 20; ++query)
        require(manager.queryResourceSnapshot()->snapshot.collected_at == final.snapshot.collected_at &&
                manager.queryDeviceState().timestamp == state.timestamp, "post-join query advanced sampling/health");
    require(manager.query("stubborn")->state == ServiceState::stopped && manager.query("stubborn")->pid == -1,
            "native shutdown did not settle/reap the managed child");
    require_reaped(initial.pid);
}

void test_runtime_shutdown_respects_long_grace_period() {
    ReadyPipe ready;
    TemporaryConfig config(ready);
    RuntimeManager manager(config.path);
    RuntimeThread thread(manager);
    ready.wait(); // SIGTERM is ignored before requesting shutdown.
    const auto startup_deadline = Clock::now() + 3s;
    auto initial = manager.query("stubborn");
    while (initial && initial->state != ServiceState::running && Clock::now() < startup_deadline) {
        std::this_thread::sleep_for(5ms);
        initial = manager.query("stubborn");
    }
    require(initial && initial->state == ServiceState::running && initial->pid > 0,
            "shutdown regression child did not reach RUNNING");
    const auto requested = Clock::now();
    thread.finish();
    const auto status = manager.query("stubborn");
    // Regression for the former fixed 4s budget with a 6s grace period.
    // Keep the intended assertion; do not mark WILL_FAIL or relax the deadline.
    require(status && status->state == ServiceState::stopped && status->pid == -1,
            "RuntimeManager returned before shutdown_timeout escalation/reaping completed");
    require(Clock::now() - requested >= 6s, "runtime shortened the configured shutdown grace period");
    require_reaped(initial->pid);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--fixture") return child_fixture(argc, argv);
        if (argc == 2 && std::string(argv[1]) == "--shutdown-regression") {
            test_runtime_shutdown_respects_long_grace_period();
        } else {
            require(argc == 1, "unexpected test arguments");
            test_start_and_graceful_shutdown();
            test_real_crash_backoff_and_restart_limit();
            test_heartbeat_timeout_escalation_and_recovery();
            test_real_dependency_start_stop_order();
            test_p5_native_default_reader_system_process_and_shutdown();
        }
        std::cout << "Phase 2 integration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Phase 2 integration test failed: " << error.what() << '\n';
        return 1;
    }
}
