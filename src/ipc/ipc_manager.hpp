#pragma once

#include "runtime/event.hpp"
#include "runtime/service_manager.hpp"
#include <atomic>
#include <functional>
#include <memory>
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
    using QueryDevice = std::function<DeviceStateSnapshot()>;
    using DeviceStateSink = std::function<void(const DeviceStateSnapshot&)>;

    // Definitions are a static configuration snapshot, never a registration API.
    IpcManager(std::string control_path, std::string service_path, Post post, Query query,
               std::vector<ServiceConfig> definitions = {}, QueryDevice query_device = {});
    ~IpcManager();
    IpcManager(const IpcManager&) = delete;
    IpcManager& operator=(const IpcManager&) = delete;
    void start();
    void stop();
    // Safe to retain after destruction: the callback owns only a weak queue reference.
    // The runtime writer publishes committed snapshots; it never touches sockets.
    DeviceStateSink device_state_sink() const;

private:
    void serve();
    std::string control_path_;
    std::string service_path_;
    Post post_;
    Query query_;
    std::vector<ServiceConfig> definitions_;
    QueryDevice query_device_;
    struct DeviceEvents;
    std::shared_ptr<DeviceEvents> device_events_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int control_fd_ = -1;
    int service_fd_ = -1;
    int epoll_fd_ = -1;
    bool started_ = false;
};

} // namespace runtime
