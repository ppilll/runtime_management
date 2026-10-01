#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <ostream>
#include <queue>
#include <string>
#include <thread>

namespace runtime {

enum class LogLevel { info, warning, error };

class Logger {
public:
    explicit Logger(std::ostream& output);
    ~Logger();
    void log(LogLevel level, const std::string& module, const std::string& message);

private:
    struct Entry {
        std::chrono::system_clock::time_point at;
        LogLevel level;
        std::string module;
        std::string message;
    };
    void write_loop();
    std::ostream& output_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<Entry> pending_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace runtime
