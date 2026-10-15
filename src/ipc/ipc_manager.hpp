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

class RuntimeManager;
// Construction captures Runtime's private, weak mailbox capability. Only IPC
// may submit it; this does not expose a public normalized-fact posting API.
class ComponentTransportAdapter {
public:
    explicit ComponentTransportAdapter(RuntimeManager& runtime);
private:
    friend class IpcManager;
    std::function<bool(ComponentIngress)> submit_;
    std::vector<ComponentPolicyEntry> components_;
};

// Socket transport only: lifecycle changes are submitted to the Runtime event queue.
class IpcManager {
public:
    using Post = std::function<void(Event)>;
    using Query = std::function<std::optional<ServiceStatus>(const std::string&)>;
    using QueryDevice = std::function<DeviceStateSnapshot()>;
    using DeviceStateSink = std::function<void(const DeviceStateSnapshot&)>;
    using QueryComponents = std::function<std::vector<ComponentHealthSnapshot>()>;

    // Definitions are a static configuration snapshot, never a registration API.
    IpcManager(std::string control_path, std::string service_path, Post post, Query query,
               std::vector<ServiceConfig> definitions = {}, QueryDevice query_device = {},
               std::shared_ptr<ComponentTransportAdapter> components = {}, QueryComponents query_components = {});
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
    std::shared_ptr<ComponentTransportAdapter> components_;
    QueryComponents query_components_;
    struct DeviceEvents;
    std::shared_ptr<DeviceEvents> device_events_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int control_fd_ = -1;
    int service_fd_ = -1;
    int epoll_fd_ = -1;
    bool started_ = false;
    std::uint64_t next_connection_token_ = 1; // Never reused/wrapped across start/stop.
};

} // namespace runtime
