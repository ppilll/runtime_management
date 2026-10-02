#include "runtime/runtime_manager.hpp"
#include "runtime/config_manager.hpp"
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <exception>
#include <iostream>
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
          [this](const ServiceStateChange& change) { service_state_changed(change); }) {
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
    for (const auto& name : services_.startup_order())
        if (services_.queryServiceDefinition(name)->autostart) autostart_.push_back(name);
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

void RuntimeManager::service_state_changed(const ServiceStateChange& change) {
    if (shutting_down_) return;
    auto emit = [&](RuntimeEventType type, const std::string& reason) {
        RuntimeEvent event{type, change.service_name, "service_manager", reason, change.at};
        event.generation = change.generation;
        dispatcher_.publish(std::move(event));
    };
    switch (change.to) {
    case ServiceState::running:
        emit(RuntimeEventType::service_started, "service entered RUNNING");
        if (service_recoveries_.erase(change.service_name) != 0)
            emit(RuntimeEventType::recovery_success, "service running after failure");
        break;
    case ServiceState::failed:
        service_recoveries_.insert(change.service_name);
        // Emit timeout facts only after ServiceManager's timestamp/PID/state
        // validation accepts the monitor event. Rejected stale misses emit nothing.
        if (service_cause_ && service_cause_->type == EventType::health_missed)
            emit(RuntimeEventType::heartbeat_timeout, "validated heartbeat timeout");
        else if (service_cause_ && service_cause_->type == EventType::process_exited)
            emit(RuntimeEventType::service_failed,
                 "process exited, status=" + std::to_string(service_cause_->exit_status));
        else
            emit(RuntimeEventType::service_failed, "service lifecycle failure (see service_manager log)");
        if (change.recovery_exhausted)
            emit(RuntimeEventType::recovery_failed, "automatic restart budget exhausted");
        break;
    case ServiceState::stopping:
    case ServiceState::stopped:
        service_recoveries_.erase(change.service_name);
        emit(RuntimeEventType::service_stopped, "service stop requested or completed");
        break;
    case ServiceState::recovering:
        emit(RuntimeEventType::recovery_start, "service restart backoff started");
        break;
    default: break;
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
                if (event.type == EventType::device_state) {
                    if (!event.device_state_event ||
                        device_states_.handle(*event.device_state_event) != DeviceTransitionResult::transitioned)
                        logger_.log(LogLevel::warning, "device_state_manager", "rejected device state event");
                } else if (event.type == EventType::runtime_event) {
                    if (event.runtime_event) dispatcher_.publish(std::move(*event.runtime_event));
                    else logger_.log(LogLevel::warning, "runtime_manager", "missing internal runtime event payload");
                } else {
                    service_cause_ = &event;
                    services_.handle(event);
                    service_cause_ = nullptr;
                }
                // Causal service facts finish before the next queued command/fact.
                dispatcher_.drain();
            }
            for (const auto& exit : processes_.reap()) {
                for (const auto& status : services_.all_statuses()) {
                    if (status.pid == exit.pid)
                        post(Event{EventType::process_exited, status.service_name, Clock::now(), exit.pid, exit.status});
                }
            }
            services_.tick(Clock::now());
            dispatcher_.drain();
        }
    } catch (...) {
        service_cause_ = nullptr;
        failure = std::current_exception();
    }
    running_ = false;
    if (timer_thread_.joinable()) timer_thread_.join();
    monitor_queue_.push(Event{EventType::shutdown, {}, Clock::now()});
    if (monitor_thread_.joinable()) monitor_thread_.join();
    ::close(timer_fd_);
    timer_fd_ = -1;
    // Normal runtime shutdown preserves the last health snapshot, as in Thread 1.
    shutting_down_ = true;
    try {
        services_.stop_all(Clock::now());
        // Honor every configured grace deadline, then allow bounded SIGKILL/reap
        // completion. Failure is explicit; never report a successful partial shutdown.
        const auto deadline = services_.shutdown_deadline().value_or(Clock::now()) + std::chrono::seconds(5);
        for (;;) {
            bool active = false;
            for (const auto& exit : processes_.reap()) {
                for (const auto& status : services_.all_statuses()) {
                    if (status.pid == exit.pid)
                        services_.handle(Event{EventType::process_exited, status.service_name, Clock::now(), exit.pid, exit.status});
                }
            }
            for (const auto& status : services_.all_statuses()) active |= status.pid > 0;
            if (!active) break;
            if (Clock::now() >= deadline)
                throw std::runtime_error("runtime shutdown timed out before all services were reaped");
            services_.tick(Clock::now());
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
