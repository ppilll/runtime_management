#include "runtime/logger.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <utility>

namespace runtime {

Logger::Logger(std::ostream& output) : output_(output), thread_([this] { write_loop(); }) {}

Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void Logger::log(LogLevel level, const std::string& module, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push(Entry{std::chrono::system_clock::now(), level, module, message});
    }
    ready_.notify_one();
}

void Logger::write_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (pending_.empty()) break;
        Entry entry = std::move(pending_.front());
        pending_.pop();
        lock.unlock();
        const char* label = entry.level == LogLevel::info ? "INFO" :
            entry.level == LogLevel::warning ? "WARN" : "ERROR";
        const auto time = std::chrono::system_clock::to_time_t(entry.at);
        output_ << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
                << " [" << label << "] " << entry.module << ": " << entry.message << '\n';
        output_.flush();
        lock.lock();
    }
}

} // namespace runtime
