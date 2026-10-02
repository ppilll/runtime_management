#include "runtime/event.hpp"
#include "runtime/recovery_manager.hpp"
#include <chrono>
#include <memory>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

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

// Deliberately no clock/launch work hidden in query. Only this fake executor
// issues lifecycle generations, modeling SM's existing generation ownership.
struct FakeExecutor : RecoveryExecutor {
    std::unordered_map<std::string, RecoveryExecutionSnapshot> services;
    unsigned launches = 0;
    std::optional<RecoveryOperation> last_operation;

    std::optional<RecoveryExecutionSnapshot> snapshot(const std::string& name) const override {
        const auto found = services.find(name);
        if (found == services.end()) return {};
        return found->second;
    }

    void fault(const std::string& name, std::uint64_t generation) {
        auto& service = services.at(name);
        service.generation = generation;
        service.state = RecoveryExecutionState::failed;
    }

    std::uint64_t launch(const RecoveryOperation& operation, int pid = 42, bool succeeds = true,
                         std::optional<std::uint64_t> preparation_generation = {}) {
        auto& service = services.at(operation.service_name);
        require(service.generation == preparation_generation.value_or(operation.expected_current_generation),
                "executor expected-generation mismatch");
        require(service.generation != std::numeric_limits<std::uint64_t>::max(), "executor lifecycle overflow");
        ++service.generation;
        service.launched_generation = service.generation;
        service.pid = pid;
        service.state = succeeds ? RecoveryExecutionState::running : RecoveryExecutionState::starting;
        last_operation = operation;
        ++launches;
        return service.generation;
    }
};

struct Fixture {
    FakeExecutor executor;
    RecoveryManager manager;
    Clock::time_point now = Clock::time_point{} + 100s;

    Fixture() : executor{}, manager({"alpha", "beta"}, executor) {
        executor.services.emplace("alpha", RecoveryExecutionSnapshot{10, 0, -1, RecoveryExecutionState::failed});
        executor.services.emplace("beta", RecoveryExecutionSnapshot{20, 0, -1, RecoveryExecutionState::failed});
    }

    RecoveryRequest request(const std::string& name = "alpha", std::uint64_t generation = 10) const {
        RecoveryRequest result;
        result.service_name = name;
        result.service_generation = generation;
        result.reason = "captured process fault";
        result.producer_time = now - 90s;
        return result;
    }

    void admit(const std::string& name = "alpha", std::uint64_t generation = 10) {
        require(manager.submit(request(name, generation), now, 30s) == RecoveryAdmission::admitted, "admission failed");
    }

    RecoveryOperation launch(const std::string& name = "alpha", int pid = 42) {
        const auto slot = manager.query(name);
        const auto operation = manager.beginExecution(name, slot->recovery_generation, now);
        require(operation.has_value(), "begin execution failed");
        const auto generation = executor.launch(*operation, pid);
        require(manager.bindExecution(*operation, generation, pid, true), "captured launch binding failed");
        return *operation;
    }

    RecoveryResult result(RecoveryOutcome outcome = RecoveryOutcome::success,
                          RecoveryTerminalReason reason = RecoveryTerminalReason::completed,
                          const std::string& name = "alpha") const {
        const auto slot = manager.query(name);
        const auto& active = *slot->active;
        RecoveryResult result;
        result.service_name = name;
        result.recovery_generation = active.context.recovery_generation;
        result.origin = active.context.origin;
        result.initial_fault_generation = active.context.initial_fault_generation;
        result.latest_fault_generation = active.latest_fault_generation;
        result.execution_generation = active.context.execution_generation;
        result.outcome = outcome;
        result.terminal_reason = reason;
        result.attempts_reserved_total = slot->attempts_reserved_total;
        result.completed_at = now - 50s;
        result.launched_pid = active.launched_pid;
        result.failure_type = active.request.failure_type;
        result.latest_failure_type = active.latest_failure_type;
        return result;
    }
};

struct PolicyExecutor : FakeExecutor {
    std::vector<std::string> launch_order;
    Clock::time_point* clock = nullptr;
    bool blocked = false;
    bool fail_launch = false;
    bool invalid_launch = false;
    Clock::duration launch_elapsed{};
    Clock::duration last_cap{};
    unsigned projected = 0;
    unsigned finishes = 0;
    bool gate = true;
    bool close_gate_on_launch = false;
    RecoveryExecutionReply prepareRecovery(const RecoveryOperation& operation, unsigned total, Clock::time_point) override {
        auto& current = services.at(operation.service_name);
        projected = total;
        if (operation.context.origin == RecoveryOrigin::manual_restart) {
            ++current.generation;
            current.state = current.pid > 0 ? RecoveryExecutionState::stopping : RecoveryExecutionState::stopped;
        } else if (!operation.finalize_only) current.state = RecoveryExecutionState::recovering;
        return {RecoveryStep::prepared, current};
    }
    bool recoveryReady(const RecoveryOperation&, Clock::time_point, const std::function<bool()>&) override { return !blocked; }
    RecoveryExecutionReply launchRecoveryAttempt(const RecoveryOperation& operation, std::uint64_t expected,
            Clock::duration cap, Clock::time_point) override {
        last_cap = cap;
        if (invalid_launch) return {};
        auto& current = services.at(operation.service_name);
        require(current.generation == expected && current.pid <= 0, "unsafe fake policy launch");
        ++launches;
        launch_order.push_back(operation.service_name);
        ++current.generation;
        current.launched_generation = current.generation;
        if (clock) *clock += launch_elapsed;
        if (close_gate_on_launch) gate = false;
        if (fail_launch) {
            ++current.generation;
            current.pid = -1;
            current.launched_generation = 0;
            current.state = RecoveryExecutionState::failed;
            return {RecoveryStep::failed, current, FailureType::startup_failure};
        }
        current.pid = 42;
        current.state = RecoveryExecutionState::running;
        return {RecoveryStep::launched, current};
    }
    RecoveryExecutionReply finishRecoveryFailure(const RecoveryOperation& operation, std::uint64_t expected,
            RecoveryTerminalReason, Clock::time_point) override {
        auto& current = services.at(operation.service_name);
        require(current.generation == expected, "finalize did not use captured token");
        if (current.state != RecoveryExecutionState::failed) ++current.generation;
        current.state = RecoveryExecutionState::failed;
        ++finishes;
        return {RecoveryStep::prepared, current};
    }
    void projectRestartCount(const std::string&, unsigned total) override { projected = total; }
};

struct PolicyFixture {
    Clock::time_point now = Clock::time_point{} + 100s;
    PolicyExecutor executor;
    ServiceConfig definition;
    std::unique_ptr<RecoveryManager> manager;
    explicit PolicyFixture(RestartPolicy policy = RestartPolicy::on_failure, std::chrono::seconds timeout = 174s) {
        definition.service_name = "alpha";
        definition.executable = "/fake/service";
        definition.restart_policy = policy;
        definition.recovery_timeout = timeout;
        executor.services.emplace("alpha", RecoveryExecutionSnapshot{10, 0, -1, RecoveryExecutionState::failed});
        executor.clock = &now;
        manager = std::make_unique<RecoveryManager>(std::vector<ServiceConfig>{definition}, executor,
            [this] { return now; }, [this] { return executor.gate; });
    }
    RecoveryRequest request() const {
        RecoveryRequest request;
        request.service_name = "alpha";
        request.service_generation = executor.services.at("alpha").generation;
        request.reason = "policy fact";
        return request;
    }
    void admit() { require(manager->submit(request(), now) == RecoveryAdmission::admitted, "policy admission failed"); }
    void tick(Clock::time_point at) { now = at; manager->tick(now); }
};

void test_policy_backoff_exhaustion_and_duplicate_budget() {
    PolicyFixture f;
    f.executor.fail_launch = true;
    f.admit();
    const auto episode = f.manager->query("alpha")->active->request;
    require(f.manager->submit(f.request(), f.now + 1s) == RecoveryAdmission::coalesced, "duplicate budget fact");
    unsigned attempt = 0;
    for (const auto delay : {2s, 4s, 8s, 16s, 32s}) {
        const auto reserved_at = f.now;
        ++attempt;
        const auto slot = f.manager->query("alpha");
        require(slot->attempts_reserved_total == attempt && slot->active->due == reserved_at + delay &&
                slot->active->request.deadline == episode.deadline, "retry budget/due/deadline changed");
        f.tick(reserved_at + delay - 1ms);
        require(f.executor.launches == attempt - 1, "retry launched before exact due");
        f.tick(reserved_at + delay);
        require(f.executor.launches == attempt, "retry missed exact due");
    }
    const auto results = f.manager->takeResults();
    require(results.size() == 1 && results[0].outcome == RecoveryOutcome::failed &&
            results[0].terminal_reason == RecoveryTerminalReason::retry_exhausted &&
            results[0].attempts_reserved_total == 5 && f.executor.finishes == 1, "missing sole exhaustion");
    f.tick(f.now + 1h);
    require(f.executor.launches == 5 && f.manager->takeResults().empty(), "exhaustion retried");
}

void test_policy_lifetime_success_cancel_and_manual_budget() {
    PolicyFixture f;
    for (unsigned attempt = 1; attempt <= 5; ++attempt) {
        f.executor.fault("alpha", f.executor.services.at("alpha").generation + 1);
        f.executor.services.at("alpha").pid = -1;
        f.admit();
        f.tick(*f.manager->query("alpha")->active->due);
        require(!f.manager->query("alpha")->active &&
                f.manager->query("alpha")->attempts_reserved_total == attempt, "success reset budget");
    }
    auto manual = f.request();
    manual.origin = RecoveryOrigin::manual_restart;
    manual.failure_type = FailureType::manual_request;
    require(f.manager->submit(manual, f.now) == RecoveryAdmission::admitted, "manual admission");
    require(f.manager->submit(manual, f.now + 1s) == RecoveryAdmission::coalesced, "manual duplicated");
    // Simulated reap preserves the captured stop token, clearing only instance identity.
    f.executor.services.at("alpha").pid = -1;
    f.executor.services.at("alpha").launched_generation = 0;
    f.executor.services.at("alpha").state = RecoveryExecutionState::stopped;
    f.tick(f.now);
    require(f.manager->query("alpha")->attempts_reserved_total == 5 &&
            f.executor.launches == 6, "manual consumed/reset budget");
    f.executor.fault("alpha", f.executor.services.at("alpha").generation + 1);
    f.executor.services.at("alpha").pid = -1;
    f.admit();
    require(!f.manager->query("alpha")->active && f.executor.launches == 6 &&
            f.manager->query("alpha")->last_result->terminal_reason == RecoveryTerminalReason::retry_exhausted,
            "sixth fault did not terminate without launch");
    PolicyFixture cancelled;
    cancelled.admit();
    cancelled.manager->cancel("alpha", RecoveryTerminalReason::explicit_stop, cancelled.now);
    require(cancelled.manager->query("alpha")->attempts_reserved_total == 1, "cancel refunded reservation");
}

void test_policy_matrix_and_absolute_deadline() {
    for (const auto policy : {RestartPolicy::never, RestartPolicy::on_failure, RestartPolicy::always}) {
        PolicyFixture f(policy);
        for (const int status : {0, 1 << 8, 9, -1}) {
            require(f.manager->classifyExit("alpha", status) ==
                    (status == 0 && policy != RestartPolicy::always ? ExitDisposition::normal_stopped :
                        ExitDisposition::recoverable_failure), "exit disposition matrix");
        }
        for (const auto type : {FailureType::process_crash, FailureType::clean_exit_restart,
                               FailureType::startup_failure, FailureType::startup_timeout, FailureType::heartbeat_timeout}) {
            auto request = f.request();
            request.failure_type = type;
            const bool enabled = policy != RestartPolicy::never &&
                (type != FailureType::clean_exit_restart || policy == RestartPolicy::always);
            const auto admission = f.manager->submit(request, f.now);
            require(admission == (enabled ? RecoveryAdmission::admitted : RecoveryAdmission::suppressed_policy), "policy admission matrix");
            if (enabled) f.manager->cancel("alpha", RecoveryTerminalReason::explicit_stop, f.now);
            f.executor.fault("alpha", f.executor.services.at("alpha").generation + 1);
        }
        if (policy == RestartPolicy::never) require(f.manager->takeResults().empty(), "never emitted terminal");
    }
    for (const auto elapsed : {7999ms, 8000ms, 8001ms}) {
        PolicyFixture f(RestartPolicy::on_failure, 10s);
        f.executor.launch_elapsed = elapsed;
        f.admit();
        f.tick(f.now + 2s);
        const auto result = f.manager->query("alpha")->last_result;
        require(result && result->outcome == (elapsed < 8s ? RecoveryOutcome::success : RecoveryOutcome::timeout),
                "return checkpoint did not prefer timeout");
        require(f.executor.last_cap == 8s, "remaining startup budget not passed");
        if (elapsed >= 8s) require(f.executor.services.at("alpha").pid == 42 &&
                result->latest_fault_generation == f.executor.services.at("alpha").generation,
                "timeout lost PID or final captured token");
    }
    PolicyFixture simultaneous(RestartPolicy::on_failure, 2s);
    simultaneous.admit();
    simultaneous.tick(simultaneous.now + 2s);
    require(simultaneous.executor.launches == 0 && simultaneous.executor.finishes == 1 &&
            simultaneous.manager->query("alpha")->last_result->outcome == RecoveryOutcome::timeout,
            "due won over deadline");
}

void test_policy_blocked_invariant_manual_failure_and_launch_gate() {
    PolicyFixture blocked(RestartPolicy::on_failure, 5s);
    blocked.executor.services.at("alpha").pid = 99;
    blocked.executor.services.at("alpha").launched_generation = 9;
    blocked.admit();
    blocked.tick(blocked.now + 2s);
    require(blocked.executor.launches == 0 && blocked.manager->query("alpha")->attempts_reserved_total == 1,
            "unreaped PID launched or reserved again");
    blocked.tick(blocked.now + 3s);
    require(blocked.executor.services.at("alpha").pid == 99 && blocked.executor.finishes == 1, "blocked timeout lost PID");
    PolicyFixture dependency(RestartPolicy::on_failure, 5s);
    dependency.executor.blocked = true;
    dependency.admit();
    dependency.tick(dependency.now + 5s);
    require(dependency.executor.launches == 0 && dependency.executor.finishes == 1, "blocked dependency never terminated");
    PolicyFixture invalid;
    invalid.executor.invalid_launch = true;
    invalid.admit();
    invalid.tick(invalid.now + 2s);
    require(invalid.manager->query("alpha")->last_result->terminal_reason == RecoveryTerminalReason::execution_failure &&
            invalid.executor.finishes == 1, "executor invariant did not terminate once");
    PolicyFixture manual;
    manual.executor.fail_launch = true;
    auto request = manual.request();
    request.origin = RecoveryOrigin::manual_restart;
    request.failure_type = FailureType::manual_request;
    manual.manager->submit(request, manual.now);
    manual.tick(manual.now);
    const auto results = manual.manager->takeResults();
    require(results.size() == 1 && results[0].origin == RecoveryOrigin::manual_restart &&
            results[0].outcome == RecoveryOutcome::failed && manual.manager->query("alpha")->active &&
            manual.manager->query("alpha")->active->context.origin == RecoveryOrigin::automatic_failure &&
            manual.manager->query("alpha")->attempts_reserved_total == 1, "manual failure did not create independent auto episode");
    PolicyFixture gated;
    gated.admit();
    gated.executor.gate = false;
    gated.tick(gated.now + 2s);
    require(gated.executor.launches == 0 && gated.manager->query("alpha")->last_result->outcome == RecoveryOutcome::cancelled,
            "shutdown gate allowed replacement");
    PolicyFixture during;
    during.admit();
    during.executor.close_gate_on_launch = true;
    during.tick(during.now + 2s);
    require(during.manager->query("alpha")->last_result->outcome == RecoveryOutcome::cancelled,
            "late success survived shutdown return checkpoint");
}

void test_multi_due_order_one_launch_and_offline_cancellation() {
    Clock::time_point now = Clock::time_point{} + 100s;
    PolicyExecutor executor;
    executor.clock = &now;
    std::vector<ServiceConfig> definitions;
    // This is the topology/name order passed by the production SM registry.
    for (const auto* name : {"base", "left", "right"}) {
        ServiceConfig config;
        config.service_name = name;
        config.executable = "/fake/service";
        config.restart_policy = RestartPolicy::on_failure;
        if (config.service_name != "base") config.dependency = {"base"};
        definitions.push_back(config);
        executor.services.emplace(name, RecoveryExecutionSnapshot{10, 0, -1, RecoveryExecutionState::failed});
    }
    RecoveryManager manager(definitions, executor, [&] { return now; });
    auto request = [](const char* name) {
        RecoveryRequest fact;
        fact.service_name = name;
        fact.service_generation = 10;
        fact.reason = "simultaneous failures";
        return fact;
    };
    for (const auto* name : {"right", "left", "base"})
        require(manager.submit(request(name), now) == RecoveryAdmission::admitted, "multi admission");
    require(manager.takeStarts().size() == 3, "canonical START receipts missing");
    now += 2s;
    for (unsigned turn = 1; turn <= 3; ++turn) {
        manager.tick(now);
        require(executor.launches == turn && manager.takeResults().size() == 1,
                "more than one recovery launch/result in a writer turn");
    }
    require(executor.launch_order == std::vector<std::string>{"base", "left", "right"},
            "equal due ignored topology/name tie order");

    PolicyExecutor priority;
    priority.clock = &now;
    for (const auto& config : definitions)
        priority.services.emplace(config.service_name, RecoveryExecutionSnapshot{10, 0, -1, RecoveryExecutionState::failed});
    RecoveryManager ordered(definitions, priority, [&] { return now; });
    unsigned offset = 0;
    for (const auto* name : {"right", "left", "base"})
        ordered.submit(request(name), now + std::chrono::milliseconds(offset++));
    now += 2s + 2ms;
    for (unsigned turn = 0; turn < 3; ++turn) ordered.tick(now);
    require(priority.launch_order == std::vector<std::string>{"right", "left", "base"},
            "topology tie order overrode an earlier due permission");

    PolicyExecutor pending;
    pending.clock = &now;
    for (auto& config : definitions) {
        config.recovery_timeout = config.service_name == "base" ? 1s : 20s;
        pending.services.emplace(config.service_name, RecoveryExecutionSnapshot{10, 0, -1, RecoveryExecutionState::failed});
    }
    RecoveryManager offline(definitions, pending, [&] { return now; });
    for (const auto* name : {"right", "left", "base"}) offline.submit(request(name), now);
    now += 2s;
    offline.tick(now);
    const auto terminal = offline.takeResults();
    require(terminal.size() == 1 && terminal[0].service_name == "base" &&
            terminal[0].outcome == RecoveryOutcome::timeout && pending.launches == 0,
            "other due launch overtook terminal health checkpoint");
    offline.stopAutomatic(now); // Runtime commits OFFLINE, then invokes this gate.
    require(offline.takeResults().size() == 2 && !offline.query("left")->active && !offline.query("right")->active,
            "OFFLINE retained other pending tasks");
    offline.tick(now + 1h);
    require(pending.launches == 0 && offline.takeResults().empty() &&
            offline.submit(request("left"), now) == RecoveryAdmission::suppressed_policy,
            "OFFLINE relaunched an automatic task");
    auto manual = request("left");
    manual.origin = RecoveryOrigin::manual_restart;
    manual.failure_type = FailureType::manual_request;
    offline.submit(manual, now);
    offline.tick(now);
    require(pending.launches == 1 && offline.query("left")->attempts_reserved_total == 1 &&
            offline.query("left")->last_result->origin == RecoveryOrigin::manual_restart,
            "manual after OFFLINE reset budget or was incorrectly disabled");
}

void test_manual_never_failure_and_shutdown_in_each_phase() {
    PolicyFixture never(RestartPolicy::never);
    never.executor.fail_launch = true;
    auto request = never.request();
    request.origin = RecoveryOrigin::manual_restart;
    request.failure_type = FailureType::manual_request;
    never.manager->submit(request, never.now);
    never.tick(never.now);
    const auto receipts = never.manager->takeResults();
    require(receipts.size() == 1 && receipts[0].origin == RecoveryOrigin::manual_restart &&
            receipts[0].outcome == RecoveryOutcome::failed && !never.manager->query("alpha")->active &&
            never.manager->query("alpha")->attempts_reserved_total == 0 && never.manager->takeStarts().empty(),
            "never manual failure generated an automatic terminal or reservation");
    for (const unsigned phase : {0u, 1u, 2u}) {
        PolicyFixture f;
        auto fact = f.request();
        if (phase == 2) {
            fact.origin = RecoveryOrigin::manual_restart;
            fact.failure_type = FailureType::manual_request;
        }
        f.manager->submit(fact, f.now);
        if (phase == 1) {
            f.now += 2s;
            require(f.manager->beginExecution("alpha", 1, f.now).has_value(), "EXECUTING fixture");
        }
        f.manager->cancelAll(f.now);
        f.tick(f.now + 1h);
        const auto results = f.manager->takeResults();
        require(results.size() == 1 && results[0].outcome == RecoveryOutcome::cancelled &&
                results[0].terminal_reason == RecoveryTerminalReason::shutdown && f.executor.launches == 0 &&
                f.manager->query("alpha")->attempts_reserved_total == (phase == 2 ? 0u : 1u),
                "shutdown retained permission or refunded/reset budget");
    }
}

void test_admission_metadata_and_snapshot() {
    Fixture fixture;
    auto request = fixture.request();
    request.service_name = "unknown";
    require(fixture.manager.submit(request, fixture.now, 30s) == RecoveryAdmission::invalid, "unknown service admitted");
    request = fixture.request();
    request.reason.clear();
    require(fixture.manager.submit(request, fixture.now, 30s) == RecoveryAdmission::invalid, "empty metadata admitted");
    request = fixture.request();
    request.service_generation = 0;
    require(fixture.manager.submit(request, fixture.now, 30s) == RecoveryAdmission::invalid, "missing fault token admitted");
    for (const auto generation : {9u, 11u}) {
        request = fixture.request("alpha", generation);
        require(fixture.manager.submit(request, fixture.now, 30s) == RecoveryAdmission::stale, "old/future fault admitted");
    }
    request = fixture.request();
    request.recovery_generation = 1;
    require(fixture.manager.submit(request, fixture.now, 30s) == RecoveryAdmission::invalid, "producer issued episode");
    request = fixture.request();
    request.origin = static_cast<RecoveryOrigin>(999);
    require(fixture.manager.submit(request, fixture.now, 30s) == RecoveryAdmission::invalid, "unknown origin admitted");
    request = fixture.request();
    request.failure_type = static_cast<FailureType>(999);
    require(fixture.manager.submit(request, fixture.now, 30s) == RecoveryAdmission::invalid, "unknown failure admitted");
    require(fixture.manager.submit(fixture.request(), fixture.now, 0s) == RecoveryAdmission::invalid, "zero timeout admitted");
    fixture.admit();
    auto copy = fixture.manager.query("alpha");
    require(copy->recovery_generation == 1 && copy->active->request.accepted_at == fixture.now &&
            copy->active->request.deadline == fixture.now + 30s && copy->active->request.producer_time == fixture.now - 90s,
            "writer admission time or metadata lost");
    copy->active.reset();
    require(fixture.manager.query("alpha")->active.has_value(), "query leaked mutable registry state");
    require(!fixture.manager.query("unknown"), "unknown service snapshot fabricated");
    require(fixture.executor.launches == 0, "admission performed lifecycle work");
}

void test_duplicate_submit_and_result() {
    Fixture fixture;
    fixture.admit();
    auto duplicate = fixture.request();
    duplicate.reason = "different diagnostic wording";
    duplicate.producer_time = fixture.now + 1h;
    require(fixture.manager.submit(duplicate, fixture.now + 1s, 60s) == RecoveryAdmission::coalesced, "duplicate not coalesced");
    auto slot = fixture.manager.query("alpha");
    require(slot->recovery_generation == 1 && slot->active->request.deadline == fixture.now + 30s &&
            slot->attempts_reserved_total == 0 && fixture.manager.takeResults().empty(), "duplicate changed episode");
    fixture.launch();
    require(fixture.manager.submit(duplicate, fixture.now + 2s, 60s) == RecoveryAdmission::coalesced,
            "original duplicate not coalesced after fault-launch bridge");
    const auto success = fixture.result();
    require(fixture.manager.acceptsResult(success, fixture.now), "correct result gate rejected");
    require(fixture.manager.observe(success, fixture.now), "correct result rejected");
    require(!fixture.manager.observe(success, fixture.now) && !fixture.manager.acceptsResult(success, fixture.now),
            "duplicate result accepted after terminal");
    const auto results = fixture.manager.takeResults();
    require(results.size() == 1 && results[0].outcome == RecoveryOutcome::success && results[0].completed_at == fixture.now,
            "terminal output not sealed once at writer checkpoint");
    require(fixture.manager.takeResults().empty() && !fixture.manager.query("alpha")->active, "terminal not drained/closed");
    fixture.executor.fault("alpha", 12);
    fixture.admit("alpha", 12);
    require(fixture.manager.query("alpha")->recovery_generation == 2, "episode reset after success");
}

void test_result_gate_all_identity_fields() {
    Fixture fixture;
    fixture.admit();
    const auto before_launch = fixture.result();
    require(!fixture.manager.observe(before_launch, fixture.now), "success without execution accepted");
    fixture.launch();
    const auto good = fixture.result();
    auto reject = [&](RecoveryResult result) {
        require(!fixture.manager.acceptsResult(result, fixture.now) && !fixture.manager.observe(result, fixture.now),
                "identity gate accepted invalid result");
        require(fixture.manager.query("alpha")->active.has_value() && fixture.manager.takeResults().empty(),
                "rejected result changed state/output");
    };
    auto bad = good; bad.service_name = "unknown"; reject(bad);
    bad = good; bad.recovery_generation = 0; reject(bad);
    bad = good; bad.recovery_generation = 2; reject(bad);
    bad = good; bad.origin = RecoveryOrigin::manual_restart; reject(bad);
    bad = good; bad.initial_fault_generation = 11; reject(bad);
    bad = good; bad.latest_fault_generation = 11; reject(bad);
    bad = good; bad.execution_generation.reset(); reject(bad);
    bad = good; bad.execution_generation = 10; reject(bad);
    bad = good; bad.execution_generation = 12; reject(bad);
    bad = good; bad.launched_pid.reset(); reject(bad);
    bad = good; bad.launched_pid = 43; reject(bad);
    bad = good; bad.attempts_reserved_total = 1; reject(bad);
    bad = good; bad.failure_type = FailureType::heartbeat_timeout; reject(bad);
    bad = good; bad.latest_failure_type = FailureType::heartbeat_timeout; reject(bad);
    bad = good; bad.terminal_reason = RecoveryTerminalReason::recovery_timeout; reject(bad);
    bad = good; bad.outcome = static_cast<RecoveryOutcome>(999); reject(bad);
    auto& current = fixture.executor.services.at("alpha");
    const auto saved = current;
    current.generation = 12; reject(good); current = saved;
    current.launched_generation = 10; reject(good); current = saved;
    current.pid = 43; reject(good); current = saved;
    current.state = RecoveryExecutionState::starting; reject(good); current = saved;
    require(fixture.manager.observe(good, fixture.now), "valid captured bridge result rejected");
    const auto result = fixture.manager.query("alpha")->last_result;
    require(result->initial_fault_generation == 10 && result->execution_generation == 11 &&
            result->latest_fault_generation == 10, "fault and execution identity conflated");
}

void test_bound_attempt_failure_invalidates_old_execution() {
    Fixture fixture;
    fixture.admit();
    const auto operation = fixture.launch();
    const auto old_result = fixture.result();
    const auto captured = fixture.manager.query("alpha")->active->context;
    fixture.executor.fault("alpha", 12);
    auto wrong = captured;
    wrong.recovery_generation = 2;
    require(!fixture.manager.observeFailure("alpha", wrong, 12, FailureType::startup_failure, "failed launch"),
            "unbound attempt failure accepted");
    require(fixture.manager.observeFailure("alpha", captured, 12, FailureType::startup_failure, "failed launch"),
            "bound attempt failure rejected");
    require(!fixture.manager.observeFailure("alpha", captured, 12, FailureType::startup_failure, "duplicate"),
            "duplicate attempt failure advanced state");
    require(!fixture.manager.observe(old_result, fixture.now), "old execution survived bound failure");
    require(!fixture.manager.bindExecution(operation, 11, 42, true), "old launch binding resurrected");
    const auto slot = fixture.manager.query("alpha");
    require(slot->recovery_generation == 1 && slot->active->expected_generation == 12 &&
            slot->active->latest_fault_generation == 12 && slot->active->request.service_generation == 10 &&
            !slot->active->context.execution_generation && slot->active->state == RecoveryState::backoff,
            "attempt failure lost episode/initial identity");
    fixture.launch(); // Fake reuses PID=42, but SM launch generation is now 13.
    require(!fixture.manager.observe(old_result, fixture.now), "PID reuse accepted old execution");
    require(fixture.manager.observe(fixture.result(), fixture.now), "next captured execution failed gate");
    require(fixture.executor.launches == 2 && fixture.manager.takeResults().size() == 1, "attempt failure emitted terminal");
}

void test_independent_fault_supersedes_episode() {
    Fixture fixture;
    fixture.admit();
    const auto old_operation = fixture.launch();
    const auto old_result = fixture.result();
    fixture.executor.fault("alpha", 12); // No old operation context: independent failure.
    fixture.admit("alpha", 12);
    const auto slot = fixture.manager.query("alpha");
    require(slot->recovery_generation == 2 && slot->active->context.initial_fault_generation == 12,
            "independent failure did not replace episode");
    require(!fixture.manager.observe(old_result, fixture.now) && !fixture.manager.bindExecution(old_operation, 11, 42, true),
            "superseded operation accepted");
    auto receipts = fixture.manager.takeResults();
    require(receipts.size() == 1 && receipts[0].outcome == RecoveryOutcome::cancelled &&
            receipts[0].terminal_reason == RecoveryTerminalReason::superseded && receipts[0].recovery_generation == 1,
            "supersede did not cancel exactly once");
    fixture.launch();
    require(fixture.manager.observe(fixture.result(), fixture.now), "replacement episode failed");
}

void test_failed_launch_without_pid_and_latest_fault_duplicate() {
    Fixture fixture;
    fixture.admit();
    const auto operation = fixture.manager.beginExecution("alpha", 1, fixture.now);
    require(operation.has_value(), "failed launch operation missing");
    // Synchronous exec failure may have no live PID/STARTING snapshot by drain
    // time. Its original captured operation still binds FAILED generation 12.
    fixture.executor.fault("alpha", 12);
    require(fixture.manager.observeFailure("alpha", operation->context, 12, FailureType::startup_failure, "exec failed"),
            "failed launch without PID rejected");
    auto duplicate = fixture.request("alpha", 12);
    duplicate.failure_type = FailureType::startup_failure;
    require(fixture.manager.submit(duplicate, fixture.now + 1s, 90s) == RecoveryAdmission::coalesced,
            "bound latest-fault duplicate replaced episode");
    auto different = duplicate;
    different.failure_type = FailureType::heartbeat_timeout;
    require(fixture.manager.submit(different, fixture.now + 1s, 90s) == RecoveryAdmission::stale,
            "different fault type reused current token");
    auto result = fixture.result(RecoveryOutcome::failed, RecoveryTerminalReason::execution_failure);
    require(result.initial_fault_generation == 10 && result.latest_fault_generation == 12 &&
            !result.execution_generation && result.latest_failure_type == FailureType::startup_failure,
            "no-PID attempt lost fault identity");
    require(fixture.manager.observe(result, fixture.now), "captured no-launch failure result rejected");
    require(fixture.manager.submit(duplicate, fixture.now, 30s) == RecoveryAdmission::coalesced,
            "latest failure readmitted after terminal");
    require(fixture.manager.takeResults().size() == 1 && fixture.executor.launches == 0,
            "no-PID failure duplicated output or invented launch");
}

void test_cancel_shutdown_and_service_isolation() {
    Fixture fixture;
    fixture.admit();
    fixture.admit("beta", 20);
    fixture.launch();
    const auto old = fixture.result();
    // An explicit stop advances SM generation; cancel must revoke the old work
    // without using that stop token to fabricate an automatic failure result.
    fixture.executor.services.at("alpha").generation = 12;
    fixture.executor.services.at("alpha").state = RecoveryExecutionState::stopping;
    require(!fixture.manager.observe(old, fixture.now), "stop allowed late success");
    require(!fixture.manager.cancel("alpha", RecoveryTerminalReason::completed, fixture.now), "invalid cancel reason");
    require(fixture.manager.cancel("alpha", RecoveryTerminalReason::explicit_stop, fixture.now), "cancel rejected");
    require(!fixture.manager.cancel("alpha", RecoveryTerminalReason::explicit_stop, fixture.now), "duplicate cancel emitted");
    require(fixture.manager.query("beta")->active.has_value(), "cancel leaked across services");
    fixture.manager.cancelAll(fixture.now);
    fixture.manager.cancelAll(fixture.now);
    const auto results = fixture.manager.takeResults();
    require(results.size() == 2 && results[0].terminal_reason == RecoveryTerminalReason::explicit_stop &&
            results[1].terminal_reason == RecoveryTerminalReason::shutdown, "cancel/shutdown outputs duplicated");
    require(fixture.manager.submit(fixture.request("beta", 20), fixture.now, 30s) == RecoveryAdmission::rejected_shutdown,
            "admission after shutdown");
    require(!fixture.manager.beginExecution("beta", 1, fixture.now) && !fixture.manager.observe(old, fixture.now),
            "shutdown revived operation");
    require(!fixture.manager.query("beta")->active && fixture.executor.launches == 1, "shutdown launched/retained active");
}

void test_manual_supersede_coalesce_and_preparation() {
    Fixture fixture;
    fixture.admit();
    auto manual = fixture.request();
    manual.origin = RecoveryOrigin::manual_restart;
    manual.failure_type = FailureType::manual_request;
    manual.reason = "manual request";
    require(fixture.manager.submit(manual, fixture.now, 30s) == RecoveryAdmission::admitted, "manual did not supersede auto");
    const auto cancelled = fixture.manager.takeResults();
    require(cancelled.size() == 1 && cancelled[0].terminal_reason == RecoveryTerminalReason::superseded_manual,
            "manual supersede receipt wrong");
    require(fixture.manager.submit(manual, fixture.now + 1s, 90s) == RecoveryAdmission::coalesced, "duplicate manual admitted");
    const auto operation = fixture.manager.beginExecution("alpha", 2, fixture.now);
    require(operation.has_value(), "manual operation missing");
    auto& current = fixture.executor.services.at("alpha");
    current.generation = 11;
    current.state = RecoveryExecutionState::stopping;
    require(fixture.manager.observePreparation(*operation, 11), "captured stop preparation rejected");
    current.state = RecoveryExecutionState::stopped;
    require(fixture.manager.observePreparation(*operation, 11), "same-token STOPPED rejected");
    manual.service_generation = 11;
    require(fixture.manager.submit(manual, fixture.now + 2s, 90s) == RecoveryAdmission::coalesced,
            "manual stop token failed duplicate coalescing");
    require(!fixture.manager.observePreparation(*operation, 10), "stale preparation accepted");
    // The executor retains the original operation context throughout stop/reap;
    // the launch primitive checks the now-captured expected generation.
    const auto execution_generation = fixture.executor.launch(*operation, 42, true, 11);
    require(execution_generation == 12 && fixture.executor.launches == 1, "manual launch lost captured stop checkpoint");
    require(fixture.manager.bindExecution(*operation, execution_generation, 42, true), "manual stop-launch bridge rejected");
    require(fixture.manager.observe(fixture.result(), fixture.now), "manual result gate failed");
    require(fixture.manager.query("alpha")->attempts_reserved_total == 0, "manual changed automatic budget");
    require(fixture.manager.takeResults()[0].origin == RecoveryOrigin::manual_restart, "manual reported automatic origin");
}

void test_deadline_result_idempotency_and_producer_time() {
    for (const auto outcome : {RecoveryOutcome::failed, RecoveryOutcome::timeout}) {
        Fixture fixture;
        auto request = fixture.request();
        request.producer_time = fixture.now + 1h; // Future metadata cannot change identity or deadline.
        require(fixture.manager.submit(request, fixture.now, 30s) == RecoveryAdmission::admitted, "future producer time rejected");
        const auto checkpoint = outcome == RecoveryOutcome::timeout ? fixture.now + 30s : fixture.now;
        const auto reason = outcome == RecoveryOutcome::timeout ? RecoveryTerminalReason::recovery_timeout
                                                               : RecoveryTerminalReason::execution_failure;
        auto result = fixture.result(outcome, reason);
        result.completed_at = fixture.now - 1h;
        if (outcome == RecoveryOutcome::timeout)
            require(!fixture.manager.observe(result, checkpoint - 1ms), "early timeout accepted");
        require(fixture.manager.observe(result, checkpoint), "valid terminal rejected");
        require(!fixture.manager.observe(result, checkpoint), "duplicate terminal accepted");
        const auto results = fixture.manager.takeResults();
        require(results.size() == 1 && results[0].completed_at == checkpoint && !results[0].execution_generation,
                "no-launch terminal invented execution or trusted timestamp");
        require(fixture.manager.submit(request, checkpoint, 30s) == RecoveryAdmission::coalesced,
                "terminal duplicate fault readmitted");
    }
    Fixture fixture;
    fixture.admit();
    fixture.launch();
    auto success = fixture.result();
    require(fixture.manager.acceptsResult(success, fixture.now + 30s - 1ms), "deadline-1ms rejected success");
    success.completed_at = fixture.now - 10s;
    require(!fixture.manager.observe(success, fixture.now + 30s) &&
            !fixture.manager.observe(success, fixture.now + 31s), "producer timestamp bypassed actual deadline");
    auto timeout = fixture.result(RecoveryOutcome::timeout, RecoveryTerminalReason::recovery_timeout);
    require(fixture.manager.observe(timeout, fixture.now + 30s), "timeout did not win at deadline");
    require(fixture.manager.takeResults().size() == 1, "deadline duplicate output");
    Fixture waiting;
    waiting.admit();
    require(!waiting.manager.beginExecution("alpha", 1, waiting.now + 30s) && waiting.executor.launches == 0,
            "execution authorized at deadline");
}

void test_captured_binding_and_launch_handshake() {
    Fixture fixture;
    fixture.admit();
    const auto operation = fixture.manager.beginExecution("alpha", 1, fixture.now);
    require(operation.has_value(), "operation missing");
    require(!fixture.manager.beginExecution("alpha", 1, fixture.now), "duplicate execution authorized");
    const auto generation = fixture.executor.launch(*operation, 42, false);
    auto forged = *operation;
    forged.expected_current_generation = 11;
    require(!fixture.manager.bindExecution(forged, generation, 42, false), "completion-time operation relabel accepted");
    forged = *operation;
    forged.context.recovery_generation = 2;
    require(!fixture.manager.bindExecution(forged, generation, 42, false), "future episode binding accepted");
    require(!fixture.manager.bindExecution(*operation, 10, 42, false), "fault token reused as execution");
    require(fixture.manager.bindExecution(*operation, generation, 42, false), "captured STARTING rejected");
    require(!fixture.manager.observe(fixture.result(), fixture.now), "STARTING accepted as success");
    fixture.executor.services.at("alpha").state = RecoveryExecutionState::running;
    require(!fixture.manager.observe(fixture.result(), fixture.now), "unconfirmed exec handshake accepted");
    require(fixture.manager.bindExecution(*operation, generation, 42, true), "confirmed RUNNING binding rejected");
    require(fixture.manager.bindExecution(*operation, generation, 42, true), "duplicate launch binding not idempotent");
    require(fixture.manager.observe(fixture.result(), fixture.now), "confirmed result rejected");
}

void test_overflow_and_registry() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    require(RecoveryManager::nextGeneration(0) == 1 && RecoveryManager::nextGeneration(maximum - 1) == maximum,
            "generation increment skipped/wrapped");
    rejects([&] { RecoveryManager::nextGeneration(maximum); }, "episode overflow silently wrapped");
    Fixture fixture;
    rejects([&] { fixture.manager.submit(fixture.request(), Clock::time_point::max() - 1s, 30s); },
            "deadline overflow accepted");
    require(!fixture.manager.query("alpha")->active && fixture.manager.query("alpha")->recovery_generation == 0,
            "overflow partially admitted episode");
    rejects([&] { RecoveryManager invalid({"alpha", "alpha"}, fixture.executor); }, "duplicate registry accepted");
    rejects([&] { RecoveryManager invalid({""}, fixture.executor); }, "empty registry accepted");
}

void test_event_envelope_tail_and_fifo() {
    Fixture fixture;
    fixture.admit();
    fixture.launch();
    const auto context = fixture.manager.query("alpha")->active->context;
    Event command{EventType::restart_request, "alpha", fixture.now + 1h};
    command.instance_generation = 11;
    command.recovery_context = context;
    Event result{EventType::recovery_result, "alpha", fixture.now - 1h};
    result.recovery_result = fixture.result();
    EventQueue queue;
    queue.push(command);
    queue.push(result);
    Event received{};
    require(queue.pop_for(received, 0ms) && received.type == EventType::restart_request &&
            received.instance_generation == 11 && received.recovery_context->recovery_generation == 1,
            "tail metadata lost/FIFO reordered by timestamp");
    require(queue.pop_for(received, 0ms) && received.type == EventType::recovery_result && received.recovery_result &&
            fixture.manager.observe(*received.recovery_result, fixture.now), "result envelope lost or gate rejected");
    RuntimeEvent fact{RuntimeEventType::recovery_success, "alpha", "arbitrary source", "diagnostic", fixture.now};
    fact.generation = 11;
    fact.recovery_context = context;
    require(fact.recovery_context->initial_fault_generation == 10, "runtime fact bridge lost initial fault");
    // Source/reason cannot authenticate results: even naming recovery_manager
    // cannot bypass the typed gate when the episode is already closed.
    fact.source = "recovery_manager";
    require(!fixture.manager.observe(*received.recovery_result, fixture.now), "source spelling bypassed active gate");
}

} // namespace

int main() {
    try {
        test_policy_backoff_exhaustion_and_duplicate_budget();
        test_policy_lifetime_success_cancel_and_manual_budget();
        test_policy_matrix_and_absolute_deadline();
        test_policy_blocked_invariant_manual_failure_and_launch_gate();
        test_multi_due_order_one_launch_and_offline_cancellation();
        test_manual_never_failure_and_shutdown_in_each_phase();
        test_admission_metadata_and_snapshot();
        test_duplicate_submit_and_result();
        test_result_gate_all_identity_fields();
        test_bound_attempt_failure_invalidates_old_execution();
        test_independent_fault_supersedes_episode();
        test_failed_launch_without_pid_and_latest_fault_duplicate();
        test_cancel_shutdown_and_service_isolation();
        test_manual_supersede_coalesce_and_preparation();
        test_deadline_result_idempotency_and_producer_time();
        test_captured_binding_and_launch_handshake();
        test_overflow_and_registry();
        test_event_envelope_tail_and_fifo();
        std::cout << "recovery_manager_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
