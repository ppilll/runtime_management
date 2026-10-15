#include "runtime/runtime_manager.hpp"
#include "runtime/config_manager.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <exception>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/timerfd.h>
#include <sys/random.h>
#include <unistd.h>

namespace runtime {
namespace {
volatile std::sig_atomic_t signal_received = 0;
void on_signal(int) { signal_received = 1; }
}

namespace {
std::string component_runtime_epoch() {
    unsigned char bytes[16];
    std::size_t done = 0;
    while (done < sizeof(bytes)) {
        const auto count = ::getrandom(bytes + done, sizeof(bytes) - done, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(count < 0 ? errno : EIO,
            std::generic_category(), "health runtime entropy unavailable");
        done += static_cast<std::size_t>(count);
    }
    const char hex[] = "0123456789abcdef";
    std::string epoch;
    epoch.reserve(32);
    for (const auto byte : bytes) { epoch += hex[byte >> 4]; epoch += hex[byte & 15]; }
    return epoch;
}

RuntimeConfig legacy_config(const std::string& path) {
    auto config = ConfigManager::load_runtime_file(path);
    config.monitoring.sample_interval_seconds = std::chrono::seconds{2};
    return config;
}

ResourceThresholds native_thresholds(const MonitoringConfig& config) {
    ConfigManager::validate(config);
    return {config.cpu_warning, config.memory_warning, config.memory_critical,
            config.cpu_clear, config.memory_clear, config.memory_critical_clear};
}

AggregationOptions health_aggregation(const RuntimeConfig& config, AggregationOptions options) {
    ConfigManager::validate(config.health, config.services);
    for (const auto& entry : config.health.services) {
        const auto found = options.policies.find(entry.first);
        if (found != options.policies.end() &&
            (found->second.required != entry.second.required || found->second.criticality != entry.second.criticality))
            throw std::invalid_argument("conflicting service health policies");
        options.policies[entry.first] = entry.second;
    }
    if (!config.health.components.empty()) {
        if (!options.components.empty()) {
            const auto& a = options.components.front();
            const auto& b = config.health.components.front();
            if (options.components.size() != 1 || a.component != b.component ||
                a.producer_service != b.producer_service || a.policy.required != b.policy.required ||
                a.policy.criticality != b.policy.criticality)
                throw std::invalid_argument("conflicting component health policies");
        } else options.components = config.health.components;
    }
    ConfigManager::validate(HealthConfig{options.policies, options.components}, config.services);
    const auto vision = config.health.services.find("vision_service");
    if (options.vision_required && vision != config.health.services.end() &&
        (!vision->second.required || vision->second.criticality != ServiceCriticality::high))
        throw std::invalid_argument("conflicting vision_required health policy");
    return options;
}
}

const char* component_status_name(ComponentHealthStatus status) {
    switch (status) {
    case ComponentHealthStatus::unknown: return "UNKNOWN";
    case ComponentHealthStatus::connecting: return "CONNECTING";
    case ComponentHealthStatus::healthy: return "HEALTHY";
    case ComponentHealthStatus::degraded: return "DEGRADED";
    case ComponentHealthStatus::unhealthy: return "UNHEALTHY";
    }
    return "INVALID";
}

const char* component_reason_name(ComponentReason reason) {
    switch (reason) {
    case ComponentReason::initial: return "initial";
    case ComponentReason::opening: return "opening";
    case ComponentReason::negotiating: return "negotiating";
    case ComponentReason::ready: return "ready";
    case ComponentReason::response_timeout: return "response_timeout";
    case ComponentReason::link_lost: return "link_lost";
    case ComponentReason::open_failed: return "open_failed";
    case ComponentReason::hello_rejected: return "hello_rejected";
    case ComponentReason::startup_not_ready: return "startup_not_ready";
    case ComponentReason::peer_reboot: return "peer_reboot";
    case ComponentReason::recovered: return "recovered";
    case ComponentReason::observation_unavailable: return "observation_unavailable";
    case ComponentReason::report_expired: return "report_expired";
    case ComponentReason::owner_stopped: return "owner_stopped";
    }
    return "invalid";
}

bool same_component_sample(const ComponentSample& a, const ComponentSample& b) {
    return a.component == b.component && a.status == b.status && a.reason_code == b.reason_code &&
        a.reason == b.reason && a.projection_revision == b.projection_revision &&
        a.link_epoch == b.link_epoch && a.peer_boot_id == b.peer_boot_id && a.sample_kind == b.sample_kind;
}

namespace {
bool component_utf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        unsigned remaining = 0;
        std::uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; code = first & 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; code = first & 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        if (remaining > value.size() - i) return false;
        while (remaining--) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
}

bool valid_component_sample(const ComponentSample& sample) {
    if (sample.component != "mcu_link" || sample.reason.size() > 256 || !component_utf8(sample.reason) ||
        sample.link_epoch == 0 || (sample.projection_revision == 0 &&
            (sample.status != ComponentHealthStatus::unknown || sample.reason_code != ComponentReason::initial))) return false;
    if (sample.sample_kind != ComponentSampleKind::current && sample.sample_kind != ComponentSampleKind::issue) return false;
    if (sample.sample_kind == ComponentSampleKind::issue && sample.status != ComponentHealthStatus::degraded &&
        sample.status != ComponentHealthStatus::unhealthy) return false;
    switch (sample.reason_code) {
    case ComponentReason::initial: return sample.status == ComponentHealthStatus::unknown;
    case ComponentReason::opening:
    case ComponentReason::negotiating:
        return sample.status == ComponentHealthStatus::connecting || sample.status == ComponentHealthStatus::degraded ||
            sample.status == ComponentHealthStatus::unhealthy;
    case ComponentReason::ready:
    case ComponentReason::recovered: return sample.status == ComponentHealthStatus::healthy;
    case ComponentReason::response_timeout: return sample.status == ComponentHealthStatus::degraded;
    case ComponentReason::peer_reboot:
        return sample.status == ComponentHealthStatus::degraded || sample.status == ComponentHealthStatus::unhealthy;
    case ComponentReason::link_lost:
    case ComponentReason::open_failed:
    case ComponentReason::hello_rejected:
    case ComponentReason::startup_not_ready:
    case ComponentReason::owner_stopped: return sample.status == ComponentHealthStatus::unhealthy;
    default: return false; // Runtime-derived reasons cannot be reported by owners.
    }
}

ComponentAdmission::ComponentAdmission(ComponentPolicyEntry entry, std::string runtime_epoch,
        Lookup lookup, FactSink facts)
    : entry_(std::move(entry)), runtime_epoch_(std::move(runtime_epoch)), lookup_(std::move(lookup)), facts_(std::move(facts)) {
    if (entry_.component != "mcu_link" || entry_.producer_service.empty() || !valid_health_policy(entry_.policy) ||
        runtime_epoch_.size() != 32 || runtime_epoch_.find_first_not_of("0123456789abcdef") != std::string::npos ||
        !lookup_ || !facts_) throw std::invalid_argument("invalid component admission contract");
    identity_.service_name = entry_.producer_service;
    identity_.runtime_epoch = runtime_epoch_;
    committed_.sample.component = entry_.component;
    committed_.identity = identity_;
}

bool ComponentAdmission::producer(const std::string& name) const { return name == entry_.producer_service; }

bool ComponentAdmission::current_instance(const std::shared_ptr<ComponentPeer>& peer) const {
    if (stopped_ || !peer || peer->revoked || peer != peer_) return false;
    const auto status = lookup_(entry_.producer_service);
    return status && status->state == ServiceState::running && status->pid > 0 &&
        status->pid == peer->pid && status->pid == identity_.pid && status->launched_generation > 0 &&
        status->launched_generation == identity_.launched_generation && status->start_time &&
        peer->accepted_at >= *status->start_time;
}

void ComponentAdmission::commit(ComponentHealthFact fact, bool publish) {
    const bool changed = !same_component_sample(committed_.sample, fact.sample) ||
        committed_.identity.pid != fact.identity.pid ||
        committed_.identity.launched_generation != fact.identity.launched_generation ||
        committed_.observation_fresh != fact.observation_fresh;
    committed_ = std::move(fact);
    if (changed && publish && !stopped_) facts_(committed_);
}

void ComponentAdmission::unavailable(ComponentReason reason, Clock::time_point now) {
    (void)now;
    auto fact = committed_;
    fact.identity = identity_;
    fact.sample.status = ComponentHealthStatus::unknown;
    fact.sample.reason_code = reason;
    fact.sample.reason = component_reason_name(reason);
    fact.observation_fresh = false;
    // Preserve owner lower bounds and report time. Derived UNKNOWN is not an owner revision.
    commit(std::move(fact));
}

void ComponentAdmission::observe_lifecycle(Clock::time_point now) {
    if (stopped_) return;
    const auto status = lookup_(entry_.producer_service);
    if (status && status->state == ServiceState::running && status->pid > 0 && status->launched_generation > 0 &&
        (status->launched_generation != identity_.launched_generation || status->pid != identity_.pid)) {
        if (peer_) peer_->revoked = true;
        peer_.reset();
        last_owner_.reset();
        last_report_.reset();
        identity_.pid = status->pid;
        identity_.launched_generation = status->launched_generation;
        identity_.session_id = 0;
        ComponentHealthFact fresh;
        fresh.sample.component = entry_.component;
        fresh.sample.reason = "initial";
        fresh.identity = identity_;
        commit(std::move(fresh)); // New launch clears only the old component episode.
    }
    if (peer_ && !current_instance(peer_)) {
        peer_->revoked = true;
        peer_.reset();
        unavailable(ComponentReason::observation_unavailable, now);
    }
}

void ComponentAdmission::checkpoint(Clock::time_point now) {
    if (stopped_) return;
    observe_lifecycle(now);
    if (committed_.observation_fresh && committed_.received_at && now >= *committed_.received_at &&
        now - *committed_.received_at >= component_observation_lease)
        unavailable(ComponentReason::report_expired, now);
}

ComponentReply ComponentAdmission::reply(const ComponentIngress& ingress, unsigned error) const {
    ComponentReply result;
    result.error = error;
    result.request_id = ingress.request_id;
    result.op = ingress.kind == ComponentIngressKind::bind ? "bind" :
        ingress.kind == ComponentIngressKind::heartbeat ? "heartbeat" : "report";
    result.result = error ? "ERROR" : ingress.kind == ComponentIngressKind::report ? "APPLIED" : "OK";
    result.identity = identity_;
    result.report_seq = ingress.report.report_seq;
    result.projection_revision = ingress.report.sample.projection_revision;
    return result;
}

ComponentReply ComponentAdmission::apply(const ComponentIngress& ingress, Clock::time_point now) {
    checkpoint(now);
    if (stopped_) return reply(ingress, 1003);
    if (ingress.kind != ComponentIngressKind::heartbeat && ingress.request_id == 0) return reply(ingress, 1002);
    auto reject_identity = [&] {
        if (ingress.peer) ingress.peer->revoked = true;
        return reply(ingress, 1004);
    };
    if (!ingress.peer || ingress.peer->revoked || ingress.peer->connection_token == 0 ||
        ingress.peer->accepted_at > now) return reject_identity();
    if (ingress.kind == ComponentIngressKind::bind) {
        if (!producer(ingress.service_name)) return reply(ingress, 1001);
        const auto status = lookup_(entry_.producer_service);
        if (!status || status->state != ServiceState::running || status->pid <= 0 || status->launched_generation == 0 ||
            !status->start_time || ingress.peer->pid != status->pid || ingress.peer->accepted_at < *status->start_time)
            return reject_identity(); // Early connect must reconnect; never wait and relabel.
        if (peer_ == ingress.peer && current_instance(peer_)) return reply(ingress);
        if (next_session_ == 0) throw std::overflow_error("component session IDs exhausted");
        if (peer_) peer_->revoked = true;
        peer_ = ingress.peer;
        identity_.session_id = next_session_;
        next_session_ = next_session_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_session_ + 1;
        last_report_.reset(); // Seq is session-scoped; owner lower bound is launch-scoped.
        unavailable(ComponentReason::observation_unavailable, now);
        return reply(ingress);
    }
    if (ingress.kind != ComponentIngressKind::report && ingress.kind != ComponentIngressKind::heartbeat)
        return reply(ingress, 1002);
    if (!current_instance(ingress.peer) || ingress.report.runtime_epoch != runtime_epoch_ ||
        ingress.report.session_id == 0 || ingress.report.session_id != identity_.session_id)
        return reject_identity();
    if (ingress.kind == ComponentIngressKind::heartbeat) {
        if (!producer(ingress.service_name)) return reject_identity();
        if (ingress.received_at > now || now - ingress.received_at >= component_observation_lease)
            return reply(ingress, 1003);
        return reply(ingress); // Does not renew report lease; Runtime delivers it to SM.
    }
    const auto& report = ingress.report;
    const auto& sample = report.sample;
    if (sample.component != entry_.component) return reply(ingress, 1001);
    if (report.report_seq == 0 || !valid_component_sample(sample)) return reply(ingress, 1002);
    if (last_report_ && report.report_seq <= last_report_->report_seq) {
        if (report.report_seq == last_report_->report_seq && same_component_sample(sample, last_report_->sample))
            return reply(ingress); // ACK loss: repeat ACK, no lease renewal or event.
        return reply(ingress, 1002);
    }
    if (ingress.received_at > now || now - ingress.received_at >= component_observation_lease ||
        (committed_.observation_fresh && committed_.received_at && ingress.received_at < *committed_.received_at))
        return reply(ingress, 1003);
    if (last_owner_) {
        if (sample.projection_revision < last_owner_->projection_revision || sample.link_epoch < last_owner_->link_epoch)
            return reply(ingress, 1002);
        if (sample.projection_revision == last_owner_->projection_revision && !same_component_sample(sample, *last_owner_)) {
            const bool issue_to_current = last_owner_->sample_kind == ComponentSampleKind::issue &&
                sample.sample_kind == ComponentSampleKind::current && sample.link_epoch == last_owner_->link_epoch &&
                sample.peer_boot_id == last_owner_->peer_boot_id;
            if (!issue_to_current) return reply(ingress, 1002);
        }
    }
    const bool repeated_issue = last_owner_ && last_owner_->sample_kind == ComponentSampleKind::issue &&
        same_component_sample(sample, *last_owner_);
    last_owner_ = sample; // Identical issue replay after reconnect emits no duplicate fact.
    last_report_ = report;
    commit(ComponentHealthFact{sample, identity_, ingress.received_at, true}, !repeated_issue);
    return reply(ingress); // Only returned after facts_ completed writer dispatch/DSM commit.
}

void ComponentAdmission::stop() {
    stopped_ = true;
    if (peer_) peer_->revoked = true;
    peer_.reset();
}

ComponentHealthSnapshot ComponentAdmission::snapshot(Clock::time_point now) const {
    ComponentHealthSnapshot view{committed_, entry_.policy, {}};
    if (committed_.received_at) view.report_age = std::chrono::duration_cast<std::chrono::milliseconds>(
        now > *committed_.received_at ? now - *committed_.received_at : Clock::duration::zero());
    return view; // Query never advances lease or calls lookup_/facts_.
}

ResourceSamplingSchedule::ResourceSamplingSchedule(std::chrono::seconds interval) : interval_(interval) {
    if (interval < std::chrono::seconds{1} || interval > std::chrono::seconds{60})
        throw std::invalid_argument("resource interval must be within 1..60 seconds");
}

bool ResourceSamplingSchedule::due(Clock::time_point now) const {
    return !next_due_ || now >= *next_due_;
}

void ResourceSamplingSchedule::completed(Clock::time_point now) { next_due_ = now + interval_; }

bool ResourceSamplingSchedule::tryQueueTick() { return !tick_pending_.exchange(true); }

void ResourceSamplingSchedule::consumeTick() { tick_pending_ = false; }

RuntimeManager::RuntimeManager(const std::string& config_path, AggregationOptions aggregation,
    DeviceStateManager::StateChangeSink device_changes, ResourceThresholds resources)
    : RuntimeManager(legacy_config(config_path), std::move(aggregation),
          std::move(device_changes), ResourceInputMode::external, {}, {}, resources) {}

RuntimeManager::RuntimeManager(RuntimeConfig config, AggregationOptions aggregation,
    DeviceStateManager::StateChangeSink device_changes, ResourceInputMode input_mode,
    ResourceCollector::Reader reader, ResourceCollector::Now now)
    : RuntimeManager(config, std::move(aggregation), std::move(device_changes), input_mode,
          std::move(reader), std::move(now), native_thresholds(config.monitoring)) {}

RuntimeManager::RuntimeManager(RuntimeConfig config, AggregationOptions aggregation,
    DeviceStateManager::StateChangeSink device_changes, ResourceInputMode input_mode,
    ResourceCollector::Reader reader, ResourceCollector::Now now, ResourceThresholds thresholds)
    : logger_(std::cout), resource_mode_(input_mode), resource_interval_(config.monitoring.sample_interval_seconds),
      resource_now_(now ? std::move(now) : ResourceCollector::Now{[] { return Clock::now(); }}),
      collector_(input_mode == ResourceInputMode::native
          ? std::make_unique<ResourceCollector>(std::move(reader), resource_now_) : nullptr),
      resource_schedule_(resource_interval_), monitor_([this](Event event) { queue_.push(std::move(event)); },
          std::chrono::seconds{5}, [this](RuntimeEvent event) { enqueue_resource_fact(std::move(event)); }, thresholds),
      device_states_([this, device_changes = std::move(device_changes)](const DeviceStateSnapshot& state) {
          logger_.log(LogLevel::info, "device_state_manager",
              std::string(state_name(state.previous)) + " -> " + state_name(state.current) +
              " source=" + state.source + " reason=" + state.reason);
          if (device_changes) device_changes(state);
      }),
      aggregation_(device_states_, health_aggregation(config, std::move(aggregation))),
      services_(processes_, monitor_, logger_,
          [this](const ServiceStateChange& change) { service_state_changed(change); },
          [this](const std::string& name) { post(Event{EventType::restart_request, name, Clock::now()}); }) {
    dispatcher_.subscribe([this](const RuntimeEvent& event) {
        if (!aggregation_.handle(event))
            logger_.log(LogLevel::warning, "service_aggregation", "rejected internal runtime event");
    });
    if (resource_mode_ != ResourceInputMode::native && resource_mode_ != ResourceInputMode::external)
        throw std::invalid_argument("invalid resource input mode");
    const auto& configs = config.services;
    for (const auto& config : configs) {
        services_.add(config);
        aggregation_.add_service(config.service_name, config.autostart);
    }
    if (aggregation_.configured_components().size() > 1)
        throw std::invalid_argument("only one component producer is supported");
    for (const auto& entry : aggregation_.configured_components()) {
        const auto definition = services_.queryServiceDefinition(entry.producer_service);
        if (!definition || !definition->autostart || definition->heartbeat_timeout < std::chrono::seconds{4})
            throw std::invalid_argument("component requires an autostart producer with heartbeat timeout >=4s");
        aggregation_.add_component(entry);
        component_admission_ = std::make_unique<ComponentAdmission>(entry, component_runtime_epoch(),
            [this](const std::string& name) { return services_.query(name); },
            [this](const ComponentHealthFact& fact) {
                if (shutting_down_) return;
                RuntimeEvent event{RuntimeEventType::component_health_changed, {}, "component:" + fact.sample.component,
                    fact.sample.reason.empty() ? component_reason_name(fact.sample.reason_code) : fact.sample.reason, Clock::now()};
                event.component_health = fact;
                dispatcher_.publish(std::move(event));
                dispatcher_.drain();
            });
        component_snapshot_.push_back(component_admission_->snapshot(Clock::now()));
    }
    component_mailbox_->wake_queue = &queue_;
    // Validate the complete graph before run() installs signals/starts threads.
    std::vector<ServiceConfig> ordered;
    for (const auto& name : services_.startup_order()) {
        ordered.push_back(*services_.queryServiceDefinition(name));
        if (ordered.back().autostart) autostart_.push_back(name);
    }
    recovery_ = std::make_unique<RecoveryManager>(std::move(ordered), services_,
        [] { return Clock::now(); }, [this] { return !shutting_down_ && running_ && !signal_received; });
    logger_.log(LogLevel::info, "runtime_manager", "loaded " + std::to_string(configs.size()) + " service configuration(s)");
}

RuntimeManager::~RuntimeManager() {
    stop_component_ingress();
    stop_resource_sampling();
    running_ = false;
    if (timer_thread_.joinable()) timer_thread_.join();
    monitor_queue_.push(Event{EventType::shutdown, {}, Clock::now()});
    if (monitor_thread_.joinable()) monitor_thread_.join();
    if (timer_fd_ >= 0) ::close(timer_fd_);
}

void RuntimeManager::post(Event event) {
    if (event.type == EventType::component_ingress_wake)
        throw std::logic_error("component wake is private");
    if (resource_mode_ == ResourceInputMode::native && event.runtime_event &&
        (event.runtime_event->source == "cpu_monitor" || event.runtime_event->source == "memory_monitor"))
        throw std::logic_error("native resource sources are reserved");
    if (event.type == EventType::shutdown) stop_resource_sampling();
    queue_.push(std::move(event));
}

void RuntimeManager::stop_resource_sampling() {
    std::lock_guard<std::mutex> lock(resource_submit_mutex_);
    resource_stopped_ = true;
}

bool RuntimeManager::resource_sampling_active() const { return running_ && !resource_stopped_; }

void RuntimeManager::enqueue_resource_fact(RuntimeEvent event) {
    std::lock_guard<std::mutex> lock(resource_submit_mutex_);
    if (resource_stopped_) return;
    Event envelope{EventType::runtime_event, event.service_name, event.at};
    envelope.runtime_event = std::move(event);
    queue_.push(std::move(envelope));
}

void RuntimeManager::post(DeviceStateEvent event) {
    Event envelope{EventType::device_state, {}, event.timestamp};
    envelope.device_state_event = std::move(event);
    post(std::move(envelope));
}

void RuntimeManager::post(RuntimeEvent event) {
    Event envelope{EventType::runtime_event, event.service_name, event.at};
    envelope.runtime_event = std::move(event);
    post(std::move(envelope));
}

void RuntimeManager::post(RecoveryResult result) {
    Event envelope{EventType::recovery_result, result.service_name, result.completed_at};
    envelope.recovery_result = std::move(result);
    post(std::move(envelope));
}

void RuntimeManager::service_state_changed(const ServiceStateChange& change) {
    // Never call RM/SM or the dispatcher from a lifecycle callback.
    lifecycle_work_.push_back(change);
}

namespace {
bool same_episode(const RecoveryContext& a, const RecoveryContext& b) {
    return a.recovery_generation == b.recovery_generation &&
           a.initial_fault_generation == b.initial_fault_generation && a.origin == b.origin;
}

const char* terminal_reason(RecoveryTerminalReason reason) {
    switch (reason) {
    case RecoveryTerminalReason::completed: return "recovery completed";
    case RecoveryTerminalReason::retry_exhausted: return "automatic restart budget exhausted";
    case RecoveryTerminalReason::recovery_timeout: return "recovery timeout";
    case RecoveryTerminalReason::execution_failure: return "recovery execution failed";
    default: return "recovery cancelled";
    }
}
}

bool RuntimeManager::captured_recovery(const ServiceStateChange& change) const {
    if (!change.operation) return false;
    const auto slot = recovery_->query(change.service_name);
    if (!slot) return false;
    const auto& context = change.operation->context;
    if (slot->active && same_episode(context, slot->active->context)) return true;
    // A synchronous primitive can seal its result before queued callbacks drain.
    // Use its captured receipt, never query a new lifecycle token to relabel it.
    if (!slot->last_result || slot->last_result->outcome == RecoveryOutcome::cancelled) return false;
    const auto& result = *slot->last_result;
    return same_episode(context, RecoveryContext{result.recovery_generation,
        result.initial_fault_generation, result.execution_generation, result.origin}) &&
        (change.generation == result.latest_fault_generation ||
         (result.execution_generation && change.generation == *result.execution_generation));
}

void RuntimeManager::cancel_closure(const std::string& name, RecoveryTerminalReason reason) {
    std::set<std::string> affected{name};
    for (const auto& service : services_.startup_order()) {
        for (const auto& dependency : services_.queryServiceDefinition(service)->dependency)
            if (affected.count(dependency)) { affected.insert(service); break; }
    }
    const auto now = Clock::now();
    for (const auto& service : affected)
        if (service != name || reason == RecoveryTerminalReason::explicit_stop)
            recovery_->cancel(service, service == name ? reason : RecoveryTerminalReason::dependency_stop, now);
}

void RuntimeManager::apply_change(const ServiceStateChange& change) {
    component_checkpoint(change.at); // Lifecycle already committed by SM; revoke before aggregating process facts.
    const bool bound = captured_recovery(change);
    auto emit = [&](RuntimeEventType type, const std::string& reason) {
        RuntimeEvent event{type, change.service_name, "service_manager", reason, change.at};
        event.generation = change.generation;
        if (bound) event.recovery_context = change.operation->context;
        dispatcher_.publish(std::move(event));
        dispatcher_.drain();
    };
    switch (change.to) {
    case ServiceState::running:
        // An invalidated recovery launch must not become an ordinary START fact.
        if (!change.operation || bound)
            emit(RuntimeEventType::service_started, "service entered RUNNING");
        break;
    case ServiceState::failed:
        if (change.operation && !bound) break;
        emit(change.failure_type == FailureType::heartbeat_timeout ? RuntimeEventType::heartbeat_timeout
                                                                 : RuntimeEventType::service_failed,
             change.cause == ServiceChangeCause::recovery_finalization ? "recovery finalization" : "validated lifecycle failure");
        cancel_closure(change.service_name, RecoveryTerminalReason::dependency_stop);
        if (change.cause == ServiceChangeCause::recovery_finalization) {
            if (bound) aggregation_.bind_recovery(change.service_name, change.generation,
                change.operation->context, false, change.at);
        } else if (change.operation) {
            // Rejected/duplicate captured failure is never downgraded to submit.
            recovery_->observeFailure(change.service_name, change.operation->context, change.generation,
                change.failure_type.value_or(FailureType::startup_failure), "validated recovery attempt failure", Clock::now());
        } else {
            RecoveryRequest request;
            request.service_name = change.service_name;
            request.service_generation = change.generation;
            request.failure_type = change.failure_type.value_or(FailureType::startup_failure);
            request.reason = "validated lifecycle failure";
            request.producer_time = change.at;
            recovery_->submit(std::move(request), Clock::now());
        }
        break;
    case ServiceState::stopping:
    case ServiceState::stopped:
        if (change.cause == ServiceChangeCause::dependency_stop || change.cause == ServiceChangeCause::explicit_stop)
            recovery_->cancel(change.service_name, change.cause == ServiceChangeCause::explicit_stop
                ? RecoveryTerminalReason::explicit_stop : RecoveryTerminalReason::dependency_stop, Clock::now());
        emit(RuntimeEventType::service_stopped, "service stop requested or completed");
        if (bound && change.cause == ServiceChangeCause::recovery_preparation)
            aggregation_.bind_recovery(change.service_name, change.generation, change.operation->context, false, change.at);
        // A clean spontaneous prerequisite exit also stops its affected closure.
        cancel_closure(change.service_name, RecoveryTerminalReason::dependency_stop);
        break;
    case ServiceState::recovering:
        if (bound) aggregation_.bind_recovery(change.service_name, change.generation,
            change.operation->context, true, change.at);
        break;
    default: break;
    }
}

void RuntimeManager::drain_work() {
    for (;;) {
        while (!lifecycle_work_.empty()) {
            auto change = std::move(lifecycle_work_.front());
            lifecycle_work_.pop_front();
            if (!shutting_down_) apply_change(change);
        }
        // RM owns admission notifications, including auto after a failed manual launch.
        for (const auto& start : recovery_->takeStarts()) {
            const auto slot = recovery_->query(start.service_name);
            if (shutting_down_ || !slot || !slot->active || !same_episode(start.context, slot->active->context)) continue;
            RuntimeEvent event{RuntimeEventType::recovery_start, start.service_name,
                "recovery_manager", "automatic recovery admitted", start.at};
            event.generation = start.generation;
            event.recovery_context = start.context;
            dispatcher_.publish(std::move(event));
        }
        dispatcher_.drain();
        for (const auto& result : recovery_->takeResults()) {
            logger_.log(LogLevel::info, "recovery_manager", result.service_name + " " + terminal_reason(result.terminal_reason));
            if (shutting_down_) continue;
            const RecoveryContext context{result.recovery_generation, result.initial_fault_generation,
                                          result.execution_generation, result.origin};
            if (result.outcome == RecoveryOutcome::cancelled) {
                aggregation_.cancel_recovery(result.service_name, context, result.completed_at);
            } else if (result.origin == RecoveryOrigin::manual_restart) {
                aggregation_.complete_manual(result);
            } else {
                if (result.outcome != RecoveryOutcome::success)
                    aggregation_.bind_recovery(result.service_name, result.latest_fault_generation,
                        context, false, result.completed_at);
                RuntimeEvent event{result.outcome == RecoveryOutcome::success ? RuntimeEventType::recovery_success
                                                                            : RuntimeEventType::recovery_failed,
                    result.service_name, "recovery_manager", terminal_reason(result.terminal_reason), result.completed_at};
                event.generation = result.outcome == RecoveryOutcome::success ? result.execution_generation
                                                                            : std::optional<std::uint64_t>(result.latest_fault_generation);
                event.recovery_context = context;
                dispatcher_.publish(std::move(event));
                dispatcher_.drain();
            }
        }
        if (device_states_.query().current == DeviceState::offline) {
            recovery_->stopAutomatic(Clock::now());
            // Cancellation receipts only revoke bindings; OFFLINE is latched.
            for (const auto& result : recovery_->takeResults()) {
                // Revoking an RM binding alone leaves SM in RECOVERING with
                // no task/deadline. Complete cancellation through the existing
                // lifecycle stop primitive, preserving any outstanding grace.
                services_.stopService(result.service_name, Clock::now());
                aggregation_.cancel_recovery(result.service_name, RecoveryContext{result.recovery_generation,
                    result.initial_fault_generation, result.execution_generation, result.origin}, result.completed_at);
            }
        }
        if (lifecycle_work_.empty()) break;
    }
}

void RuntimeManager::handle_event(const Event& event) {
    if (event.type == EventType::component_ingress_wake) {
        reap_children(); // Known exits and captured lifecycle work precede component identity admission.
        drain_component_ingress(event.component_ingress_batch, Clock::now());
    } else if (event.type == EventType::heartbeat && component_producer(event.service_name)) {
        logger_.log(LogLevel::warning, "runtime_manager", "rejected unbound component producer heartbeat");
    } else if (event.type == EventType::runtime_event) {
        // Only resource facts use the public RuntimeEvent ingress. Lifecycle and
        // recovery facts originate from the captured SM/RM writer adapters.
        if (!shutting_down_ && event.runtime_event && event.runtime_event->type == RuntimeEventType::resource_warning)
            dispatcher_.publish(*event.runtime_event);
        else logger_.log(LogLevel::warning, "runtime_manager", "rejected unowned runtime fact");
    } else if (event.type == EventType::recovery_result) {
        if (!event.recovery_result || !recovery_->observe(*event.recovery_result, Clock::now()))
            logger_.log(LogLevel::warning, "runtime_manager", "rejected recovery result");
    } else if (event.type == EventType::device_state) {
        // Public device triggers cannot bypass RM's terminal authority.
        if (!event.device_state_event || event.device_state_event->type == DeviceStateEventType::recovery_started ||
            event.device_state_event->type == DeviceStateEventType::recovery_succeeded ||
            event.device_state_event->type == DeviceStateEventType::recovery_failed ||
            event.device_state_event->health_target ||
            device_states_.handle(*event.device_state_event) != DeviceTransitionResult::transitioned)
            logger_.log(LogLevel::warning, "device_state_manager", "rejected device state event");
    } else if (event.type == EventType::restart_request) {
        const auto status = services_.query(event.service_name);
        if (!status) return;
        // CREATED has no token yet; capture an explicit preparation STOP first.
        if (status->generation == 0) {
            services_.stopService(event.service_name, Clock::now());
            drain_work();
        }
        cancel_closure(event.service_name, RecoveryTerminalReason::dependency_stop);
        RecoveryRequest request;
        request.service_name = event.service_name;
        request.origin = RecoveryOrigin::manual_restart;
        request.failure_type = FailureType::manual_request;
        request.service_generation = services_.query(event.service_name)->generation;
        request.reason = "manual restart request";
        request.producer_time = event.at;
        recovery_->submit(std::move(request), Clock::now());
        // Already STOPPED manual preparation may produce no lifecycle callback.
        const auto slot = recovery_->query(event.service_name);
        if (slot && slot->active) {
            drain_work();
            aggregation_.bind_recovery(event.service_name, slot->active->expected_generation,
                slot->active->context, false, Clock::now());
        }
    } else {
        if (event.type == EventType::stop) cancel_closure(event.service_name, RecoveryTerminalReason::explicit_stop);
        if (event.type == EventType::process_exited) {
            if (!event.instance_generation || *event.instance_generation == 0) return;
            if (services_.query(event.service_name))
                services_.handleProcessExit(event, recovery_->classifyExit(event.service_name, event.exit_status));
        } else if (event.type == EventType::health_missed) {
            if (event.instance_generation && *event.instance_generation != 0) services_.handle(event);
        } else services_.handle(event);
    }
    drain_work();
}

void RuntimeManager::reap_children() {
    for (const auto& exit : processes_.reap()) {
        for (const auto& status : services_.all_statuses()) {
            if (status.pid != exit.pid) continue;
            Event event{EventType::process_exited, status.service_name, Clock::now(), exit.pid, exit.status};
            event.instance_generation = status.launched_generation;
            // Apply the already reaped child before any deadline decision.
            handle_event(event);
            break;
        }
    }
}

DeviceStateSnapshot RuntimeManager::queryDeviceState() const { return device_states_.query(); }

bool RuntimeManager::component_producer(const std::string& name) const {
    return component_admission_ && component_admission_->producer(name);
}

RuntimeManager::ComponentSink RuntimeManager::component_sink() {
    const std::weak_ptr<ComponentMailbox> weak = component_mailbox_;
    return [weak](ComponentIngress ingress) {
        const auto mailbox = weak.lock();
        if (!mailbox || !ingress.peer || ingress.peer->revoked) return false;
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        if (mailbox->stopped || !mailbox->wake_queue || mailbox->outstanding >= component_ingress_limit ||
            ingress.peer->pending.exchange(true)) return false;
        const auto peer = ingress.peer;
        try {
            mailbox->operations.push_back(std::move(ingress));
            ++mailbox->outstanding;
            if (!mailbox->wake_pending) {
                Event wake{EventType::component_ingress_wake, {}, Clock::now()};
                wake.component_ingress_batch = 1;
                mailbox->wake_queue->push(std::move(wake));
                mailbox->wake_pending = true;
            }
        } catch (...) {
            // A failed wake insertion must not strand a pending operation.
            if (!mailbox->wake_pending && !mailbox->operations.empty()) {
                mailbox->operations.pop_back();
                --mailbox->outstanding;
            }
            peer->pending = false;
            throw;
        }
        return true; // Transport acceptance only, never an APPLIED ACK.
    };
}

void RuntimeManager::drain_component_ingress(unsigned batch, Clock::time_point now) {
    std::deque<ComponentIngress> work;
    {
        std::lock_guard<std::mutex> lock(component_mailbox_->mutex);
        if (component_mailbox_->stopped) return;
        // A wake captures its batch before it enters the original FIFO. Later
        // operations wait for a new wake, so cannot jump past an intervening STOP.
        for (unsigned i = 0; i < batch && i < 8 && !component_mailbox_->operations.empty(); ++i) {
            work.push_back(std::move(component_mailbox_->operations.front()));
            component_mailbox_->operations.pop_front();
        }
        component_mailbox_->wake_pending = false;
        if (!component_mailbox_->operations.empty()) {
            Event wake{EventType::component_ingress_wake, {}, now};
            wake.component_ingress_batch = static_cast<unsigned>(std::min<std::size_t>(8, component_mailbox_->operations.size()));
            queue_.push(std::move(wake));
            component_mailbox_->wake_pending = true;
        }
    }
    component_checkpoint(now);
    for (const auto& ingress : work) {
        ComponentReply reply;
        try {
            if (shutting_down_ || !component_admission_) {
                reply.error = 1003;
                reply.request_id = ingress.request_id;
            } else {
                reply = component_admission_->apply(ingress, Clock::now());
                if (!reply.error && ingress.kind == ComponentIngressKind::heartbeat) {
                    Event heartbeat{EventType::heartbeat, ingress.service_name, ingress.received_at, reply.identity.pid};
                    heartbeat.instance_generation = reply.identity.launched_generation;
                    services_.handle(heartbeat); // Private verified path; public name-only type4 is rejected.
                    drain_work();
                }
                component_checkpoint(Clock::now()); // Publish committed cache before asynchronous ACK.
            }
        } catch (...) {
            for (const auto& abandoned : work) {
                abandoned.peer->pending = false;
                abandoned.peer->revoked = true;
            }
            stop_component_ingress();
            throw;
        }
        {
            std::lock_guard<std::mutex> lock(component_mailbox_->mutex);
            --component_mailbox_->outstanding;
            ingress.peer->pending = false;
        }
        if (const auto sink = ingress.reply.lock()) sink->completed(reply);
    }
}

void RuntimeManager::component_checkpoint(Clock::time_point now) {
    if (shutting_down_ || !component_admission_) return;
    component_admission_->checkpoint(now);
    auto view = component_admission_->snapshot(now);
    std::lock_guard<std::mutex> lock(component_snapshot_mutex_);
    component_snapshot_ = {std::move(view)};
}

void RuntimeManager::stop_component_ingress() {
    std::lock_guard<std::mutex> lock(component_mailbox_->mutex);
    component_mailbox_->stopped = true;
    component_mailbox_->wake_queue = nullptr;
    for (const auto& ingress : component_mailbox_->operations) {
        ingress.peer->revoked = true;
        ingress.peer->pending = false;
    }
    component_mailbox_->operations.clear();
    component_mailbox_->outstanding = 0;
    if (component_admission_) component_admission_->stop();
}

std::vector<ComponentHealthSnapshot> RuntimeManager::queryComponentHealth(Clock::time_point now) const {
    std::vector<ComponentHealthSnapshot> views;
    {
        std::lock_guard<std::mutex> lock(component_snapshot_mutex_);
        views = component_snapshot_;
    }
    for (auto& view : views) {
        if (view.fact.received_at) view.report_age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now > *view.fact.received_at ? now - *view.fact.received_at : Clock::duration::zero());
    }
    return views; // Only the committed cache; no admission/SM/lease evaluation.
}

std::optional<ResourceSnapshotView> RuntimeManager::queryResourceSnapshot(Clock::time_point now) const {
    ResourceSnapshotView view;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (!resource_snapshot_) return std::nullopt;
        view.snapshot = *resource_snapshot_;
    }
    auto age = [now](const auto& metric) -> std::optional<Clock::duration> {
        if (!metric.last_success_at) return std::nullopt;
        return now > *metric.last_success_at ? now - *metric.last_success_at : Clock::duration::zero();
    };
    view.cpu_age = age(view.snapshot.system.cpu);
    view.memory_age = age(view.snapshot.system.memory);
    view.cpu_stale = view.cpu_age && *view.cpu_age > 3 * resource_interval_;
    view.memory_stale = view.memory_age && *view.memory_age > 3 * resource_interval_;
    return view;
}

void RuntimeManager::log_resource_failures(const ResourceSnapshotBundle& snapshot) {
    const auto at = snapshot.collected_at;
    auto log = [&](ResourceLogState& state, bool failing, const std::string& name, const std::string& detail) {
        if (failing) {
            if (!state.failing || at >= state.next_log) {
                logger_.log(LogLevel::warning, "resource_monitor", name + " unavailable " + detail);
                state.next_log = at + std::chrono::seconds{30};
            }
        } else if (state.failing) {
            logger_.log(LogLevel::info, "resource_monitor", name + " collection recovered");
        }
        state.failing = failing;
    };
    log(cpu_log_, snapshot.system.cpu.quality == MetricQuality::unavailable, "CPU",
        "consecutive_errors=" + std::to_string(snapshot.system.cpu.consecutive_errors));
    log(memory_log_, snapshot.system.memory.quality == MetricQuality::unavailable, "Memory",
        "consecutive_errors=" + std::to_string(snapshot.system.memory.consecutive_errors));
    std::size_t failed = 0;
    for (const auto& row : snapshot.processes)
        if (row.observation_status != ProcessObservationStatus::observed ||
            row.cpu_quality == MetricQuality::unavailable) ++failed;
    log(process_log_, snapshot.process_scan_quality == MetricQuality::unavailable || failed != 0,
        "process scan", "failed_rows=" + std::to_string(failed) +
        " total_rows=" + std::to_string(snapshot.processes.size()));
}

void RuntimeManager::sample_resources() {
    if (!resource_sampling_active()) return;
    const auto started = resource_now_();
    ResourceSnapshotBundle snapshot;
    snapshot.system = collector_->collectSystem();
    if (!resource_sampling_active()) return;
    const auto captured = services_.trySnapshotProcessIdentities();
    auto scan = collector_->collectProcesses(captured, [this](const auto& identities) {
        return services_.tryValidateProcessIdentities(identities);
    }, resource_interval_, [this] { return resource_sampling_active(); });
    snapshot.process_scan_quality = scan.quality;
    snapshot.processes = std::move(scan.processes);
    const auto completed = resource_now_();
    snapshot.collected_at = completed;
    {
        std::lock_guard<std::mutex> submit(resource_submit_mutex_);
        if (!resource_sampling_active()) return;
        std::lock_guard<std::mutex> cache(snapshot_mutex_);
        resource_snapshot_ = snapshot;
    }
    // The private sink rechecks the synchronized stop gate for every queued fact.
    if (!resource_sampling_active()) return;
    monitor_.observeResources(snapshot.system);
    log_resource_failures(snapshot);
    if (completed - started > resource_interval_ && (!next_overrun_log_ || completed >= *next_overrun_log_)) {
        logger_.log(LogLevel::warning, "resource_monitor", "collection exceeded sampling interval; next cycle postponed");
        next_overrun_log_ = completed + std::chrono::seconds{30};
    }
    // Includes policy/logging cost, and advances even after expected read failures.
    resource_schedule_.completed(resource_now_());
}

void RuntimeManager::reportResourceUsage(double cpu_percent, double memory_percent, Clock::time_point at) {
    if (resource_mode_ == ResourceInputMode::native)
        throw std::logic_error("native mode rejects external resource percentages");
    monitor_.report_resources(cpu_percent, memory_percent, at);
}

std::optional<ServiceStatus> RuntimeManager::query(const std::string& name) const {
    return services_.query(name);
}

void RuntimeManager::run() {
    if (running_.exchange(true)) throw std::logic_error("runtime already running");
    signal_received = 0;
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    struct sigaction old_int{}, old_term{};
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);
    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer_fd_ < 0) {
        const int error = errno;
        running_ = false;
        sigaction(SIGINT, &old_int, nullptr);
        sigaction(SIGTERM, &old_term, nullptr);
        throw std::system_error(error, std::generic_category(), "timerfd_create");
    }
    itimerspec timer{};
    timer.it_value.tv_sec = 1;
    timer.it_interval.tv_sec = 1;
    if (::timerfd_settime(timer_fd_, 0, &timer, nullptr) < 0) {
        const int error = errno;
        ::close(timer_fd_);
        timer_fd_ = -1;
        running_ = false;
        sigaction(SIGINT, &old_int, nullptr);
        sigaction(SIGTERM, &old_term, nullptr);
        throw std::system_error(error, std::generic_category(), "timerfd_settime");
    }
    try {
        monitor_thread_ = std::thread([this] {
            try {
                for (;;) {
                    Event event{};
                    if (!monitor_queue_.pop_for(event, std::chrono::milliseconds(200))) {
                        if (!running_) break;
                        continue;
                    }
                    if (event.type == EventType::shutdown) break;
                    if (event.type == EventType::health_check) {
                        resource_schedule_.consumeTick();
                        if (!resource_sampling_active()) continue;
                        // Heartbeat uses actual time and runs before any procfs I/O.
                        monitor_.check(Clock::now());
                        if (resource_mode_ == ResourceInputMode::native && resource_sampling_active() &&
                            resource_schedule_.due(resource_now_())) sample_resources();
                    }
                }
            } catch (...) {
                monitor_failure_ = std::current_exception();
                stop_resource_sampling();
                try { logger_.log(LogLevel::error, "resource_monitor", "fatal monitor worker exception; requesting shutdown"); }
                catch (...) {} // Cleanup still owns the original worker exception.
                post(Event{EventType::shutdown, {}, Clock::now()});
            }
        });
        timer_thread_ = std::thread([this] {
            while (running_) {
                std::uint64_t expirations = 0;
                const auto count = ::read(timer_fd_, &expirations, sizeof(expirations));
                if (count < 0 && errno == EINTR) continue;
                if (count != static_cast<ssize_t>(sizeof(expirations))) {
                    if (running_) {
                        logger_.log(LogLevel::error, "runtime_manager", "timerfd read failed");
                        post(Event{EventType::shutdown, {}, Clock::now()});
                    }
                    break;
                }
                if (running_ && resource_schedule_.tryQueueTick())
                    monitor_queue_.push(Event{EventType::health_check, {}, Clock::now()});
            }
        });
    } catch (...) {
        stop_resource_sampling();
        running_ = false;
        if (timer_thread_.joinable()) timer_thread_.join();
        monitor_queue_.push(Event{EventType::shutdown, {}, Clock::now()});
        if (monitor_thread_.joinable()) monitor_thread_.join();
        ::close(timer_fd_);
        timer_fd_ = -1;
        sigaction(SIGINT, &old_int, nullptr);
        sigaction(SIGTERM, &old_term, nullptr);
        throw;
    }
    std::exception_ptr failure;
    try {
        // Initialization is an explicit runtime trigger, independent of service health.
        device_states_.handle(DeviceStateEvent{DeviceStateEventType::runtime_initialized,
            "runtime_manager", "runtime event loop initialized", Clock::now()});
        aggregation_.refresh();
        for (const auto& name : autostart_) post(Event{EventType::start, name, Clock::now()});
        logger_.log(LogLevel::info, "runtime_manager", "event loop started");
        while (running_ && !signal_received) {
            Event event{};
            if (queue_.pop_for(event, std::chrono::milliseconds(200))) {
                if (event.type == EventType::shutdown) break;
                handle_event(event);
            }
            reap_children();
            services_.tick(Clock::now());
            drain_work();
            recovery_->tick(Clock::now());
            drain_work();
            component_checkpoint(Clock::now());
        }
    } catch (...) {
        failure = std::current_exception();
    }
    stop_resource_sampling();
    shutting_down_ = true;
    stop_component_ingress();
    try {
        recovery_->cancelAll(Clock::now());
        drain_work();
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
    running_ = false;
    if (timer_thread_.joinable()) timer_thread_.join();
    monitor_queue_.push(Event{EventType::shutdown, {}, Clock::now()});
    if (monitor_thread_.joinable()) monitor_thread_.join();
    if (!failure && monitor_failure_) failure = monitor_failure_;
    ::close(timer_fd_);
    timer_fd_ = -1;
    // Normal runtime shutdown preserves the last health snapshot, as in Thread 1.
    try {
        services_.stop_all(Clock::now());
        drain_work();
        // Honor every configured grace deadline, then allow bounded SIGKILL/reap
        // completion. Failure is explicit; never report a successful partial shutdown.
        const auto deadline = services_.shutdown_deadline().value_or(Clock::now()) + std::chrono::seconds(5);
        for (;;) {
            bool active = false;
            reap_children();
            for (const auto& status : services_.all_statuses()) active |= status.pid > 0;
            if (!active) break;
            if (Clock::now() >= deadline)
                throw std::runtime_error("runtime shutdown timed out before all services were reaped");
            services_.tick(Clock::now());
            drain_work();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
    logger_.log(LogLevel::info, "runtime_manager", "event loop stopped");
    if (failure) std::rethrow_exception(failure);
}

} // namespace runtime
