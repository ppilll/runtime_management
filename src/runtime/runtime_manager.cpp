#include "runtime/runtime_manager.hpp"
#include "runtime/config_manager.hpp"
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/timerfd.h>
#include <unistd.h>

namespace runtime {
namespace {
volatile std::sig_atomic_t signal_received = 0;
void on_signal(int) { signal_received = 1; }
}

RuntimeManager::RuntimeManager(const std::string& config_path, AggregationOptions aggregation,
                               DeviceStateManager::StateChangeSink device_changes, ResourceThresholds resources)
    : logger_(std::cout), monitor_([this](Event event) { queue_.push(std::move(event)); },
          std::chrono::seconds{5}, [this](RuntimeEvent event) { post(std::move(event)); }, resources),
      device_states_([this, device_changes = std::move(device_changes)](const DeviceStateSnapshot& state) {
          logger_.log(LogLevel::info, "device_state_manager",
              std::string(state_name(state.previous)) + " -> " + state_name(state.current) +
              " source=" + state.source + " reason=" + state.reason);
          if (device_changes) device_changes(state);
      }),
      aggregation_(device_states_, std::move(aggregation)),
      services_(processes_, monitor_, logger_,
          [this](const ServiceStateChange& change) { service_state_changed(change); },
          [this](const std::string& name) { post(Event{EventType::restart_request, name, Clock::now()}); }) {
    dispatcher_.subscribe([this](const RuntimeEvent& event) {
        if (!aggregation_.handle(event))
            logger_.log(LogLevel::warning, "service_aggregation", "rejected internal runtime event");
    });
    const auto configs = ConfigManager::load_file(config_path);
    for (const auto& config : configs) {
        services_.add(config);
        aggregation_.add_service(config.service_name, config.autostart);
    }
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
    running_ = false;
    if (timer_thread_.joinable()) timer_thread_.join();
    monitor_queue_.push(Event{EventType::shutdown, {}, Clock::now()});
    if (monitor_thread_.joinable()) monitor_thread_.join();
    if (timer_fd_ >= 0) ::close(timer_fd_);
}

void RuntimeManager::post(Event event) { queue_.push(std::move(event)); }

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
    if (event.type == EventType::runtime_event) {
        // Only resource facts use the public RuntimeEvent ingress. Lifecycle and
        // recovery facts originate from the captured SM/RM writer adapters.
        if (event.runtime_event && event.runtime_event->type == RuntimeEventType::resource_warning)
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

void RuntimeManager::reportResourceUsage(double cpu_percent, double memory_percent, Clock::time_point at) {
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
            for (;;) {
                Event event{};
                if (!monitor_queue_.pop_for(event, std::chrono::milliseconds(200))) {
                    if (!running_) break;
                    continue;
                }
                if (event.type == EventType::shutdown) break;
                if (event.type == EventType::health_check) monitor_.check(event.at);
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
                if (running_) monitor_queue_.push(Event{EventType::health_check, {}, Clock::now()});
            }
        });
    } catch (...) {
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
        }
    } catch (...) {
        failure = std::current_exception();
    }
    shutting_down_ = true;
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
