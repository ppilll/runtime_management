#include "runtime/runtime_manager.hpp"
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace runtime;
using namespace std::chrono_literals;

namespace runtime {
// Restricted fake transport access. No production public trusted flag or post bypass.
class ComponentHealthTestAccess {
public:
    static std::function<bool(ComponentIngress)> sink(RuntimeManager& manager) { return manager.component_sink(); }
    static std::size_t pending(RuntimeManager& manager) {
        std::lock_guard<std::mutex> lock(manager.component_mailbox_->mutex);
        return manager.component_mailbox_->operations.size();
    }
    static void stop(RuntimeManager& manager) { manager.stop_component_ingress(); }
    static void dispatch_next(RuntimeManager& manager) {
        Event event{};
        if (!manager.queue_.pop_for(event, std::chrono::milliseconds{0}))
            throw std::logic_error("fake transport expected queued event");
        manager.handle_event(event);
    }
    static void final_session(ComponentAdmission& admission) {
        admission.next_session_ = std::numeric_limits<std::uint64_t>::max();
    }
};
}

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
const std::string epoch = "00112233445566778899aabbccddeeff";
const Clock::time_point origin{10s};

ComponentPolicyEntry policy(ServiceCriticality criticality = ServiceCriticality::high, bool required = true) {
    return {"mcu_link", "worker", {criticality, required}};
}

ComponentSample sample(ComponentHealthStatus status, std::uint64_t revision = 1,
                       ComponentSampleKind kind = ComponentSampleKind::current) {
    ComponentReason reason = ComponentReason::initial;
    if (status == ComponentHealthStatus::connecting) reason = ComponentReason::negotiating;
    else if (status == ComponentHealthStatus::healthy) reason = ComponentReason::ready;
    else if (status == ComponentHealthStatus::degraded) reason = ComponentReason::response_timeout;
    else if (status == ComponentHealthStatus::unhealthy) reason = ComponentReason::link_lost;
    return {"mcu_link", status, reason, component_reason_name(reason), revision, 1, std::uint32_t{0}, kind};
}

struct AdmissionFixture {
    ServiceStatus status;
    std::vector<ComponentHealthFact> facts;
    ComponentAdmission admission;
    std::shared_ptr<ComponentPeer> peer;
    ComponentReply binding;
    Clock::time_point now = origin;
    std::uint64_t connection = 0;

    AdmissionFixture()
        : admission(policy(), epoch,
            [this](const std::string& name) -> std::optional<ServiceStatus> {
                return name == "worker" ? std::optional<ServiceStatus>{status} : std::nullopt;
            }, [this](const ComponentHealthFact& fact) { facts.push_back(fact); }) {
        status.service_name = "worker";
        status.state = ServiceState::running;
        status.pid = 42;
        status.start_time = origin;
        status.generation = 99; // Failure/lifecycle token must never be used as launch identity.
        status.launched_generation = 3;
    }
    std::shared_ptr<ComponentPeer> connection_peer(int pid = 42) {
        auto result = std::make_shared<ComponentPeer>();
        result->pid = pid;
        result->uid = 1000;
        result->gid = 1000;
        result->connection_token = ++connection;
        result->accepted_at = now;
        return result;
    }
    ComponentReply bind() {
        peer = connection_peer();
        binding = admission.apply(ComponentIngress{ComponentIngressKind::bind, peer, "worker", {}, now, 1, {}}, now);
        return binding;
    }
    ComponentIngress report(std::uint64_t seq, ComponentSample value) {
        return {ComponentIngressKind::report, peer, {},
            {epoch, binding.identity.session_id, seq, std::move(value)}, now, 2, {}};
    }
    ComponentReply send(std::uint64_t seq, ComponentSample value) {
        return admission.apply(report(seq, std::move(value)), now);
    }
};

struct AggregateFixture {
    DeviceStateManager states;
    ServiceAggregation aggregation;
    explicit AggregateFixture(ServiceCriticality criticality = ServiceCriticality::high, bool required = true)
        : aggregation(states, AggregationOptions{false, {{"worker", {ServiceCriticality::high, true}}}, {}}) {
        aggregation.add_service("worker");
        aggregation.add_component(policy(criticality, required));
        require(states.handle({DeviceStateEventType::runtime_initialized, "test", "initialize", origin}) ==
            DeviceTransitionResult::transitioned, "initialize DSM");
        RuntimeEvent event{RuntimeEventType::service_started, "worker", "service_manager", "running", origin};
        event.generation = 1;
        require(aggregation.handle(event), "start service");
    }
    void component(ComponentHealthStatus status, std::uint64_t generation = 3) {
        RuntimeEvent event{RuntimeEventType::component_health_changed, {}, "component:mcu_link", "normalized", origin};
        event.component_health = ComponentHealthFact{sample(status), {"worker", 42, generation, epoch, 1}, origin,
            status != ComponentHealthStatus::unknown};
        require(aggregation.handle(event), "component fact accepted");
    }
    void resource(ResourceSeverity severity, bool active = true) {
        require(aggregation.handle({RuntimeEventType::resource_warning, {}, "cpu_monitor", "pressure", origin,
            active, severity}), "resource fact accepted");
    }
    void expect(DeviceState target) { require(states.query().current == target, "aggregate target mismatch"); }
    void recovery() {
        RuntimeEvent fail{RuntimeEventType::service_failed, "worker", "service_manager", "fault", origin};
        fail.generation = 2;
        require(aggregation.handle(fail), "process fault");
        RuntimeEvent start{RuntimeEventType::recovery_start, "worker", "recovery_manager", "recover", origin};
        start.generation = 2;
        start.recovery_context = RecoveryContext{1, 2, {}, RecoveryOrigin::automatic_failure};
        require(aggregation.handle(start), "process recovery");
    }
};

void test_normalized_status_reason_and_utf8_bounds() {
    for (const auto status : {ComponentHealthStatus::unknown, ComponentHealthStatus::connecting,
            ComponentHealthStatus::healthy, ComponentHealthStatus::degraded, ComponentHealthStatus::unhealthy})
        require(valid_component_sample(sample(status)), "five mapped statuses");
    auto value = sample(ComponentHealthStatus::healthy);
    value.reason_code = ComponentReason::link_lost;
    require(!valid_component_sample(value), "reject reason/status mismatch");
    value = sample(ComponentHealthStatus::unknown);
    value.reason_code = ComponentReason::report_expired;
    require(!valid_component_sample(value), "reject server-derived reason");
    value = sample(ComponentHealthStatus::healthy, 1, ComponentSampleKind::issue);
    require(!valid_component_sample(value), "issue cannot be healthy");
    value = sample(ComponentHealthStatus::healthy);
    value.reason = std::string(256, 'a');
    require(valid_component_sample(value), "256-byte reason");
    value.reason += 'a';
    require(!valid_component_sample(value), "257-byte reason");
    value.reason = "\xF0\x9F\x98\x80";
    require(valid_component_sample(value), "valid UTF-8");
    for (const auto invalid : {"\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82"}) {
        value.reason = invalid;
        require(!valid_component_sample(value), "reject overlong/surrogate/out-of-range/truncated UTF-8");
    }
    value = sample(ComponentHealthStatus::unknown, 0);
    require(valid_component_sample(value), "initial revision zero");
    value.status = ComponentHealthStatus::healthy;
    value.reason_code = ComponentReason::ready;
    require(!valid_component_sample(value), "zero revision only initial");
}

void test_bind_exact_pid_launch_start_and_early_retry() {
    AdmissionFixture f;
    auto peer = f.connection_peer(43); // Same UID, other managed or unmanaged PID.
    auto request = ComponentIngress{ComponentIngressKind::bind, peer, "worker", {}, f.now, 7, {}};
    require(f.admission.apply(request, f.now).error == 1004 && peer->revoked, "wrong exact PID");
    request.peer = f.connection_peer();
    request.peer->accepted_at = origin - 1ms;
    require(f.admission.apply(request, f.now).error == 1004, "old connection cannot be adopted");
    request.peer = f.connection_peer();
    f.status.state = ServiceState::starting;
    require(f.admission.apply(request, f.now).error == 1004, "early bind RETRY_BIND");
    f.status.state = ServiceState::running;
    require(f.admission.apply(request, f.now).error == 1004, "must close/reconnect after early bind");
    require(f.bind().error == 0 && f.binding.identity.launched_generation == 3, "server launch identity, not generation 99");
    require(f.admission.snapshot(f.now).fact.sample.status == ComponentHealthStatus::unknown, "bind cannot establish health");
    const auto session = f.binding.identity.session_id;
    request.peer = f.peer;
    request.service_name = "worker";
    require(f.admission.apply(request, f.now).identity.session_id == session, "same connection bind is idempotent");
}

void test_sequence_duplicate_no_renewal_conflict_and_lower_bound() {
    AdmissionFixture f;
    require(f.bind().error == 0, "bind");
    auto healthy = sample(ComponentHealthStatus::healthy, 10);
    healthy.link_epoch = 2;
    require(f.send(1, healthy).result == "APPLIED", "writer applied");
    const auto count = f.facts.size();
    f.now += 1s;
    require(f.send(1, healthy).error == 0, "same seq/content duplicate ACK");
    require(f.facts.size() == count && f.admission.snapshot(f.now).fact.received_at == origin, "duplicate no renewal/event");
    require(f.send(1, sample(ComponentHealthStatus::unhealthy, 10)).error == 1002, "same seq conflicting content");
    require(f.send(0, healthy).error == 1002, "zero/low sequence");
    require(f.send(2, sample(ComponentHealthStatus::healthy, 9)).error == 1002, "revision rollback");
    require(f.send(2, sample(ComponentHealthStatus::unhealthy, 10)).error == 1002, "high seq cannot change same revision");
    require(f.send(2, healthy).error == 0 && f.facts.size() == count, "same revision stable renewal no event");
    require(f.admission.snapshot(f.now).fact.received_at == f.now, "stable high sequence renews");
    auto rollback_epoch = sample(ComponentHealthStatus::healthy, 11);
    rollback_epoch.link_epoch = 1;
    require(f.send(3, rollback_epoch).error == 1002, "link epoch rollback");
}

void test_issue_current_same_revision_and_new_issue_priority() {
    AdmissionFixture f;
    f.bind();
    auto issue = sample(ComponentHealthStatus::unhealthy, 12, ComponentSampleKind::issue);
    require(f.send(1, issue).error == 0, "pending transient issue applied");
    auto changed_boot = sample(ComponentHealthStatus::healthy, 12);
    changed_boot.peer_boot_id = 1;
    require(f.send(2, changed_boot).error == 1002, "same coherent revision cannot change boot evidence");
    require(f.send(2, sample(ComponentHealthStatus::healthy, 12)).error == 0, "one issue-to-current same revision exception");
    require(f.send(3, issue).error == 1002, "current closes old issue exception");
    require(f.send(3, sample(ComponentHealthStatus::unhealthy, 14, ComponentSampleKind::issue)).error == 0, "new issue takes priority");
    require(f.send(4, sample(ComponentHealthStatus::healthy, 13)).error == 1002, "cached healthy cannot skip new issue");
    require(f.send(4, sample(ComponentHealthStatus::healthy, 14)).error == 0, "fresh latest after new issue");
}

void test_ack_lost_reconnect_preserves_owner_and_confirmation() {
    AdmissionFixture f;
    f.bind();
    auto issue = sample(ComponentHealthStatus::unhealthy, 12, ComponentSampleKind::issue);
    require(f.send(1, issue).error == 0, "issue applied but transport ACK lost");
    const auto old_session = f.binding.identity.session_id;
    const auto old_peer = f.peer;
    old_peer->revoked = true;
    f.admission.checkpoint(f.now);
    require(f.admission.snapshot(f.now).fact.sample.status == ComponentHealthStatus::unknown, "disconnect derives unknown");
    require(f.bind().error == 0 && f.binding.identity.session_id > old_session, "new session on same launch");
    const auto before = f.facts.size();
    require(f.send(1, issue).error == 0 && f.facts.size() == before, "reconnect identical issue ACK without duplicate event");
    require(f.send(2, sample(ComponentHealthStatus::healthy, 12)).error == 0, "same revision current after re-confirmation");
    old_peer->revoked = true;
    f.admission.checkpoint(f.now);
    require(f.admission.snapshot(f.now).fact.sample.status == ComponentHealthStatus::healthy, "old disconnect cannot revoke new session");
    f.peer->revoked = true;
    f.admission.checkpoint(f.now);
    f.bind();
    require(f.send(1, issue).error == 1002, "current already applied: old issue cannot roll back after reconnect");
    require(f.send(1, sample(ComponentHealthStatus::healthy, 11)).error == 1002, "derived UNKNOWN retained lower bound");
}

void test_lease_exact_boundary_query_and_stale_queue() {
    AdmissionFixture f;
    f.bind();
    require(f.send(1, sample(ComponentHealthStatus::healthy, 9)).error == 0, "healthy");
    const auto late = origin + component_observation_lease;
    require(f.admission.snapshot(late).fact.observation_fresh, "query cannot advance expiry");
    f.admission.checkpoint(late - 1ms);
    require(f.admission.snapshot(late).fact.observation_fresh, "expiry minus one fresh");
    f.admission.checkpoint(late);
    auto view = f.admission.snapshot(late);
    require(!view.fact.observation_fresh && view.fact.sample.reason_code == ComponentReason::report_expired, "equality expires");
    require(view.fact.sample.projection_revision == 9 && view.report_age == 3500ms, "UNKNOWN keeps metadata and age");
    auto queued = f.report(2, sample(ComponentHealthStatus::healthy, 9));
    require(f.admission.apply(queued, late).error == 1003, "stale queued report cannot restore health");
    f.now = late;
    require(f.send(2, sample(ComponentHealthStatus::healthy, 8)).error == 1002, "expiry retained owner bound");
    require(f.send(2, sample(ComponentHealthStatus::healthy, 9)).error == 0, "new observed stable sample can restore");
    require(f.admission.snapshot(origin).report_age == 0ms, "query age clamps backwards time");
}

void test_launch_change_same_pid_and_shutdown_gate() {
    AdmissionFixture f;
    f.bind();
    require(f.send(1, sample(ComponentHealthStatus::unhealthy, 19)).error == 0, "old launch fault");
    auto stale = f.report(2, sample(ComponentHealthStatus::healthy, 20));
    f.status.launched_generation = 4; // Deliberately reuse numerical PID.
    f.status.start_time = f.now;
    require(f.admission.apply(stale, f.now).error == 1004, "writer rechecks launch even same PID");
    f.bind();
    require(f.send(1, sample(ComponentHealthStatus::healthy, 1)).error == 0, "new launch resets owner bounds");
    const auto before = f.facts.size();
    f.admission.stop();
    require(f.send(2, sample(ComponentHealthStatus::unhealthy, 2)).error == 1003, "shutdown rejects late report");
    f.admission.checkpoint(f.now + 10s);
    require(f.facts.size() == before, "no state commits after shutdown gate");
}

void test_stop_committed_rejects_health_and_bound_heartbeat() {
    AdmissionFixture f;
    f.bind();
    require(f.send(1, sample(ComponentHealthStatus::healthy)).error == 0, "healthy");
    ComponentIngress heartbeat{ComponentIngressKind::heartbeat, f.peer, "worker",
        {epoch, f.binding.identity.session_id, 0, {}}, f.now, 0, {}};
    require(f.admission.apply(heartbeat, f.now).error == 0, "bound process heartbeat");
    f.now += 1s;
    heartbeat.received_at = f.now;
    require(f.admission.apply(heartbeat, f.now).error == 0, "heartbeat independent of report stream");
    require(f.admission.snapshot(f.now).fact.received_at == origin, "type4 cannot renew component lease");
    auto queued = f.report(2, sample(ComponentHealthStatus::healthy, 2));
    f.status.state = ServiceState::stopping;
    require(f.admission.apply(queued, f.now).error == 1004, "STOP committed first rejects later health");
    require(f.admission.apply(heartbeat, f.now).error == 1004, "STOP committed rejects heartbeat");
    require(!f.admission.snapshot(f.now).fact.observation_fresh, "STOP invalidates observation");
}

void test_epoch_and_replaced_session_identity() {
    AdmissionFixture f;
    f.bind();
    auto old_report = f.report(1, sample(ComponentHealthStatus::healthy));
    const auto old_id = f.binding.identity.session_id;
    f.bind();
    require(f.binding.identity.session_id != old_id, "session never reused");
    require(f.admission.apply(old_report, f.now).error == 1004, "old connection/session rejected");
    auto forged = f.report(1, sample(ComponentHealthStatus::healthy));
    forged.report.runtime_epoch = std::string(32, '0');
    require(f.admission.apply(forged, f.now).error == 1004, "different Runtime epoch rejected");
}

void test_session_overflow_never_wraps_and_link_loss_has_no_process_action() {
    AdmissionFixture f;
    ComponentHealthTestAccess::final_session(f.admission);
    require(f.bind().identity.session_id == std::numeric_limits<std::uint64_t>::max(), "last uint64 session is legal");
    require(f.send(1, sample(ComponentHealthStatus::unhealthy)).error == 0, "link loss admitted");
    require(f.status.pid == 42 && f.status.state == ServiceState::running && f.status.generation == 99 &&
        f.status.launched_generation == 3 && f.status.restart_count == 0, "normalized link fact never mutates process/budget");
    bool rejected = false;
    try { f.bind(); } catch (const std::overflow_error&) { rejected = true; }
    require(rejected, "session exhaustion is fatal management error, never wraps");
}

void test_service_manager_bound_heartbeat_instance_gate() {
    class FakeProcesses final : public ProcessSupervisor {
    public:
        int start(const ServiceConfig&) override { return 42; }
        void stop(int) override {}
        void force_stop(int) override {}
        std::vector<ProcessExit> reap() override { return {}; }
    } processes;
    std::ostringstream logs;
    Logger logger(logs);
    Monitor monitor([](Event) {});
    ServiceManager manager(processes, monitor, logger);
    ServiceConfig config;
    config.service_name = "worker";
    config.executable = "/fixture/foreground";
    manager.add(config);
    manager.startService("worker", origin);
    const auto status = *manager.query("worker");
    Event heartbeat{EventType::heartbeat, "worker", *status.start_time + 1s, 42};
    heartbeat.instance_generation = status.launched_generation + 1;
    manager.handle(heartbeat);
    require(!manager.query("worker")->heartbeat_time, "bound heartbeat wrong launch rejected");
    heartbeat.instance_generation = status.launched_generation;
    manager.handle(heartbeat);
    require(manager.query("worker")->heartbeat_time == heartbeat.at, "current bound heartbeat accepted");
    heartbeat.at += 1s;
    heartbeat.instance_generation.reset();
    manager.handle(heartbeat);
    require(manager.query("worker")->heartbeat_time == heartbeat.at, "legacy internal heartbeat semantics preserved");
}

void test_required_readiness_high_loss_and_local_latch() {
    AggregateFixture f;
    f.expect(DeviceState::ready);
    f.component(ComponentHealthStatus::connecting);
    f.expect(DeviceState::ready);
    f.component(ComponentHealthStatus::healthy);
    f.expect(DeviceState::running);
    f.component(ComponentHealthStatus::unhealthy);
    f.expect(DeviceState::error);
    f.component(ComponentHealthStatus::connecting);
    f.expect(DeviceState::error);
    f.component(ComponentHealthStatus::unknown);
    f.expect(DeviceState::error);
    f.component(ComponentHealthStatus::healthy);
    f.expect(DeviceState::running);
    f.component(ComponentHealthStatus::unknown);
    f.expect(DeviceState::error); // Report observation loss after ever HEALTHY.
    f.component(ComponentHealthStatus::unknown, 4);
    f.expect(DeviceState::ready); // New launch does not inherit ever-healthy/adverse.
}

void test_optional_low_required_low_and_optional_high() {
    AggregateFixture low(ServiceCriticality::low, false);
    low.expect(DeviceState::running);
    low.component(ComponentHealthStatus::unhealthy);
    low.expect(DeviceState::warning);
    low.component(ComponentHealthStatus::healthy);
    low.expect(DeviceState::running);
    AggregateFixture required_low(ServiceCriticality::low, true);
    required_low.expect(DeviceState::ready);
    required_low.component(ComponentHealthStatus::degraded);
    required_low.expect(DeviceState::warning);
    AggregateFixture optional_high(ServiceCriticality::high, false);
    optional_high.expect(DeviceState::running);
    optional_high.component(ComponentHealthStatus::unhealthy);
    optional_high.expect(DeviceState::error);
}

void test_clear_reaggregates_resources_process_recovery_and_terminal() {
    AggregateFixture f;
    f.component(ComponentHealthStatus::healthy);
    f.resource(ResourceSeverity::warning);
    f.component(ComponentHealthStatus::unhealthy);
    f.expect(DeviceState::error);
    f.component(ComponentHealthStatus::healthy);
    f.expect(DeviceState::warning); // Component clear cannot clear CPU source.
    f.resource(ResourceSeverity::warning, false);
    f.recovery();
    f.expect(DeviceState::recovering);
    f.component(ComponentHealthStatus::unhealthy);
    f.expect(DeviceState::error); // Critical component wins over active RM.
    f.component(ComponentHealthStatus::healthy);
    f.expect(DeviceState::recovering); // Link recovery is not RM success.
    f.resource(ResourceSeverity::critical);
    f.component(ComponentHealthStatus::healthy);
    f.expect(DeviceState::error);
    require(f.aggregation.handle({RuntimeEventType::recovery_failed, {}, "recovery_manager", "terminal", origin}), "terminal");
    f.component(ComponentHealthStatus::healthy);
    f.expect(DeviceState::offline);
}

void test_component_clear_preserves_unrecovered_process_and_invalid_registry() {
    AggregateFixture f;
    f.component(ComponentHealthStatus::healthy);
    RuntimeEvent fail{RuntimeEventType::service_failed, "worker", "service_manager", "real process failure", origin};
    fail.generation = 2;
    require(f.aggregation.handle(fail), "process failure accepted");
    f.component(ComponentHealthStatus::unhealthy);
    f.component(ComponentHealthStatus::healthy);
    f.expect(DeviceState::error); // No active RM: link cannot clear process failure.
    DeviceStateManager states;
    ServiceAggregation aggregation(states);
    aggregation.add_service("worker");
    for (auto invalid : {ComponentPolicyEntry{"other", "worker", {ServiceCriticality::high, true}},
            ComponentPolicyEntry{"mcu_link", "missing", {ServiceCriticality::high, true}}}) {
        bool rejected = false;
        try { aggregation.add_component(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "explicit registry cannot infer unknown producer/component");
    }
    aggregation.add_component(policy());
    bool duplicate = false;
    try { aggregation.add_component(policy()); } catch (const std::invalid_argument&) { duplicate = true; }
    require(duplicate, "only one component registration");
}

void test_private_queue_bound_pending_revocation_and_weak_lifetime() {
    std::function<bool(ComponentIngress)> retained;
    std::vector<std::shared_ptr<ComponentPeer>> peers;
    {
        RuntimeConfig config;
        RuntimeManager manager(config, {}, {}, ResourceInputMode::external);
        retained = ComponentHealthTestAccess::sink(manager);
        for (std::uint64_t i = 1; i <= component_ingress_limit; ++i) {
            auto peer = std::make_shared<ComponentPeer>();
            peer->connection_token = i;
            peers.push_back(peer);
            require(retained({ComponentIngressKind::bind, peer, "worker", {}, origin, 1, {}}), "bounded operation accepted");
            if (i == 1) require(!retained({ComponentIngressKind::bind, peer, "worker", {}, origin, 2, {}}),
                "one pending per connection even before full");
        }
        require(ComponentHealthTestAccess::pending(manager) == 32, "32-operation limit");
        require(!retained({ComponentIngressKind::bind, peers.front(), "worker", {}, origin, 2, {}}), "one pending per connection");
        auto overflow = std::make_shared<ComponentPeer>();
        overflow->connection_token = 33;
        require(!retained({ComponentIngressKind::bind, overflow, "worker", {}, origin, 1, {}}), "queue full cannot ACK");
        require(!overflow->pending, "full rejection must not strand pending flag");
        peers.front()->revoked = true; // Disconnect tombstone is independent of full queue.
        require(peers.front()->revoked, "immediate revoke without queue slot");
        ComponentHealthTestAccess::stop(manager);
        require(ComponentHealthTestAccess::pending(manager) == 0 && !peers.back()->pending, "shutdown clears pending queue");
        require(!retained({ComponentIngressKind::bind, overflow, "worker", {}, origin, 1, {}}), "sink shutdown gate");
    }
    require(!retained({ComponentIngressKind::bind, std::make_shared<ComponentPeer>(), "worker", {}, origin, 1, {}}),
        "retained weak sink safe after Runtime destruction");
}

void test_public_component_facts_and_name_heartbeat_cannot_bypass() {
    RuntimeConfig config;
    ServiceConfig service;
    service.service_name = "worker";
    service.executable = "/bin/true";
    service.autostart = true;
    config.services.push_back(service);
    RuntimeManager manager(config, AggregationOptions{false, {}, {policy()}}, {}, ResourceInputMode::external);
    RuntimeEvent fake{RuntimeEventType::component_health_changed, {}, "component:mcu_link", "forged", origin};
    fake.component_health = ComponentHealthFact{sample(ComponentHealthStatus::healthy), {"worker", 42, 3, epoch, 1}, origin, true};
    manager.post(fake); // Public ingress merely queues; writer accepts resource facts only.
    manager.post(Event{EventType::heartbeat, "worker", origin, 42});
    ComponentHealthTestAccess::dispatch_next(manager);
    ComponentHealthTestAccess::dispatch_next(manager);
    require(manager.queryComponentHealth(origin).front().fact.sample.status == ComponentHealthStatus::unknown,
        "public posts cannot commit component state");
    require(!manager.query("worker")->heartbeat_time, "public heartbeat cannot pre-confirm producer");
    bool rejected = false;
    try { manager.post(Event{EventType::component_ingress_wake, {}, origin}); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "public wake injection rejected");
}
}

int main() {
    try {
        test_normalized_status_reason_and_utf8_bounds();
        test_bind_exact_pid_launch_start_and_early_retry();
        test_sequence_duplicate_no_renewal_conflict_and_lower_bound();
        test_issue_current_same_revision_and_new_issue_priority();
        test_ack_lost_reconnect_preserves_owner_and_confirmation();
        test_lease_exact_boundary_query_and_stale_queue();
        test_launch_change_same_pid_and_shutdown_gate();
        test_stop_committed_rejects_health_and_bound_heartbeat();
        test_epoch_and_replaced_session_identity();
        test_session_overflow_never_wraps_and_link_loss_has_no_process_action();
        test_service_manager_bound_heartbeat_instance_gate();
        test_required_readiness_high_loss_and_local_latch();
        test_optional_low_required_low_and_optional_high();
        test_clear_reaggregates_resources_process_recovery_and_terminal();
        test_component_clear_preserves_unrecovered_process_and_invalid_registry();
        test_private_queue_bound_pending_revocation_and_weak_lifetime();
        test_public_component_facts_and_name_heartbeat_cannot_bypass();
        std::cout << "component health tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
