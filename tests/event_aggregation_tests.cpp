#include "runtime/runtime_manager.hpp"
#include "runtime/service_aggregation.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <deque>
#include <memory>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace runtime;
using namespace std::chrono_literals;
const Clock::time_point base{100s};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

RuntimeEvent fact(RuntimeEventType type, std::string service = {}) {
    return {type, std::move(service), "test producer", "explicit health fact", base};
}

struct Fixture {
    std::vector<DeviceStateSnapshot> changes;
    DeviceStateManager states{[this](const DeviceStateSnapshot& state) { changes.push_back(state); }, base};
    ServiceAggregation aggregation;
    std::map<std::string, std::uint64_t> generations;
    std::map<std::string, RecoveryContext> contexts;
    std::map<std::string, std::uint64_t> episodes;

    explicit Fixture(AggregationOptions options = {}) : aggregation(states, std::move(options)) {
        aggregation.add_service("control_service");
        aggregation.add_service("vision_service");
        aggregation.add_service("ota_service");
        require(states.handle({DeviceStateEventType::runtime_initialized, "runtime_manager", "initialized", base})
                    == DeviceTransitionResult::transitioned, "initialization failed");
    }
    void send(RuntimeEventType type, const std::string& name) {
        auto event = fact(type, name);
        if (!name.empty()) {
            if (type == RuntimeEventType::recovery_start) {
                contexts[name] = RecoveryContext{++episodes[name], generations[name], {}, RecoveryOrigin::automatic_failure};
                event.recovery_context = contexts[name];
            } else if (type == RuntimeEventType::recovery_success) {
                if (!contexts.count(name)) send(RuntimeEventType::recovery_start, name);
                auto& context = contexts[name];
                context.execution_generation = ++generations[name];
                auto candidate = fact(RuntimeEventType::service_started, name);
                candidate.generation = generations[name];
                candidate.recovery_context = context;
                require(aggregation.handle(candidate), "candidate rejected");
                event.recovery_context = context;
            } else if (type == RuntimeEventType::recovery_failed) {
                event.recovery_context = contexts.at(name);
            } else {
                ++generations[name];
                contexts.erase(name);
            }
            event.generation = generations[name];
        }
        require(aggregation.handle(event), "health event rejected");
        if (type == RuntimeEventType::recovery_success || type == RuntimeEventType::recovery_failed)
            contexts.erase(name);
    }
    void running() {
        send(RuntimeEventType::service_started, "control_service");
        send(RuntimeEventType::service_started, "vision_service");
        send(RuntimeEventType::service_started, "ota_service");
        expect(DeviceState::running);
    }
    void expect(DeviceState state) { require(states.query().current == state, "wrong aggregate device state"); }
};

void test_vision_failure_case1() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "vision_service");
    fixture.expect(DeviceState::warning);
    const auto state = fixture.states.query();
    require(state.previous == DeviceState::running && state.timestamp == base &&
            state.source == "test producer" && state.reason == "explicit health fact", "failure metadata lost");
}

void test_control_failure_case2() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.expect(DeviceState::error);
}

void test_normal_startup_and_complete_critical_recovery() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.expect(DeviceState::error);
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    fixture.expect(DeviceState::recovering);
    fixture.send(RuntimeEventType::recovery_success, "control_service");
    fixture.expect(DeviceState::running);
    const std::array<DeviceState, 5> expected{{DeviceState::ready, DeviceState::running,
        DeviceState::error, DeviceState::recovering, DeviceState::running}};
    require(fixture.changes.size() == expected.size(), "boot/recovery has extra or missing notifications");
    DeviceState previous = DeviceState::booting;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(fixture.changes[i].current == expected[i] && fixture.changes[i].previous == previous,
                "boot/recovery notification path changed");
        previous = expected[i];
    }
}

void test_heartbeat_timeout_and_successful_recovery() {
    for (const auto* name : {"control_service", "vision_service", "ota_service"}) {
        Fixture fixture;
        fixture.running();
        fixture.changes.clear();
        fixture.send(RuntimeEventType::heartbeat_timeout, name);
        fixture.expect(DeviceState::error);
        fixture.send(RuntimeEventType::recovery_start, name);
        fixture.expect(DeviceState::recovering);
        fixture.send(RuntimeEventType::recovery_success, name);
        fixture.expect(DeviceState::running);
        require(fixture.changes.size() == 3 && fixture.changes[0].previous == DeviceState::running &&
                fixture.changes[1].previous == DeviceState::error &&
                fixture.changes[2].previous == DeviceState::recovering,
                "heartbeat recovery path or notification count changed");
        auto duplicate = fact(RuntimeEventType::recovery_success, name);
        duplicate.generation = fixture.generations[name];
        require(!fixture.aggregation.handle(duplicate) && fixture.changes.size() == 3,
                "unbound duplicate recovery success changed the snapshot");
    }
}

void test_multiple_failure_priority_case3() {
    const std::array<std::array<const char*, 3>, 6> orders{{
        {{"control_service", "vision_service", "ota_service"}},
        {{"control_service", "ota_service", "vision_service"}},
        {{"vision_service", "control_service", "ota_service"}},
        {{"vision_service", "ota_service", "control_service"}},
        {{"ota_service", "control_service", "vision_service"}},
        {{"ota_service", "vision_service", "control_service"}},
    }};
    for (const auto& order : orders) {
        Fixture fixture;
        fixture.running();
        for (const auto* name : order) fixture.send(RuntimeEventType::service_failed, name);
        fixture.expect(DeviceState::error);
        fixture.send(RuntimeEventType::recovery_success, "ota_service");
        fixture.expect(DeviceState::error);
        fixture.send(RuntimeEventType::recovery_success, "control_service");
        fixture.expect(DeviceState::warning);
        fixture.send(RuntimeEventType::recovery_success, "vision_service");
        fixture.expect(DeviceState::running);
    }
}

void test_partial_recovery_case4() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "vision_service");
    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    fixture.expect(DeviceState::recovering);
    fixture.changes.clear();
    fixture.send(RuntimeEventType::recovery_success, "control_service");
    fixture.expect(DeviceState::warning);
    require(fixture.changes.size() == 1 && fixture.changes.front().previous == DeviceState::recovering,
            "partial recovery transiently forced RUNNING");
    fixture.send(RuntimeEventType::recovery_success, "vision_service");
    fixture.expect(DeviceState::running);
}

void test_startup_and_required_vision() {
    AggregationOptions options;
    options.vision_required = true;
    Fixture fixture(options);
    fixture.send(RuntimeEventType::service_started, "control_service");
    fixture.expect(DeviceState::ready);
    fixture.send(RuntimeEventType::service_failed, "vision_service");
    fixture.expect(DeviceState::error);
    fixture.send(RuntimeEventType::service_started, "vision_service");
    fixture.expect(DeviceState::running);
    fixture.send(RuntimeEventType::service_failed, "vision_service");
    fixture.expect(DeviceState::error);
}

void test_static_criticality_override() {
    AggregationOptions options;
    options.policies["ota_service"] = {ServiceCriticality::high, true};
    Fixture fixture(options);
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "ota_service");
    fixture.expect(DeviceState::error);
}

void test_heartbeat_stopped_and_recovery_failure() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::service_stopped, "ota_service");
    fixture.expect(DeviceState::warning);
    fixture.send(RuntimeEventType::service_started, "ota_service");
    fixture.expect(DeviceState::running);
    fixture.send(RuntimeEventType::heartbeat_timeout, "vision_service");
    fixture.expect(DeviceState::error);
    fixture.send(RuntimeEventType::service_stopped, "vision_service");
    fixture.expect(DeviceState::error);
    fixture.send(RuntimeEventType::recovery_start, "vision_service");
    fixture.expect(DeviceState::recovering);
    fixture.send(RuntimeEventType::recovery_failed, "vision_service");
    fixture.expect(DeviceState::offline);
    fixture.send(RuntimeEventType::service_started, "vision_service");
    fixture.expect(DeviceState::offline);
}

void test_optional_recovery_failure_and_resource_sources() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "ota_service");
    fixture.send(RuntimeEventType::recovery_start, "ota_service");
    fixture.send(RuntimeEventType::recovery_failed, "ota_service");
    fixture.expect(DeviceState::warning);
    auto cpu = fact(RuntimeEventType::resource_warning);
    cpu.source = "cpu_monitor";
    auto memory = cpu;
    memory.source = "memory_monitor";
    require(fixture.aggregation.handle(cpu) && fixture.aggregation.handle(memory), "resource warning rejected");
    fixture.send(RuntimeEventType::service_started, "ota_service");
    fixture.expect(DeviceState::warning);
    cpu.active = false;
    require(fixture.aggregation.handle(cpu), "resource clear rejected");
    fixture.expect(DeviceState::warning);
    memory.active = false;
    require(fixture.aggregation.handle(memory), "resource clear rejected");
    fixture.expect(DeviceState::running);
}

void test_recovery_rechecks_other_critical_failures() {
    AggregationOptions options;
    options.vision_required = true;
    Fixture fixture(options);
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    fixture.send(RuntimeEventType::service_failed, "vision_service");
    fixture.expect(DeviceState::error);
    fixture.send(RuntimeEventType::recovery_start, "vision_service");
    fixture.expect(DeviceState::recovering);
    fixture.send(RuntimeEventType::recovery_success, "control_service");
    fixture.expect(DeviceState::recovering);
    fixture.send(RuntimeEventType::recovery_success, "vision_service");
    fixture.expect(DeviceState::running);
}

void test_device_wide_recovery_does_not_clear_service_faults() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.send(RuntimeEventType::recovery_start, {});
    fixture.expect(DeviceState::recovering);
    fixture.send(RuntimeEventType::recovery_success, {});
    fixture.expect(DeviceState::error);
    fixture.send(RuntimeEventType::recovery_failed, {});
    fixture.expect(DeviceState::offline);
}

void test_resource_critical_downgrade_and_clear() {
    Fixture fixture;
    fixture.running();
    auto memory = fact(RuntimeEventType::resource_warning);
    memory.source = "memory_monitor";
    memory.severity = ResourceSeverity::critical;
    require(fixture.aggregation.handle(memory), "critical resource fact rejected");
    fixture.expect(DeviceState::error);
    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    fixture.expect(DeviceState::error); // A restart cannot recover memory pressure.
    fixture.send(RuntimeEventType::recovery_success, "control_service");
    fixture.expect(DeviceState::error);
    auto cpu = fact(RuntimeEventType::resource_warning);
    cpu.source = "cpu_monitor";
    require(fixture.aggregation.handle(cpu), "CPU warning rejected");
    memory.severity = ResourceSeverity::warning;
    require(fixture.aggregation.handle(memory), "memory downgrade rejected");
    fixture.expect(DeviceState::warning);
    memory.active = false;
    require(fixture.aggregation.handle(memory), "memory clear rejected");
    fixture.expect(DeviceState::warning); // CPU source remains active.
    cpu.active = false;
    require(fixture.aggregation.handle(cpu), "CPU clear rejected");
    fixture.expect(DeviceState::running);
    memory.severity = static_cast<ResourceSeverity>(999);
    require(!fixture.aggregation.handle(memory), "unknown resource severity accepted");
    fixture.expect(DeviceState::running);

    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    fixture.expect(DeviceState::recovering);
    memory.active = true;
    memory.severity = ResourceSeverity::critical;
    require(fixture.aggregation.handle(memory), "new critical resource during recovery rejected");
    fixture.expect(DeviceState::error);
    memory.active = false;
    require(fixture.aggregation.handle(memory), "critical resource clear rejected");
    fixture.expect(DeviceState::recovering);
}

void test_stale_recovery_generation_rejected() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    auto old = fact(RuntimeEventType::recovery_success, "control_service");
    old.generation = fixture.generations["control_service"];
    old.recovery_context = fixture.contexts.at("control_service");
    fixture.send(RuntimeEventType::service_failed, "control_service"); // Independent newer fault B.
    const auto before = fixture.states.query();
    const auto notifications = fixture.changes.size();
    for (const auto type : {RuntimeEventType::recovery_success, RuntimeEventType::recovery_start,
                           RuntimeEventType::recovery_failed, RuntimeEventType::service_started}) {
        old.type = type;
        require(!fixture.aggregation.handle(old), "old generation cleared newer fault");
    }
    old.type = RuntimeEventType::recovery_success;
    old.generation.reset();
    require(!fixture.aggregation.handle(old), "untagged recovery accepted");
    old.generation = fixture.generations["control_service"] + 1;
    require(!fixture.aggregation.handle(old), "future recovery token accepted");
    const auto after = fixture.states.query();
    require(after.current == before.current && after.previous == before.previous &&
            after.source == before.source && after.reason == before.reason && after.timestamp == before.timestamp &&
            fixture.changes.size() == notifications, "rejected token mutated snapshot or notified");
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    auto candidate = fact(RuntimeEventType::service_started, "control_service");
    candidate.generation = ++fixture.generations["control_service"];
    auto context = fixture.contexts.at("control_service");
    context.execution_generation = candidate.generation;
    candidate.recovery_context = context;
    require(fixture.aggregation.handle(candidate), "captured candidate rejected");
    fixture.expect(DeviceState::recovering); // Exec alone cannot clear the fault.
    old.generation = candidate.generation;
    old.recovery_context = context;
    old.at = base - 1s;
    auto wrong = old;
    ++wrong.recovery_context->recovery_generation;
    require(!fixture.aggregation.handle(wrong), "wrong episode accepted");
    wrong = old;
    wrong.recovery_context->execution_generation.reset();
    require(!fixture.aggregation.handle(wrong), "missing execution token accepted");
    require(fixture.aggregation.handle(old), "matching captured recovery rejected");
    fixture.expect(DeviceState::running);
    const auto count = fixture.changes.size();
    require(!fixture.aggregation.handle(old) && fixture.changes.size() == count, "duplicate result notified again");
    fixture.send(RuntimeEventType::service_failed, "control_service");
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    fixture.send(RuntimeEventType::service_stopped, "control_service");
    require(!fixture.aggregation.handle(old), "cancelled recovery revived stopped service");
    fixture.expect(DeviceState::error);
}

void test_lifecycle_retry_exhaustion_reaches_device_state() {
    struct FailingProcesses final : ProcessSupervisor {
        int start(const ServiceConfig&) override { throw std::runtime_error("failed exec"); }
        void stop(int) override {}
        void force_stop(int) override {}
        std::vector<ProcessExit> reap() override { return {}; }
    };
    for (const auto* name : {"control_service", "vision_service", "ota_service"}) {
        for (const auto policy : {RestartPolicy::never, RestartPolicy::on_failure}) {
            DeviceStateManager states;
            ServiceAggregation aggregation(states);
            aggregation.add_service(name);
            require(states.handle({DeviceStateEventType::runtime_initialized, "test", "initialized", base}) ==
                    DeviceTransitionResult::transitioned, "initialization failed");
            FailingProcesses processes;
            Monitor monitor([](Event) {});
            std::ostringstream output;
            Logger logger(output);
            unsigned terminal = 0;
            EventDispatcher dispatcher;
            dispatcher.subscribe([&](const RuntimeEvent& event) {
                require(aggregation.handle(event), "lifecycle fact rejected by aggregation");
                if (event.type == RuntimeEventType::recovery_failed) ++terminal;
            });
            std::deque<ServiceStateChange> work;
            ServiceManager services(processes, monitor, logger,
                [&](const ServiceStateChange& change) { work.push_back(change); });
            ServiceConfig config;
            config.service_name = name;
            config.executable = "/fake/service";
            config.restart_policy = policy;
            services.add(config);
            auto at = Clock::now();
            RecoveryManager recovery(std::vector<ServiceConfig>{config}, services, [&] { return at; });
            auto drain = [&] {
                while (!work.empty()) {
                    const auto change = work.front();
                    work.pop_front();
                    if (change.to == ServiceState::failed) {
                        auto event = fact(RuntimeEventType::service_failed, name);
                        event.generation = change.generation;
                        if (change.operation) event.recovery_context = change.operation->context;
                        dispatcher.publish(event);
                        dispatcher.drain();
                        if (change.cause == ServiceChangeCause::recovery_finalization) {
                            aggregation.bind_recovery(name, change.generation, change.operation->context, false, at);
                        } else if (!change.operation) {
                            RecoveryRequest request;
                            request.service_name = name;
                            request.service_generation = change.generation;
                            request.failure_type = *change.failure_type;
                            request.reason = "captured initial failure";
                            recovery.submit(request, at);
                        }
                        // RM tick already consumes its synchronous failed reply.
                    } else if (change.to == ServiceState::recovering) {
                        aggregation.bind_recovery(name, change.generation, change.operation->context, true, at);
                    }
                }
                for (const auto& start : recovery.takeStarts()) {
                    auto event = fact(RuntimeEventType::recovery_start, name);
                    event.generation = start.generation;
                    event.recovery_context = start.context;
                    dispatcher.publish(event);
                }
                dispatcher.drain();
                for (const auto& result : recovery.takeResults()) {
                    auto event = fact(RuntimeEventType::recovery_failed, name);
                    event.generation = result.latest_fault_generation;
                    event.recovery_context = RecoveryContext{result.recovery_generation,
                        result.initial_fault_generation, result.execution_generation, result.origin};
                    aggregation.bind_recovery(name, result.latest_fault_generation, *event.recovery_context, false, at);
                    dispatcher.publish(event);
                }
                dispatcher.drain();
            };
            services.startService(name, at);
            drain();
            for (const auto delay : {2s, 4s, 8s, 16s, 32s}) {
                at += delay;
                services.tick(at);
                recovery.tick(at);
                drain();
            }
            require(services.query(name)->restart_count == (policy == RestartPolicy::never ? 0u : 5u),
                    "retry reservation projection changed");
            recovery.tick(at + 1h);
            drain();
            const bool critical = std::string(name) == "control_service";
            require(states.query().current == (critical ? (policy == RestartPolicy::never ? DeviceState::error
                                                                                         : DeviceState::offline)
                                                        : DeviceState::warning),
                    "terminal retry result applied the wrong criticality");
            require(terminal == (policy == RestartPolicy::never ? 0u : 1u), "terminal retry fact duplicated or missing");
            require(services.query(name)->state == ServiceState::failed, "Phase2 final FAILED state changed");
        }
    }
}

void test_bound_attempt_failure_keeps_episode_and_rejects_old_candidate() {
    Fixture fixture;
    fixture.running();
    fixture.send(RuntimeEventType::heartbeat_timeout, "control_service");
    fixture.send(RuntimeEventType::recovery_start, "control_service");
    auto context = fixture.contexts.at("control_service");
    RuntimeEvent candidate = fact(RuntimeEventType::service_started, "control_service");
    candidate.generation = ++fixture.generations["control_service"];
    context.execution_generation = candidate.generation;
    candidate.recovery_context = context;
    require(fixture.aggregation.handle(candidate), "bound candidate rejected");
    fixture.expect(DeviceState::recovering);
    auto old_success = candidate;
    old_success.type = RuntimeEventType::recovery_success;
    auto failure = fact(RuntimeEventType::service_failed, "control_service");
    failure.generation = ++fixture.generations["control_service"];
    failure.recovery_context = context;
    require(fixture.aggregation.handle(failure), "bound attempt failure rejected");
    context.execution_generation.reset();
    require(fixture.aggregation.bind_recovery("control_service", *failure.generation, context, true, base),
            "retry lost its admitted episode");
    fixture.expect(DeviceState::recovering);
    const auto notifications = fixture.changes.size();
    require(!fixture.aggregation.handle(old_success) && fixture.changes.size() == notifications,
            "old candidate cleared the newer attempt failure");
    candidate.generation = ++fixture.generations["control_service"];
    context.execution_generation = candidate.generation;
    candidate.recovery_context = context;
    require(fixture.aggregation.handle(candidate), "next candidate rejected");
    fixture.expect(DeviceState::recovering);
    candidate.type = RuntimeEventType::recovery_success;
    require(fixture.aggregation.handle(candidate), "correct second execution rejected");
    fixture.expect(DeviceState::running);
}

void test_invalid_duplicate_events_and_checked_targets() {
    Fixture fixture;
    fixture.running();
    auto invalid = fact(RuntimeEventType::service_failed, "unknown");
    require(!fixture.aggregation.handle(invalid), "unknown service accepted");
    invalid.service_name = "control_service";
    invalid.reason.clear();
    require(!fixture.aggregation.handle(invalid), "missing reason accepted");
    invalid.reason = "invalid type";
    invalid.type = static_cast<RuntimeEventType>(999);
    require(!fixture.aggregation.handle(invalid), "unknown event type accepted");
    fixture.expect(DeviceState::running);
    fixture.send(RuntimeEventType::service_failed, "vision_service");
    const auto count = fixture.changes.size();
    auto duplicate = fact(RuntimeEventType::service_failed, "vision_service");
    duplicate.generation = fixture.generations["vision_service"];
    require(fixture.aggregation.handle(duplicate), "duplicate generation fact rejected");
    require(fixture.changes.size() == count, "duplicate fact produced duplicate transition");
    DeviceStateEvent bypass{DeviceStateEventType::warning, "test", "invalid target", base};
    bypass.health_target = DeviceState::offline;
    require(fixture.states.handle(bypass) == DeviceTransitionResult::invalid_transition, "unchecked target accepted");
    fixture.expect(DeviceState::warning);
}

void test_dispatcher_fifo_subscribers_and_nested_publication() {
    EventDispatcher dispatcher;
    std::vector<std::string> delivered;
    dispatcher.subscribe([&](const RuntimeEvent& event) {
        delivered.push_back(std::string("first:") + event_name(event.type));
        if (event.type == RuntimeEventType::service_failed) {
            dispatcher.publish(fact(RuntimeEventType::recovery_start, "vision_service"));
            dispatcher.drain(); // Nested drain cannot overtake the second subscriber.
        }
    });
    dispatcher.subscribe([&](const RuntimeEvent& event) {
        delivered.push_back(std::string("second:") + event_name(event.type));
    });
    dispatcher.publish(fact(RuntimeEventType::service_failed, "vision_service"));
    dispatcher.publish(fact(RuntimeEventType::resource_warning));
    dispatcher.drain();
    require(delivered == std::vector<std::string>{
        "first:SERVICE_FAILED", "second:SERVICE_FAILED", "first:RESOURCE_WARNING", "second:RESOURCE_WARNING",
        "first:RECOVERY_START", "second:RECOVERY_START"}, "dispatcher event/subscriber order changed");
}

void test_runtime_fifo_and_start_failure_adapter() {
    const auto path = std::filesystem::temp_directory_path() /
        ("phase3_aggregation_" + std::to_string(Clock::now().time_since_epoch().count()) + ".json");
    {
        std::ofstream config(path);
        config << R"({"service_name":"control_service","executable":"/missing/phase3/service","autostart":false})";
        require(static_cast<bool>(config), "cannot create test config");
    }
    RuntimeManager manager(path.string());
    std::filesystem::remove(path);
    manager.post(Event{EventType::start, "control_service", Clock::now()});
    manager.post(fact(RuntimeEventType::resource_warning));
    manager.post(Event{EventType::shutdown, {}, Clock::now()});
    manager.run();
    require(manager.queryDeviceState().current == DeviceState::error, "startup failure did not reach device state");
    require(manager.queryDeviceState().source == "service_manager", "runtime health routing lost priority");
}

} // namespace

int main() {
    try {
        test_vision_failure_case1();
        test_control_failure_case2();
        test_normal_startup_and_complete_critical_recovery();
        test_heartbeat_timeout_and_successful_recovery();
        test_multiple_failure_priority_case3();
        test_partial_recovery_case4();
        test_startup_and_required_vision();
        test_static_criticality_override();
        test_heartbeat_stopped_and_recovery_failure();
        test_optional_recovery_failure_and_resource_sources();
        test_recovery_rechecks_other_critical_failures();
        test_device_wide_recovery_does_not_clear_service_faults();
        test_resource_critical_downgrade_and_clear();
        test_stale_recovery_generation_rejected();
        test_lifecycle_retry_exhaustion_reaches_device_state();
        test_bound_attempt_failure_keeps_episode_and_rejects_old_candidate();
        test_invalid_duplicate_events_and_checked_targets();
        test_dispatcher_fifo_subscribers_and_nested_publication();
        test_runtime_fifo_and_start_failure_adapter();
        std::cout << "event aggregation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "event aggregation test failed: " << error.what() << '\n';
        return 1;
    }
}
