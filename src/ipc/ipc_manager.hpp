#pragma once

#include "runtime/event.hpp"
#include "runtime/service_manager.hpp"
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace runtime {

// Socket transport only: lifecycle changes are submitted to the Runtime event queue.
class IpcManager {
public:
    using Post = std::function<void(Event)>;
    using Query = std::function<std::optional<ServiceStatus>(const std::string&)>;

    // Definitions are a static configuration snapshot, never a registration API.
    IpcManager(std::string control_path, std::string service_path, Post post, Query query,
               std::vector<ServiceConfig> definitions = {});
    ~IpcManager();
    IpcManager(const IpcManager&) = delete;
    IpcManager& operator=(const IpcManager&) = delete;
    void start();
    void stop();

private:
    void serve();
    std::string control_path_;
    std::string service_path_;
    Post post_;
    Query query_;
    std::vector<ServiceConfig> definitions_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int control_fd_ = -1;
    int service_fd_ = -1;
    int epoll_fd_ = -1;
    bool started_ = false;
};

} // namespace runtime
