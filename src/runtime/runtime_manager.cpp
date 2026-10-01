#include "runtime/runtime_manager.hpp"
#include "runtime/config_manager.hpp"
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <sys/timerfd.h>
#include <unistd.h>

namespace runtime {
namespace {
volatile std::sig_atomic_t signal_received = 0;
void on_signal(int) { signal_received = 1; }
}

RuntimeManager::RuntimeManager(const std::string& config_path)
    : logger_(std::cout), monitor_([this](Event event) { queue_.push(std::move(event)); }),
      services_(processes_, monitor_, logger_) {
    const auto configs = ConfigManager::load_file(config_path);
    for (const auto& config : configs) {
        services_.add(config);
        if (config.autostart) autostart_.push_back(config.service_name);
    }
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
    for (const auto& name : autostart_) post(Event{EventType::start, name, Clock::now()});
    logger_.log(LogLevel::info, "runtime_manager", "event loop started");
    while (running_ && !signal_received) {
        Event event{};
        if (queue_.pop_for(event, std::chrono::milliseconds(200))) {
            if (event.type == EventType::shutdown) break;
            services_.handle(event);
        }
        for (const auto& exit : processes_.reap()) {
            for (const auto& status : services_.all_statuses()) {
                if (status.pid == exit.pid)
                    post(Event{EventType::process_exited, status.service_name, Clock::now(), exit.pid, exit.status});
            }
        }
        services_.tick(Clock::now());
    }
    running_ = false;
    if (timer_thread_.joinable()) timer_thread_.join();
    monitor_queue_.push(Event{EventType::shutdown, {}, Clock::now()});
    if (monitor_thread_.joinable()) monitor_thread_.join();
    ::close(timer_fd_);
    timer_fd_ = -1;
    services_.stop_all(Clock::now());
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (Clock::now() < deadline) {
        bool active = false;
        for (const auto& exit : processes_.reap()) {
            for (const auto& status : services_.all_statuses()) {
                if (status.pid == exit.pid)
                    services_.handle(Event{EventType::process_exited, status.service_name, Clock::now(), exit.pid, exit.status});
            }
        }
        for (const auto& status : services_.all_statuses()) active |= status.pid > 0;
        if (!active) break;
        services_.tick(Clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
    logger_.log(LogLevel::info, "runtime_manager", "event loop stopped");
}

} // namespace runtime
