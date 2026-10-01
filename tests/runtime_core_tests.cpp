#include "runtime/config_manager.hpp"
#include "runtime/event.hpp"
#include "runtime/logger.hpp"
#include "runtime/monitor.hpp"
#include "runtime/process_supervisor.hpp"
#include "runtime/runtime_manager.hpp"
#include "runtime/service_manager.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>

using namespace runtime;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

std::filesystem::path temp_config(const std::string& content) {
    const auto path = std::filesystem::temp_directory_path() /
        ("runtime_core_test_" + std::to_string(Clock::now().time_since_epoch().count()) + ".json");
    std::ofstream(path) << content;
    return path;
}

void test_config() {
    const auto path = temp_config(R"({"services":[{"service_name":"alpha","executable":"/bin/true","arguments":["--flag"],"autostart":true,"startup_timeout":8,"heartbeat_timeout":20,"restart_policy":"on-failure"},{"service_name":"beta","executable":"/bin/true","dependency":"alpha"}]})");
    const auto configs = ConfigManager::load_file(path.string());
    std::filesystem::remove(path);
    require(configs.size() == 2, "config service count");
    require(configs[0].arguments.size() == 1 && configs[0].autostart &&
            configs[0].startup_timeout == 8s && configs[0].heartbeat_timeout == 20s &&
            configs[0].restart_policy == RestartPolicy::on_failure, "config fields");
    require(configs[1].dependency == std::vector<std::string>{"alpha"} &&
            !configs[1].autostart && configs[1].heartbeat_timeout == 15s &&
            configs[1].restart_policy == RestartPolicy::never, "config defaults");
    const auto invalid = temp_config(R"({"service_name":"a","service_name":"b","executable":"/bin/true"})");
    rejects([&] { ConfigManager::load_file(invalid.string()); }, "duplicate JSON key accepted");
    std::filesystem::remove(invalid);
    const auto missing = temp_config(R"({"service_name":"a","executable":"/bin/true","heartbeat_timeout":0})");
    rejects([&] { ConfigManager::load_file(missing.string()); }, "invalid timeout accepted");
    std::filesystem::remove(missing);
    const auto unscheduled = temp_config(R"({"services":[{"service_name":"beta","executable":"/bin/true","dependency":"alpha"},{"service_name":"alpha","executable":"/bin/true"}]})");
    const auto loaded = ConfigManager::load_file(unscheduled.string());
    std::filesystem::remove(unscheduled);
    require(loaded[0].service_name == "beta" && loaded[1].service_name == "alpha",
            "Phase 1 must preserve configuration order without dependency scheduling");
}

void test_monitor() {
    std::vector<Event> events;
    Monitor monitor([&](Event event) { events.push_back(event); });
    const auto base = Clock::now();
    monitor.watch("alpha", base, 15s);
    monitor.check(base + 14s);
    require(events.empty(), "early monitor event");
    monitor.check(base + 15s);
    monitor.check(base + 20s);
    monitor.check(base + 25s);
    require(events.size() == 3 && events[0].missed_count == 1 &&
            events[1].missed_count == 2 && events[2].missed_count == 3,
            "monitor miss sequence");
    monitor.heartbeat("alpha", base + 26s);
    monitor.check(base + 40s);
    require(events.size() == 3, "heartbeat did not reset monitor");
    monitor.check(base + 41s);
    require(events.back().missed_count == 1, "miss counter did not reset");
    monitor.unwatch("alpha");
    monitor.check(base + 100s);
    require(events.size() == 4, "unwatched service still emits events");
}

void test_logger_flush() {
    std::ostringstream output;
    {
        Logger logger(output);
        logger.log(LogLevel::info, "runtime_manager", "started");
        logger.log(LogLevel::error, "service_manager", "failed");
    }
    require(output.str().find("[INFO] runtime_manager: started") != std::string::npos &&
            output.str().find("[ERROR] service_manager: failed") != std::string::npos,
            "logger thread did not flush queued records before destruction");
}

struct FakeProcesses final : ProcessSupervisor {
    int next_pid = 100;
    bool fail_next_start = false;
    std::vector<int> stopped;
    std::vector<int> forced;
    int start(const ServiceConfig&) override {
        if (fail_next_start) {
            fail_next_start = false;
            throw std::runtime_error("simulated execv failure");
        }
        return next_pid++;
    }
    void stop(int pid) override { stopped.push_back(pid); }
    void force_stop(int pid) override { forced.push_back(pid); }
    std::vector<ProcessExit> reap() override { return {}; }
};

void test_heartbeat_starts_after_exec() {
    FakeProcesses processes;
    std::vector<Event> events;
    Monitor monitor([&](Event event) { events.push_back(event); });
    std::ostringstream log;
    Logger logger(log);
    ServiceManager manager(processes, monitor, logger);
    ServiceConfig config;
    config.service_name = "delayed";
    config.executable = "/bin/true";
    config.heartbeat_timeout = 1s;
    manager.add(config);
    const auto requested = Clock::now() - 2s;
    manager.handle(Event{EventType::start, "delayed", requested});
    monitor.check(Clock::now());
    require(events.empty(), "heartbeat deadline began before the delayed start completed");
    monitor.check(Clock::now() + 2s);
    require(events.size() == 1 && events[0].missed_count == 1,
            "heartbeat deadline did not begin after exec completed");
}

void test_service_state() {
    FakeProcesses processes;
    std::vector<Event> health_events;
    Monitor monitor([&](Event event) { health_events.push_back(event); });
    std::ostringstream log;
    Logger logger(log);
    ServiceManager manager(processes, monitor, logger);
    ServiceConfig config;
    config.service_name = "alpha";
    config.executable = "/bin/true";
    config.restart_policy = RestartPolicy::on_failure;
    manager.add(config);
    const auto base = Clock::now();
    require(manager.query("alpha")->state == ServiceState::created, "initial state");
    manager.handle(Event{EventType::start, "alpha", base});
    require(manager.query("alpha")->state == ServiceState::running && manager.query("alpha")->pid == 100,
            "start transition");
    manager.handle(Event{EventType::heartbeat, "alpha", base + 1s});
    require(manager.query("alpha")->heartbeat_time == base + 1s, "heartbeat time");
    monitor.check(base + 16s);
    monitor.check(base + 21s);
    monitor.check(base + 26s);
    for (const auto& event : health_events) manager.handle(event);
    require(manager.query("alpha")->state == ServiceState::recovering &&
            manager.query("alpha")->restart_count == 1 && processes.stopped == std::vector<int>{100},
            "health failure and recovery scheduling");
    manager.handle(Event{EventType::process_exited, "alpha", base + 26s, 100, 9});
    manager.tick(base + 27s);
    require(manager.query("alpha")->state == ServiceState::recovering, "restart occurred too early");
    manager.tick(base + 28s);
    require(manager.query("alpha")->state == ServiceState::running && manager.query("alpha")->pid == 101,
            "restart transition");
    manager.handle(Event{EventType::health_missed, "alpha", base + 26s, -1, 0, 3});
    require(manager.query("alpha")->state == ServiceState::running,
            "stale monitor event failed a restarted service");
    manager.handle(Event{EventType::stop, "alpha", base + 29s});
    require(manager.query("alpha")->state == ServiceState::stopping, "stop transition");
    manager.handle(Event{EventType::process_exited, "alpha", base + 30s, 101, 0});
    require(manager.query("alpha")->state == ServiceState::stopped, "stopped transition");
    require(!manager.query("missing"), "unknown service query");

    ServiceConfig one_shot;
    one_shot.service_name = "one_shot";
    one_shot.executable = "/bin/true";
    manager.add(one_shot);
    manager.handle(Event{EventType::start, "one_shot", base});
    manager.handle(Event{EventType::process_exited, "one_shot", base + 1s, 102, 0});
    require(manager.query("one_shot")->state == ServiceState::stopped, "clean process exit");

    ServiceConfig dependent;
    dependent.service_name = "dependent";
    dependent.executable = "/bin/true";
    dependent.dependency = {"alpha"};
    manager.add(dependent);
    manager.handle(Event{EventType::start, "dependent", base + 31s});
    require(manager.query("dependent")->state == ServiceState::running, "dependency field must not gate Phase 1 start");
}

void test_restart_limit() {
    FakeProcesses processes;
    Monitor monitor([](Event) {});
    std::ostringstream log;
    Logger logger(log);
    ServiceManager manager(processes, monitor, logger);
    ServiceConfig config;
    config.service_name = "crasher";
    config.executable = "/bin/true";
    config.restart_policy = RestartPolicy::on_failure;
    manager.add(config);
    auto now = Clock::now();
    manager.handle(Event{EventType::start, "crasher", now});
    for (const int delay : {2, 5, 10, 30, 60}) {
        const auto pid = manager.query("crasher")->pid;
        manager.handle(Event{EventType::process_exited, "crasher", now, pid, 9});
        require(manager.query("crasher")->state == ServiceState::recovering,
                "abnormal exit did not schedule recovery");
        manager.tick(now + std::chrono::seconds(delay - 1));
        require(manager.query("crasher")->state == ServiceState::recovering,
                "recovery began before the configured delay");
        manager.tick(now + std::chrono::seconds(delay));
        require(manager.query("crasher")->state == ServiceState::running,
                "recovery did not start at the configured delay");
        now += std::chrono::seconds(delay + 1);
    }
    require(manager.query("crasher")->restart_count == 5, "restart count did not reach the cap");
    manager.handle(Event{EventType::process_exited, "crasher", now,
        manager.query("crasher")->pid, 9});
    require(manager.query("crasher")->state == ServiceState::failed &&
            manager.query("crasher")->restart_count == 5,
            "sixth failure exceeded the Phase 1 restart cap");
}

void test_stop_escalation() {
    FakeProcesses processes;
    Monitor monitor([](Event) {});
    std::ostringstream log;
    Logger logger(log);
    ServiceManager manager(processes, monitor, logger);
    ServiceConfig config;
    config.service_name = "stubborn";
    config.executable = "/bin/true";
    manager.add(config);
    const auto now = Clock::now();
    manager.handle(Event{EventType::start, "stubborn", now});
    manager.handle(Event{EventType::stop, "stubborn", now});
    manager.handle(Event{EventType::stop, "stubborn", now + 1s});
    require(manager.query("stubborn")->state == ServiceState::stopping &&
            processes.stopped == std::vector<int>{100}, "STOP did not begin once");
    manager.tick(now + 1999ms);
    require(processes.forced.empty(), "force stop happened before grace period");
    manager.tick(now + 2s);
    require(processes.forced == std::vector<int>{100}, "stubborn child was not force stopped");
    manager.tick(now + 3s);
    require(processes.forced.size() == 1, "force stop was repeated");
    manager.handle(Event{EventType::process_exited, "stubborn", now + 3s, 100, 9});
    require(manager.query("stubborn")->state == ServiceState::stopped,
            "force-stopped child did not complete STOPPING to STOPPED");
}

void test_start_failure_state() {
    FakeProcesses processes;
    processes.fail_next_start = true;
    Monitor monitor([](Event) {});
    std::ostringstream log;
    Logger logger(log);
    ServiceManager manager(processes, monitor, logger);
    ServiceConfig config;
    config.service_name = "bad_executable";
    config.executable = "/not/present";
    manager.add(config);
    manager.handle(Event{EventType::start, "bad_executable", Clock::now()});
    require(manager.query("bad_executable")->state == ServiceState::failed &&
            manager.query("bad_executable")->pid < 0,
            "exec failure did not leave service FAILED without a PID");
}

void test_process() {
    PosixProcessSupervisor processes;
    ServiceConfig config;
    config.executable = "/bin/sleep";
    config.arguments = {"30"};
    const int pid = processes.start(config);
    require(pid > 0, "process did not start");
    processes.stop(pid);
    bool exited = false;
    for (int i = 0; i < 100 && !exited; ++i) {
        for (const auto& event : processes.reap()) exited |= event.pid == pid;
        if (!exited) std::this_thread::sleep_for(10ms);
    }
    require(exited, "process was not reaped");
    config.executable = "/path/that/does/not/exist";
    rejects([&] { processes.start(config); }, "exec failure not reported");

    config.executable = "/bin/true";
    config.arguments.clear();
    const int externally_reaped = processes.start(config);
    int status = 0;
    require(::waitpid(externally_reaped, &status, 0) == externally_reaped,
            "test could not reap child");
    const auto unknown_exits = processes.reap();
    require(unknown_exits.size() == 1 && unknown_exits[0].pid == externally_reaped &&
            unknown_exits[0].status == -1, "unknown child exit was reported as success");
}

void test_runtime_loop() {
    const auto path = temp_config(R"({"service_name":"idle","executable":"/bin/true","autostart":false})");
    RuntimeManager manager(path.string());
    std::filesystem::remove(path);
    std::thread thread([&] { manager.run(); });
    manager.post(Event{EventType::shutdown, {}, Clock::now()});
    thread.join();
    require(manager.query("idle")->state == ServiceState::stopped, "runtime shutdown");
}

void test_runtime_autostart() {
    const auto path = temp_config(R"({"service_name":"sleeper","executable":"/bin/sleep","arguments":["30"],"autostart":true})");
    RuntimeManager manager(path.string());
    std::filesystem::remove(path);
    std::thread thread([&] { manager.run(); });
    bool started = false;
    for (int i = 0; i < 100 && !started; ++i) {
        started = manager.query("sleeper")->state == ServiceState::running;
        if (!started) std::this_thread::sleep_for(10ms);
    }
    if (started) manager.post(Event{EventType::stop, "sleeper", Clock::now()});
    bool stopped = false;
    for (int i = 0; i < 100 && started && !stopped; ++i) {
        stopped = manager.query("sleeper")->state == ServiceState::stopped;
        if (!stopped) std::this_thread::sleep_for(10ms);
    }
    manager.post(Event{EventType::shutdown, {}, Clock::now()});
    thread.join();
    require(started && stopped, "runtime autostart/stop integration");
}

} // namespace

int main() {
    try {
        test_config();
        test_monitor();
        test_logger_flush();
        test_heartbeat_starts_after_exec();
        test_service_state();
        test_restart_limit();
        test_stop_escalation();
        test_start_failure_state();
        test_process();
        test_runtime_loop();
        test_runtime_autostart();
        std::cout << "runtime core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime core test failed: " << error.what() << '\n';
        return 1;
    }
}
