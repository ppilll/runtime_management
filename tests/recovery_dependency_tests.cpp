#include "runtime/service_manager.hpp"
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace runtime;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct FakeProcesses final : ProcessSupervisor {
    int next_pid = 100;
    bool fail_launches = false;
    std::vector<std::string> launches;
    std::vector<int> stops;
    std::vector<int> forced;
    int start(const ServiceConfig& config) override {
        launches.push_back(config.service_name);
        if (fail_launches) throw std::runtime_error("simulated launch failure");
        return next_pid++;
    }
    void stop(int pid) override { stops.push_back(pid); }
    void force_stop(int pid) override { forced.push_back(pid); }
    std::vector<ProcessExit> reap() override { return {}; }
};

struct Fixture {
    FakeProcesses processes;
    std::vector<Event> health;
    Monitor monitor{[this](Event event) { health.push_back(event); }};
    std::ostringstream output;
    Logger logger{output};
    std::vector<ServiceStateChange> changes;
    ServiceManager services{processes, monitor, logger,
        [this](const ServiceStateChange& change) {
            require(services.query(change.service_name).has_value(), "callback registry query failed");
            changes.push_back(change);
        }};
    const Clock::time_point base = Clock::now() + 1s;

    void add(const std::string& name, RestartPolicy policy = RestartPolicy::never,
             std::vector<std::string> dependencies = {}) {
        ServiceConfig config;
        config.service_name = name;
        config.executable = "/fake/service";
        config.restart_policy = policy;
        config.dependency = std::move(dependencies);
        services.add(config);
    }
    ServiceStatus status(const std::string& name) { return *services.query(name); }
    void exit(const std::string& name, Clock::time_point at, int code = 9) {
        services.handle(Event{EventType::process_exited, name, at, status(name).pid, code});
    }
    void check(Clock::time_point at) {
        health.clear();
        monitor.check(at);
        for (const auto& event : health) services.handle(event);
    }
};

void test_crash_backoff_and_limit() {
    Fixture f;
    f.add("worker", RestartPolicy::on_failure);
    f.services.startService("worker", f.base);
    auto now = f.base;
    unsigned attempts = 0;
    for (const auto delay : {2s, 4s, 8s, 16s, 32s}) {
        now += 1s;
        const auto old_pid = f.status("worker").pid;
        f.changes.clear();
        f.exit("worker", now);
        ++attempts;
        require(f.changes.size() == 2 && f.changes[0].to == ServiceState::failed &&
                f.changes[1].to == ServiceState::recovering, "crash recovery transitions missing");
        require(f.status("worker").restart_count == attempts, "retry count incorrect");
        // Duplicate exit and late health events must not consume another retry.
        f.services.handle(Event{EventType::process_exited, "worker", now, old_pid, 9});
        f.services.handle(Event{EventType::health_missed, "worker", now, old_pid, 0, 3});
        f.services.startService("worker", now); // Must not bypass backoff.
        f.services.tick(now + delay - 1ms);
        require(f.processes.launches.size() == attempts &&
                f.status("worker").restart_count == attempts, "early or duplicate restart");
        f.services.tick(now + delay);
        require(f.status("worker").state == ServiceState::running &&
                f.processes.launches.size() == attempts + 1, "restart missed deadline");
        now += delay;
    }
    f.exit("worker", now + 1s);
    f.services.tick(now + 1h);
    require(f.status("worker").state == ServiceState::failed &&
            f.status("worker").restart_count == 5 && f.processes.launches.size() == 6,
            "restart limit exceeded");
}

void test_heartbeat_deadline_and_reaping() {
    Fixture f;
    f.add("worker", RestartPolicy::on_failure);
    f.services.startService("worker", f.base);
    f.services.handle(Event{EventType::heartbeat, "worker", f.base + 5s});
    f.services.handle(Event{EventType::heartbeat, "worker", f.base + 10s});
    f.check(f.base + 24s);
    require(f.status("worker").state == ServiceState::running, "heartbeat expired early");
    f.check(f.base + 25s);
    require(f.health.size() == 1 && f.health[0].missed_count == 1 &&
            f.status("worker").state == ServiceState::recovering &&
            f.processes.stops == std::vector<int>{100}, "first timeout did not recover");
    f.services.tick(f.base + 27s);
    require(f.processes.forced == std::vector<int>{100} &&
            f.processes.launches.size() == 1, "replacement launched before reaping");
    f.exit("worker", f.base + 28s);
    f.services.tick(f.base + 28s);
    require(f.status("worker").pid == 101 && f.status("worker").restart_count == 1,
            "reaping did not release recovery or consumed another retry");
    f.services.handle(Event{EventType::heartbeat, "worker", f.base + 10s});
    f.services.handle(Event{EventType::heartbeat, "worker", f.base + 29s, 100});
    f.services.handle(Event{EventType::health_missed, "worker", f.base + 25s, -1, 0, 3});
    f.services.handle(Event{EventType::health_missed, "worker", f.base + 50s, 100, 0, 3});
    require(!f.status("worker").heartbeat_time && f.status("worker").state == ServiceState::running,
            "stale generation events changed replacement health");

    Fixture no_heartbeat;
    no_heartbeat.add("worker", RestartPolicy::on_failure);
    no_heartbeat.services.startService("worker", no_heartbeat.base);
    no_heartbeat.check(no_heartbeat.base + 15s - 1ms);
    require(no_heartbeat.status("worker").state == ServiceState::running, "default timeout early");
    no_heartbeat.check(no_heartbeat.base + 15s);
    require(no_heartbeat.status("worker").state == ServiceState::recovering, "default 15s timeout missed");
}

void test_policy_matrix_and_launch_failure() {
    for (const auto policy : {RestartPolicy::never, RestartPolicy::on_failure, RestartPolicy::always}) {
        for (const int code : {0, 1 << 8, 9, -1}) {
            Fixture f;
            f.add("worker", policy);
            f.services.startService("worker", f.base);
            f.exit("worker", f.base + 1s, code);
            const bool retry = policy == RestartPolicy::always ||
                (policy == RestartPolicy::on_failure && code != 0);
            require(f.status("worker").state == (retry ? ServiceState::recovering :
                    (code == 0 ? ServiceState::stopped : ServiceState::failed)), "wrong exit policy");
            f.services.tick(f.base + 3s);
            require(f.processes.launches.size() == (retry ? 2u : 1u), "policy restart count incorrect");
        }
    }
    Fixture f;
    f.processes.fail_launches = true;
    f.add("worker", RestartPolicy::on_failure);
    f.services.startService("worker", f.base);
    auto now = f.base;
    for (const auto delay : {2s, 4s, 8s, 16s, 32s}) {
        now += delay;
        f.services.tick(now);
    }
    f.services.tick(now + 1h);
    require(f.status("worker").state == ServiceState::failed &&
            f.status("worker").restart_count == 5 && f.processes.launches.size() == 6,
            "failed launches did not exhaust retry budget");
}

void add_diamond(Fixture& f) {
    // Forward references, shared prerequisites and duplicate edges are supported.
    f.add("app", RestartPolicy::on_failure, {"left", "right"});
    f.add("right", RestartPolicy::on_failure, {"base", "base"});
    f.add("left", RestartPolicy::on_failure, {"base"});
    f.add("base", RestartPolicy::on_failure);
    f.add("unrelated");
}

void test_dependency_order_and_shutdown() {
    Fixture f;
    add_diamond(f);
    require(f.services.startup_order() ==
            std::vector<std::string>{"base", "left", "right", "app", "unrelated"},
            "topological order or duplicate edge handling incorrect");
    f.services.startService("app", f.base);
    f.services.startService("app", f.base);
    require(f.processes.launches == std::vector<std::string>{"base", "left", "right", "app"} &&
            f.status("unrelated").state == ServiceState::created, "start closure incorrect");
    f.services.stop_all(f.base + 1s);
    require(f.processes.stops == std::vector<int>{103, 102, 101, 100}, "shutdown order incorrect");
    f.services.tick(f.base + 3s);
    require(f.processes.forced == std::vector<int>{103, 102, 101, 100}, "force-stop order incorrect");
    for (const auto& name : {"app", "right", "left", "base"}) {
        f.exit(name, f.base + 4s, 0);
        require(f.status(name).state == ServiceState::stopped, "shutdown exit not STOPPED");
    }
    f.services.tick(f.base + 1h);
    require(f.processes.launches.size() == 4, "shutdown restarted services");
}

void test_failure_propagation_and_recovery_cancellation() {
    Fixture f;
    add_diamond(f);
    f.services.startService("app", f.base);
    f.services.startService("unrelated", f.base);
    f.exit("app", f.base + 1s); // Child already has a pending recovery.
    f.exit("base", f.base + 2s);
    require(f.status("app").state == ServiceState::stopped &&
            f.status("left").state == ServiceState::stopping &&
            f.status("right").state == ServiceState::stopping &&
            f.status("unrelated").state == ServiceState::running &&
            f.processes.stops == std::vector<int>{102, 101}, "failure propagation incorrect");
    f.exit("right", f.base + 3s, 0);
    f.exit("left", f.base + 3s, 0);
    f.services.startService("app", f.base + 3s); // Base is still in backoff.
    require(f.processes.launches.size() == 5 && f.status("app").state == ServiceState::stopped,
            "dependent start bypassed prerequisite backoff");
    f.services.tick(f.base + 4s);
    require(f.status("base").state == ServiceState::running &&
            f.status("app").state == ServiceState::stopped && f.processes.launches.size() == 6,
            "cancelled dependent recovery restarted automatically");
    f.services.startService("app", f.base + 5s);
    require(f.processes.launches.size() == 9 && f.status("app").state == ServiceState::running,
            "explicit dependent restart did not restore closure");

    // Clean exit also makes a prerequisite unavailable.
    Fixture clean;
    clean.add("leaf", RestartPolicy::always, {"base"});
    clean.add("base");
    clean.services.startService("leaf", clean.base);
    clean.exit("base", clean.base + 1s, 0);
    clean.exit("leaf", clean.base + 2s, 0);
    clean.services.tick(clean.base + 1h);
    require(clean.status("leaf").state == ServiceState::stopped && clean.processes.launches.size() == 2,
            "clean prerequisite exit restarted a dependent");
}

void test_explicit_stop_and_blocked_launch() {
    Fixture f;
    add_diamond(f);
    f.services.startService("app", f.base);
    f.services.stopService("base", f.base + 1s);
    require(f.processes.stops == std::vector<int>{103, 102, 101, 100}, "explicit stop order incorrect");
    f.services.stopService("base", f.base + 2s);
    require(f.processes.stops.size() == 4, "duplicate stop signalled again");

    Fixture blocked;
    blocked.add("leaf", RestartPolicy::on_failure, {"base"});
    blocked.add("base", RestartPolicy::on_failure);
    blocked.processes.fail_launches = true;
    blocked.services.startService("leaf", blocked.base);
    require(blocked.processes.launches == std::vector<std::string>{"base"} &&
            blocked.status("leaf").state == ServiceState::stopped, "launch ignored failed prerequisite");
    blocked.services.stopService("base", blocked.base + 1s);
    blocked.services.tick(blocked.base + 1h);
    require(blocked.processes.launches.size() == 1 && blocked.status("base").state == ServiceState::stopped,
            "explicit stop did not cancel backoff");
}

void test_invalid_graphs_have_no_side_effects() {
    for (const int kind : {0, 1, 2}) {
        Fixture f;
        f.add("a", RestartPolicy::never, {kind == 0 ? "missing" : (kind == 1 ? "a" : "b")});
        if (kind == 2) f.add("b", RestartPolicy::never, {"a"});
        bool rejected = false;
        try { f.services.startService("a", f.base); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && f.processes.launches.empty() && f.changes.empty(),
                "invalid graph accepted or changed lifecycle state");
    }
}
} // namespace

int main() {
    try {
        test_crash_backoff_and_limit();
        test_heartbeat_deadline_and_reaping();
        test_policy_matrix_and_launch_failure();
        test_dependency_order_and_shutdown();
        test_failure_propagation_and_recovery_cancellation();
        test_explicit_stop_and_blocked_launch();
        test_invalid_graphs_have_no_side_effects();
        std::cout << "recovery/dependency tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery/dependency test failed: " << error.what() << '\n';
        return 1;
    }
}
