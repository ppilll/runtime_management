#include "runtime/recovery_manager.hpp"
#include <memory>
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
    std::vector<std::chrono::seconds> startup_caps;
    int start(const ServiceConfig& config) override {
        launches.push_back(config.service_name);
        startup_caps.push_back(config.startup_timeout);
        if (fail_launches) throw std::runtime_error("simulated launch failure");
        return next_pid++;
    }
    void stop(int pid) override { stops.push_back(pid); }
    void force_stop(int pid) override { forced.push_back(pid); }
    std::vector<ProcessExit> reap() override { return {}; }
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
    RecoveryHarness coordinator{services, changes};
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
        coordinator.handle(Event{EventType::process_exited, name, at, status(name).pid, code});
    }
    void check(Clock::time_point at) {
        health.clear();
        monitor.check(at);
        for (const auto& event : health) coordinator.handle(event);
    }
};

void test_crash_backoff_and_limit() {
    Fixture f;
    f.add("worker", RestartPolicy::on_failure);
    f.services.startService("worker", f.base);
    f.coordinator.drain(f.base);
    auto now = f.base;
    unsigned attempts = 0;
    for (const auto delay : {2s, 4s, 8s, 16s, 32s}) {
        now += 1s;
        const auto old_pid = f.status("worker").pid;
        const auto offset = f.changes.size();
        f.exit("worker", now);
        ++attempts;
        require(f.changes.size() == offset + 2 && f.changes[offset].to == ServiceState::failed &&
                f.changes[offset + 1].to == ServiceState::recovering, "crash recovery transitions missing");
        require(f.status("worker").restart_count == attempts, "retry count incorrect");
        // Duplicate exit and late health events must not consume another retry.
        f.services.handle(Event{EventType::process_exited, "worker", now, old_pid, 9});
        f.services.handle(Event{EventType::health_missed, "worker", now, old_pid, 0, 3});
        f.services.startService("worker", now); // Must not bypass backoff.
        f.coordinator.tick(now + delay - 1ms);
        require(f.processes.launches.size() == attempts &&
                f.status("worker").restart_count == attempts, "early or duplicate restart");
        f.coordinator.tick(now + delay);
        require(f.status("worker").state == ServiceState::running &&
                f.processes.launches.size() == attempts + 1, "restart missed deadline");
        now += delay;
    }
    f.exit("worker", now + 1s);
    f.coordinator.tick(now + 1h);
    require(f.status("worker").state == ServiceState::failed &&
            f.status("worker").restart_count == 5 && f.processes.launches.size() == 6,
            "restart limit exceeded");
}

void test_heartbeat_deadline_and_reaping() {
    Fixture f;
    f.add("worker", RestartPolicy::on_failure);
    f.services.startService("worker", f.base);
    f.coordinator.drain(f.base);
    f.services.handle(Event{EventType::heartbeat, "worker", f.base + 5s});
    f.services.handle(Event{EventType::heartbeat, "worker", f.base + 10s});
    f.check(f.base + 24s);
    require(f.status("worker").state == ServiceState::running, "heartbeat expired early");
    f.check(f.base + 25s);
    require(f.health.size() == 1 && f.health[0].missed_count == 1 &&
            f.status("worker").state == ServiceState::recovering &&
            f.processes.stops == std::vector<int>{100}, "first timeout did not recover");
    f.coordinator.tick(f.base + 27s);
    require(f.processes.forced == std::vector<int>{100} &&
            f.processes.launches.size() == 1, "replacement launched before reaping");
    f.exit("worker", f.base + 28s);
    f.coordinator.tick(f.base + 28s);
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
            f.coordinator.drain(f.base);
            f.exit("worker", f.base + 1s, code);
            const bool retry = policy == RestartPolicy::always ||
                (policy == RestartPolicy::on_failure && code != 0);
            require(f.status("worker").state == (retry ? ServiceState::recovering :
                    (code == 0 ? ServiceState::stopped : ServiceState::failed)), "wrong exit policy");
            f.coordinator.tick(f.base + 3s);
            require(f.processes.launches.size() == (retry ? 2u : 1u), "policy restart count incorrect");
        }
    }
    Fixture f;
    f.processes.fail_launches = true;
    f.add("worker", RestartPolicy::on_failure);
    f.services.startService("worker", f.base);
    f.coordinator.drain(f.base);
    auto now = f.base;
    for (const auto delay : {2s, 4s, 8s, 16s, 32s}) {
        now += delay;
        f.coordinator.tick(now);
    }
    f.coordinator.tick(now + 1h);
    require(f.status("worker").state == ServiceState::failed &&
            f.status("worker").restart_count == 5 && f.processes.launches.size() == 6,
            "failed launches did not exhaust retry budget");
}

void test_terminal_failure_metadata_and_generations() {
    Fixture f;
    f.processes.fail_launches = true;
    f.add("worker", RestartPolicy::on_failure);
    f.services.startService("worker", f.base);
    f.coordinator.drain(f.base);
    auto at = f.base;
    for (const auto delay : {2s, 4s, 8s, 16s, 32s}) {
        at += delay;
        f.coordinator.tick(at);
    }
    std::uint64_t previous = 0;
    for (const auto& change : f.changes) {
        require(change.generation > 0 && change.generation >= previous, "lifecycle token decreased");
        previous = change.generation;
        require(!change.recovery_exhausted, "SM produced a legacy terminal");
    }
    const auto results = f.coordinator.recovery->takeResults();
    require(results.size() == 1 && results[0].outcome == RecoveryOutcome::failed &&
            results[0].terminal_reason == RecoveryTerminalReason::retry_exhausted &&
            results[0].latest_fault_generation == f.status("worker").generation &&
            f.status("worker").state == ServiceState::failed,
            "restart exhaustion must produce exactly one terminal result");
    const auto count = f.changes.size();
    f.coordinator.tick(at + 1h);
    require(f.changes.size() == count, "terminal result repeated on tick");
    const auto exhausted_generation = f.status("worker").generation;
    f.services.stopService("worker", at + 1h);
    require(f.status("worker").generation > exhausted_generation, "stop did not cancel old recovery token");

    Fixture never;
    never.processes.fail_launches = true;
    never.add("worker", RestartPolicy::never);
    never.services.startService("worker", never.base);
    for (const auto& change : never.changes)
        require(!change.recovery_exhausted, "never policy incorrectly reported exhausted recovery");
}

void test_shutdown_deadline_uses_all_service_grace_periods() {
    Fixture f;
    for (const auto& entry : std::vector<std::pair<std::string, std::chrono::seconds>>{{"short", 2s}, {"long", 6s}}) {
        ServiceConfig config;
        config.service_name = entry.first;
        config.executable = "/fake/service";
        config.shutdown_timeout = entry.second;
        f.services.add(config);
        f.services.startService(entry.first, f.base);
    }
    const auto at = f.base + 1s;
    f.services.stop_all(at);
    require(f.services.shutdown_deadline() == at + 6s, "shutdown budget ignored longest active service grace");
    f.coordinator.tick(at + 2s);
    require(f.processes.forced == std::vector<int>{100}, "short service escalation deadline changed");
    f.exit("short", at + 2s);
    require(f.services.shutdown_deadline() == at + 6s, "remaining long service deadline lost");
    f.coordinator.tick(at + 6s);
    require(f.processes.forced == std::vector<int>{100, 101}, "long service forced before its deadline");
    f.exit("long", at + 6s);
    require(!f.services.shutdown_deadline(), "reaped services retained shutdown deadlines");
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
    f.coordinator.tick(f.base + 3s);
    require(f.processes.forced == std::vector<int>{103, 102, 101, 100}, "force-stop order incorrect");
    for (const auto& name : {"app", "right", "left", "base"}) {
        f.exit(name, f.base + 4s, 0);
        require(f.status(name).state == ServiceState::stopped, "shutdown exit not STOPPED");
    }
    f.coordinator.tick(f.base + 1h);
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
    f.coordinator.tick(f.base + 4s);
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
    clean.coordinator.tick(clean.base + 1h);
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
    blocked.coordinator.drain(blocked.base);
    blocked.services.stopService("base", blocked.base + 1s);
    blocked.coordinator.tick(blocked.base + 1h);
    require(blocked.processes.launches.size() == 1 && blocked.status("base").state == ServiceState::stopped,
            "explicit stop did not cancel backoff");
}

void test_timeout_finalization_preserves_grace_pid_and_instance() {
    Fixture f;
    ServiceConfig config;
    config.service_name = "worker";
    config.executable = "/fake/service";
    config.restart_policy = RestartPolicy::on_failure;
    config.shutdown_timeout = 6s;
    config.recovery_timeout = 1s;
    f.services.add(config);
    f.services.startService("worker", f.base);
    const auto launched = f.status("worker").launched_generation;
    f.check(f.base + 15s);
    const auto active = f.coordinator.recovery->query("worker")->active;
    require(active && f.status("worker").pid == 100 && f.status("worker").launched_generation == launched,
            "failure replaced unreaped identity");
    f.coordinator.tick(f.base + 16s);
    const auto result = f.coordinator.recovery->query("worker")->last_result;
    require(result && result->outcome == RecoveryOutcome::timeout &&
            result->latest_fault_generation == f.status("worker").generation &&
            f.status("worker").state == ServiceState::failed && f.status("worker").pid == 100 &&
            f.status("worker").launched_generation == launched &&
            f.services.shutdown_deadline() == f.base + 21s && f.processes.stops == std::vector<int>{100},
            "timeout lost captured fault/PID or shortened/restarted grace");
    f.coordinator.tick(f.base + 21s - 1ms);
    require(f.processes.forced.empty(), "timeout shortened cleanup grace");
    f.coordinator.tick(f.base + 21s);
    require(f.processes.forced == std::vector<int>{100}, "original grace deadline lost");
    Event exit{EventType::process_exited, "worker", f.base + 22s, 100, 9};
    exit.instance_generation = launched;
    f.coordinator.handle(exit);
    f.coordinator.tick(f.base + 1h);
    require(f.status("worker").pid == -1 && f.status("worker").launched_generation == 0 &&
            f.status("worker").state == ServiceState::failed && f.processes.launches.size() == 1 &&
            f.coordinator.recovery->takeResults().size() == 1,
            "finalization cleanup created another recovery or cleared terminal fault");
    bool tagged = false;
    for (const auto& change : f.changes)
        if (change.cause == ServiceChangeCause::recovery_finalization)
            tagged = change.operation.has_value() && change.generation == result->latest_fault_generation;
    require(tagged, "finalization callback missing captured operation");
}

void test_timeout_after_force_does_not_restart_cleanup_grace() {
    Fixture f;
    ServiceConfig config;
    config.service_name = "worker";
    config.executable = "/fake/service";
    config.restart_policy = RestartPolicy::on_failure;
    config.recovery_timeout = 10s;
    f.services.add(config);
    f.services.startService("worker", f.base);
    f.check(f.base + 15s);
    f.coordinator.tick(f.base + 17s);
    require(f.processes.forced == std::vector<int>{100} && !f.services.shutdown_deadline(), "force cleanup missing");
    f.coordinator.tick(f.base + 25s);
    require(f.status("worker").state == ServiceState::failed && f.status("worker").pid == 100 &&
            !f.services.shutdown_deadline() && f.processes.stops == std::vector<int>{100} &&
            f.processes.forced == std::vector<int>{100} && f.processes.launches.size() == 1,
            "terminal reset grace or launched before delayed reap");
}

void test_manual_restart_reap_and_remaining_startup_cap() {
    Fixture f;
    ServiceConfig config;
    config.service_name = "worker";
    config.executable = "/fake/service";
    config.restart_policy = RestartPolicy::on_failure;
    config.recovery_timeout = 10s;
    f.services.add(config);
    f.services.startService("worker", f.base);
    f.coordinator.drain(f.base);
    RecoveryRequest request;
    request.service_name = "worker";
    request.service_generation = f.status("worker").generation;
    request.origin = RecoveryOrigin::manual_restart;
    request.failure_type = FailureType::manual_request;
    request.reason = "manual restart";
    f.coordinator.checkpoint = f.base;
    require(f.coordinator.recovery->submit(request, f.base) == RecoveryAdmission::admitted, "manual not routed");
    f.coordinator.tick(f.base + 1s);
    require(f.processes.launches.size() == 1 && f.status("worker").pid == 100, "manual bypassed reap");
    f.exit("worker", f.base + 3s, 0);
    f.coordinator.tick(f.base + 3s);
    require(f.status("worker").state == ServiceState::running && f.status("worker").pid == 101 &&
            f.status("worker").restart_count == 0 && f.processes.startup_caps.back() == 7s,
            "manual reap completion cancelled restart or cap/budget changed");
    const auto results = f.coordinator.recovery->takeResults();
    require(results.size() == 1 && results[0].origin == RecoveryOrigin::manual_restart &&
            results[0].outcome == RecoveryOutcome::success, "manual did not emit one internal success");
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

void test_service_manager_has_no_automatic_retry_owner() {
    for (const auto policy : {RestartPolicy::never, RestartPolicy::on_failure, RestartPolicy::always}) {
        Fixture f;
        f.add("worker", policy);
        f.services.startService("worker", f.base);
        const auto old = f.status("worker");
        Event exit{EventType::process_exited, "worker", f.base, old.pid, 9};
        exit.instance_generation = old.launched_generation;
        f.services.handle(exit); // Deliberately no RM adapter in this test.
        f.services.tick(f.base + 1h);
        require(f.processes.launches.size() == 1 && f.status("worker").state == ServiceState::failed &&
                f.status("worker").restart_count == 0 && f.status("worker").pid == -1,
                "SM independently reserved or launched an automatic retry");
        require(!f.services.restartService("worker"), "unconnected restart facade pretended to route work");
    }
}
} // namespace

int main() {
    try {
        test_crash_backoff_and_limit();
        test_heartbeat_deadline_and_reaping();
        test_policy_matrix_and_launch_failure();
        test_terminal_failure_metadata_and_generations();
        test_shutdown_deadline_uses_all_service_grace_periods();
        test_dependency_order_and_shutdown();
        test_failure_propagation_and_recovery_cancellation();
        test_explicit_stop_and_blocked_launch();
        test_timeout_finalization_preserves_grace_pid_and_instance();
        test_timeout_after_force_does_not_restart_cleanup_grace();
        test_manual_restart_reap_and_remaining_startup_cap();
        test_invalid_graphs_have_no_side_effects();
        test_service_manager_has_no_automatic_retry_owner();
        std::cout << "recovery/dependency tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery/dependency test failed: " << error.what() << '\n';
        return 1;
    }
}
