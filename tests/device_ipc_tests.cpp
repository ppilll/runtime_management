#include "../src/ipc/frame.hpp"
#include "../src/ipc/ipc_manager.hpp"
#include "runtime/device_state_manager.hpp"
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Socket {
    int fd = -1;
    explicit Socket(const std::string& path) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        require(fd >= 0, "socket failed");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        require(path.size() < sizeof(address.sun_path), "socket path too long");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        timeval timeout{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd); fd = -1;
            throw std::runtime_error("connect failed");
        }
    }
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};

void send_all(int fd, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (n > 0) offset += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else throw std::runtime_error("send failed");
    }
}

std::string read_exact(int fd, std::size_t length) {
    std::string bytes(length, '\0');
    std::size_t offset = 0;
    while (offset < length) {
        const auto n = ::recv(fd, bytes.data() + offset, length - offset, 0);
        if (n > 0) offset += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else throw std::runtime_error("read failed");
    }
    return bytes;
}

runtime::ipc::Frame receive(int fd) {
    const auto header = read_exact(fd, runtime::ipc::header_size);
    const auto* h = reinterpret_cast<const unsigned char*>(header.data());
    const std::uint32_t length = std::uint32_t(h[0]) | (std::uint32_t(h[1]) << 8) |
        (std::uint32_t(h[2]) << 16) | (std::uint32_t(h[3]) << 24);
    require(length <= runtime::ipc::max_payload, "oversized response");
    return runtime::ipc::decode(header + read_exact(fd, length));
}

runtime::ipc::Frame request(int fd, std::uint16_t type, std::uint32_t id, const std::string& payload) {
    send_all(fd, runtime::ipc::encode({type, id, payload}));
    const auto frame = receive(fd);
    require(frame.request_id == id, "response lost request ID");
    return frame;
}

void expect_error(const runtime::ipc::Frame& frame, int code) {
    require(frame.type == 255 && frame.payload.find("\"code\":" + std::to_string(code)) != std::string::npos,
            "wrong error type/code");
}

struct Directory {
    std::filesystem::path base = std::filesystem::temp_directory_path() /
        ("rm-device-ipc-" + std::to_string(::getpid()));
    Directory() { std::filesystem::create_directory(base); }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(base, ignored); }
    std::string path(const char* name) const { return (base / name).string(); }
};

struct SnapshotGate {
    std::mutex mutex;
    std::condition_variable ready;
    bool blocked = false;
    bool entered = false;
    runtime::DeviceStateSnapshot state;
    runtime::DeviceStateSnapshot query() {
        std::unique_lock<std::mutex> lock(mutex);
        if (blocked) {
            entered = true;
            ready.notify_all();
            if (!ready.wait_for(lock, 5s, [&] { return !blocked; }))
                throw std::runtime_error("snapshot gate timed out");
        }
        return state;
    }
    void resume() {
        std::lock_guard<std::mutex> lock(mutex);
        blocked = false;
        ready.notify_all();
    }
};

struct PausedSnapshot {
    SnapshotGate& gate;
    explicit PausedSnapshot(SnapshotGate& value) : gate(value) {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.entered = false;
        gate.blocked = true;
    }
    ~PausedSnapshot() { gate.resume(); } // Unblock before IPC's destructor joins on any test failure.
    void wait_until_entered() {
        std::unique_lock<std::mutex> lock(gate.mutex);
        require(gate.ready.wait_for(lock, 3s, [&] { return gate.entered; }), "IPC query did not reach gate");
    }
};

void test_device_queries_and_invalid_requests(const Directory& directory) {
    std::mutex mutex;
    runtime::DeviceStateSnapshot state;
    state.current = runtime::DeviceState::warning;
    state.timestamp = runtime::DeviceStateTimestamp{123456ms};
    state.reason = "pressure\"\\\n";
    runtime::ServiceConfig definition;
    definition.service_name = "control_service";
    definition.heartbeat_timeout = 15s;
    runtime::ServiceStatus service;
    service.service_name = definition.service_name;
    service.state = runtime::ServiceState::running;
    service.pid = 123;
    service.start_time = runtime::Clock::now() - 2s;
    service.heartbeat_time = runtime::Clock::now();
    bool unavailable = false;
    bool service_missing = false;
    std::vector<runtime::Event> posted;
    runtime::IpcManager ipc(directory.path("query-c.sock"), directory.path("query-s.sock"),
        [&](runtime::Event event) { std::lock_guard<std::mutex> lock(mutex); posted.push_back(std::move(event)); },
        [&](const std::string& name) -> std::optional<runtime::ServiceStatus> {
            std::lock_guard<std::mutex> lock(mutex);
            if (name != service.service_name || service_missing) return std::nullopt;
            return service;
        }, {definition}, [&] {
            std::lock_guard<std::mutex> lock(mutex);
            if (unavailable) throw std::runtime_error("unavailable");
            return state;
        });
    ipc.start();
    Socket control(directory.path("query-c.sock"));
    Socket worker(directory.path("query-s.sock"));
    const auto frame = request(control.fd, 8, 101, "{}");
    require(frame.type == 8 && frame.payload ==
        R"({"state":"WARNING","timestamp":123456,"reason":"pressure\"\\\u000a"})",
        "device snapshot fields, timestamp or escaping");
    auto health = request(control.fd, 9, 102, "{}");
    require(health.type == 9 && health.payload.find("\"device_state\":" + frame.payload) != std::string::npos &&
        health.payload.find(R"("total":1,"healthy":1,"unhealthy":0,"unknown":0)") != std::string::npos &&
        health.payload.find(R"("status":"DEGRADED")") != std::string::npos,
        "health device snapshot, service counts or degradation");
    {
        std::lock_guard<std::mutex> lock(mutex);
        require(posted.empty(), "queries changed service lifecycle");
        service.state = runtime::ServiceState::failed;
    }
    health = request(control.fd, 9, 103, "{}");
    require(health.payload.find(R"("healthy":0,"unhealthy":1,"unknown":0)") != std::string::npos,
            "failed service health summary");
    const std::vector<std::string> invalid{
        R"({"service_name":"control_service"})", R"({"timestamp":1})", R"({"extra":1})",
        "[]", "{} trailing", R"({"event":"DEVICE_STATE_CHANGED"})", R"({"timestamp":01})"
    };
    std::uint32_t id = 110;
    for (const auto type : {8, 9}) {
        for (const auto& payload : invalid) expect_error(request(control.fd, type, id++, payload), 1002);
        expect_error(request(worker.fd, type, id++, "{}"), 1002);
    }
    for (const auto& payload : std::vector<std::string>{"{}", R"({"event":"OTHER"})", R"({"event":1})",
        R"({"event":"DEVICE_STATE_CHANGED","event":"DEVICE_STATE_CHANGED"})",
        R"({"event":"DEVICE_STATE_CHANGED","timestamp":1})"})
        expect_error(request(control.fd, 10, id++, payload), 1002);
    expect_error(request(worker.fd, 10, id++, R"({"event":"DEVICE_STATE_CHANGED"})"), 1002);
    expect_error(request(control.fd, 200, id++, "{}"), 1002);
    {
        std::lock_guard<std::mutex> lock(mutex);
        state.reason.assign(runtime::ipc::max_payload, 'x');
    }
    expect_error(request(control.fd, 8, id++, "{}"), 1003);
    expect_error(request(control.fd, 9, id++, "{}"), 1003);
    {
        std::lock_guard<std::mutex> lock(mutex);
        state.reason = "normal";
        service_missing = true;
    }
    expect_error(request(control.fd, 9, id++, "{}"), 1003);
    {
        std::lock_guard<std::mutex> lock(mutex);
        service_missing = false;
        unavailable = true;
    }
    expect_error(request(control.fd, 8, id++, "{}"), 1003);
    {
        std::lock_guard<std::mutex> lock(mutex);
        unavailable = false;
    }
    require(request(control.fd, 8, id++, "{}").type == 8, "connection did not survive invalid requests");
    const auto bytes = runtime::ipc::encode({8, id++, "{}"});
    send_all(control.fd, bytes.substr(0, 3));
    send_all(control.fd, bytes.substr(3) + runtime::ipc::encode({9, id++, "{}"}));
    require(receive(control.fd).type == 8 && receive(control.fd).type == 9,
            "fragmented/coalesced Phase3 frames");
    // The legacy commands still use their existing wire payloads and IDs.
    require(request(control.fd, 1, id++, R"({"service_name":"control_service"})").type == 1, "legacy START");
    require(request(control.fd, 3, id++, R"({"service_name":"control_service"})").type == 3, "legacy QUERY_STATUS");
    require(request(control.fd, 2, id++, R"({"service_name":"control_service"})").type == 2, "legacy STOP");
    send_all(worker.fd, runtime::ipc::encode({4, 0, R"({"service_name":"control_service","timestamp":1})"}));
    // Same-stream barrier: HEARTBEAT is processed before the rejected command.
    expect_error(request(worker.fd, 8, id++, "{}"), 1002);
    {
        std::lock_guard<std::mutex> lock(mutex);
        require(posted.size() == 3 && posted[0].type == runtime::EventType::start &&
            posted[1].type == runtime::EventType::stop && posted[2].type == runtime::EventType::heartbeat,
            "legacy lifecycle/one-way heartbeat events");
    }
}

void test_subscriptions_and_burst_order(const Directory& directory) {
    runtime::IpcManager::DeviceStateSink sink;
    runtime::DeviceStateManager states([&](const runtime::DeviceStateSnapshot& state) { if (sink) sink(state); });
    runtime::IpcManager ipc(directory.path("event-c.sock"), directory.path("event-s.sock"),
        [](runtime::Event) {}, [](const std::string&) -> std::optional<runtime::ServiceStatus> { return std::nullopt; },
        {}, [&] { return states.query(); });
    sink = ipc.device_state_sink();
    // An event published before subscription must not be replayed.
    require(states.handle({runtime::DeviceStateEventType::runtime_initialized, "test", "ready"}) ==
        runtime::DeviceTransitionResult::transitioned, "initialization transition");
    ipc.start();
    Socket first(directory.path("event-c.sock"));
    Socket second(directory.path("event-c.sock"));
    Socket observer(directory.path("event-c.sock"));
    const std::string subscribe = R"({"event":"DEVICE_STATE_CHANGED"})";
    require(request(first.fd, 10, 201, subscribe).type == 10, "first subscribe ACK");
    require(request(second.fd, 10, 202, subscribe).type == 10, "second subscribe ACK");
    require(request(first.fd, 10, 203, subscribe).type == 10, "duplicate subscribe ACK");
    const auto at = runtime::Clock::now(); // Equal timestamps must not coalesce distinct changes.
    require(states.handle({runtime::DeviceStateEventType::required_services_ready, "test", "running", at}) ==
        runtime::DeviceTransitionResult::transitioned, "running transition");
    require(states.handle({runtime::DeviceStateEventType::warning, "test", "warning", at}) ==
        runtime::DeviceTransitionResult::transitioned, "warning transition");
    require(states.handle({runtime::DeviceStateEventType::issue_recovered, "test", "recovered", at}) ==
        runtime::DeviceTransitionResult::transitioned, "recovery transition");
    const auto expected_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    for (const auto expected : {"RUNNING", "WARNING", "RUNNING"}) {
        for (const auto fd : {first.fd, second.fd}) {
            const auto frame = receive(fd);
            require(frame.type == 5 && frame.request_id == 0 &&
                frame.payload.find(R"("event":"DEVICE_STATE_CHANGED")") != std::string::npos &&
                frame.payload.find(std::string("\"state\":\"") + expected + "\"") != std::string::npos &&
                frame.payload.find("\"previous_state\":") != std::string::npos &&
                frame.payload.find("\"timestamp\":" + std::to_string(expected_timestamp)) != std::string::npos,
                "burst transition missing, reordered or incorrectly framed");
        }
    }
    require(states.handle({runtime::DeviceStateEventType::recovery_failed, "test", "invalid"}) ==
        runtime::DeviceTransitionResult::invalid_transition, "invalid transition accepted");
    require(request(first.fd, 8, 204, "{}").type == 8, "duplicate subscription or invalid transition emitted event");
    require(request(observer.fd, 8, 205, "{}").type == 8, "unsubscribed observer received event");
    require(::shutdown(second.fd, SHUT_RDWR) == 0, "subscriber disconnect failed");
    require(states.handle({runtime::DeviceStateEventType::warning, "test", "remaining subscriber"}) ==
        runtime::DeviceTransitionResult::transitioned, "second warning transition");
    require(receive(first.fd).type == 5, "disconnect affected another subscriber");
    // Unencodable events cause a visible gap/disconnect, never silent loss.
    auto oversized = states.query();
    oversized.reason.assign(runtime::ipc::max_payload, 'x');
    sink(oversized);
    char byte;
    require(::recv(first.fd, &byte, 1, 0) == 0, "oversized event did not disconnect affected subscriber");
    require(request(observer.fd, 8, 206, "{}").type == 8, "oversized event affected nonsubscriber");
    ipc.stop();
}

void test_running_health_requires_confirmed_heartbeats(const Directory& directory) {
    std::mutex mutex;
    runtime::ServiceConfig definition;
    definition.service_name = "control_service";
    definition.heartbeat_timeout = 15s;
    runtime::ServiceStatus service;
    service.service_name = definition.service_name;
    service.state = runtime::ServiceState::running;
    service.pid = 123;
    service.start_time = runtime::Clock::now();
    runtime::DeviceStateSnapshot state;
    state.current = runtime::DeviceState::running;
    runtime::IpcManager ipc(directory.path("health-c.sock"), directory.path("health-s.sock"),
        [](runtime::Event) {}, [&](const std::string&) -> std::optional<runtime::ServiceStatus> {
            std::lock_guard<std::mutex> lock(mutex);
            return service;
        }, {definition}, [&] { return state; });
    ipc.start();
    Socket client(directory.path("health-c.sock"));
    auto health = request(client.fd, 9, 401, "{}");
    require(health.payload.find(R"("status":"UNKNOWN")") != std::string::npos,
            "successful exec claimed confirmed device health before first heartbeat");
    {
        std::lock_guard<std::mutex> lock(mutex);
        service.heartbeat_time = runtime::Clock::now();
    }
    health = request(client.fd, 9, 402, "{}");
    require(health.payload.find(R"("status":"HEALTHY")") != std::string::npos, "confirmed heartbeat not healthy");
    {
        std::lock_guard<std::mutex> lock(mutex);
        service.start_time = runtime::Clock::now() - 30s;
        service.heartbeat_time = runtime::Clock::now() - 20s;
    }
    health = request(client.fd, 9, 403, "{}");
    require(health.payload.find(R"("status":"UNHEALTHY")") != std::string::npos,
            "expired service heartbeat hidden by RUNNING device state");
    require(request(client.fd, 8, 404, "{}").payload.find(R"("state":"RUNNING")") != std::string::npos,
            "read-only health query changed device lifecycle state");
}

void test_producer_overflow_disconnect_and_resubscribe(const Directory& directory) {
    for (const bool byte_limit : {false, true}) {
        SnapshotGate gate;
        runtime::IpcManager ipc(directory.path("overflow-c.sock"), directory.path("overflow-s.sock"),
            [](runtime::Event) {}, [](const std::string&) -> std::optional<runtime::ServiceStatus> { return std::nullopt; },
            {}, [&] { return gate.query(); });
        const auto sink = ipc.device_state_sink();
        ipc.start();
        Socket subscriber(directory.path("overflow-c.sock"));
        Socket observer(directory.path("overflow-c.sock"));
        const std::string subscribe = R"({"event":"DEVICE_STATE_CHANGED"})";
        require(request(subscriber.fd, 10, 501, subscribe).type == 10, "overflow subscriber ACK");
        PausedSnapshot pause(gate);
        send_all(subscriber.fd, runtime::ipc::encode({8, 502, "{}"}));
        pause.wait_until_entered(); // The epoll consumer cannot drain the producer queue.
        auto change = runtime::DeviceStateSnapshot{};
        change.current = runtime::DeviceState::warning;
        change.reason = byte_limit ? std::string(1024, 'x') : "warning";
        for (unsigned i = 0; i < (byte_limit ? 300u : 1025u); ++i) sink(change);
        gate.resume();
        require(receive(subscriber.fd).type == 8, "barrier query response missing");
        char byte;
        require(::recv(subscriber.fd, &byte, 1, 0) == 0, "normal-sized producer overflow did not disconnect subscriber");
        require(request(observer.fd, 8, 503, "{}").type == 8, "overflow disconnected an unsubscribed peer");
        Socket replacement(directory.path("overflow-c.sock"));
        require(request(replacement.fd, 10, 504, subscribe).type == 10, "resubscription ACK");
        change.reason = "after reconnect";
        sink(change);
        const auto frame = receive(replacement.fd);
        require(frame.type == 5 && frame.payload.find("after reconnect") != std::string::npos,
                "gap marker incorrectly disconnected a later subscription");
    }
}

void test_unread_client_output_bound(const Directory& directory) {
    SnapshotGate gate;
    runtime::IpcManager ipc(directory.path("slow-c.sock"), directory.path("slow-s.sock"),
        [](runtime::Event) {}, [](const std::string&) -> std::optional<runtime::ServiceStatus> { return std::nullopt; },
        {}, [&] { return gate.query(); });
    ipc.start();
    Socket slow(directory.path("slow-c.sock"));
    Socket observer(directory.path("slow-c.sock"));
    PausedSnapshot pause(gate);
    send_all(slow.fd, runtime::ipc::encode({8, 601, "{}"}));
    pause.wait_until_entered();
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.state.reason.assign(60000, 'x'); // Valid individual responses below 64 KiB.
    }
    std::string requests;
    for (unsigned i = 0; i < 5; ++i) requests += runtime::ipc::encode({8, 610 + i, "{}"});
    send_all(slow.fd, requests); // These requests all wait in the Unix socket while epoll is blocked.
    gate.resume();
    require(receive(slow.fd).request_id == 601, "slow-client barrier query missing");
    char bytes[4096];
    bool disconnected = false;
    for (;;) {
        const auto count = ::recv(slow.fd, bytes, sizeof(bytes), 0);
        if (count == 0) { disconnected = true; break; }
        if (count < 0) { if (errno == EINTR) continue; break; }
    }
    require(disconnected, "bounded output did not disconnect an unread burst client");
    require(request(observer.fd, 8, 620, "{}").type == 8, "one client's output overflow affected another client");
}

void test_optional_device_callback_and_sink_lifetime(const Directory& directory) {
    runtime::IpcManager::DeviceStateSink sink;
    {
        runtime::IpcManager ipc(directory.path("legacy-c.sock"), directory.path("legacy-s.sock"),
            [](runtime::Event) {}, [](const std::string&) -> std::optional<runtime::ServiceStatus> { return std::nullopt; });
        sink = ipc.device_state_sink();
        ipc.start();
        Socket client(directory.path("legacy-c.sock"));
        expect_error(request(client.fd, 8, 301, "{}"), 1003);
        expect_error(request(client.fd, 9, 302, "{}"), 1003);
        expect_error(request(client.fd, 10, 303, R"({"event":"DEVICE_STATE_CHANGED"})"), 1003);
        require(request(client.fd, 7, 304, "{}").payload == R"({"services":[]})", "legacy constructor compatibility");
    }
    sink(runtime::DeviceStateSnapshot{}); // Must not access the destroyed manager.
}
} // namespace

int main() {
    try {
        Directory directory;
        test_device_queries_and_invalid_requests(directory);
        test_subscriptions_and_burst_order(directory);
        test_optional_device_callback_and_sink_lifetime(directory);
        test_running_health_requires_confirmed_heartbeats(directory);
        test_producer_overflow_disconnect_and_resubscribe(directory);
        test_unread_client_output_bound(directory);
        std::cout << "Phase3 device IPC tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Phase3 device IPC tests failed: " << error.what() << '\n';
        return 1;
    }
}
