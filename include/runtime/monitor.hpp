#pragma once

#include "runtime/event.hpp"
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime {

class Monitor {
public:
    using Sink = std::function<void(Event)>;
    explicit Monitor(Sink sink, std::chrono::seconds interval = std::chrono::seconds{5});
    void watch(const std::string& name, Clock::time_point now, std::chrono::seconds timeout);
    void unwatch(const std::string& name);
    void heartbeat(const std::string& name, Clock::time_point now);
    void check(Clock::time_point now);

private:
    struct Watch {
        Clock::time_point last;
        Clock::time_point next_miss;
        std::chrono::seconds timeout;
        unsigned misses = 0;
    };
    Sink sink_;
    std::chrono::seconds interval_;
    std::mutex mutex_;
    std::unordered_map<std::string, Watch> watches_;
};

} // namespace runtime
