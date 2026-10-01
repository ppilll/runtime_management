#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

namespace runtime {

using Clock = std::chrono::steady_clock;

enum class EventType { start, stop, heartbeat, health_check, health_missed, process_exited, shutdown };

struct Event {
    EventType type;
    std::string service_name;
    Clock::time_point at = Clock::now();
    int pid = -1;
    int exit_status = 0;
    unsigned missed_count = 0;
};

class EventQueue {
public:
    void push(Event event);
    bool pop_for(Event& event, std::chrono::milliseconds timeout);

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<Event> events_;
};

} // namespace runtime
