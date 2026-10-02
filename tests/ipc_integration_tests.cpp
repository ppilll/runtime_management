#include "../src/ipc/frame.hpp"
#include "../src/ipc/ipc_manager.hpp"
#include "runtime/config_manager.hpp"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Child {
    pid_t pid = -1;
    void terminate() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 30; ++i) {
                if (::waitpid(pid, nullptr, WNOHANG) == pid) { pid = -1; return; }
                std::this_thread::sleep_for(100ms);
            }
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            pid = -1;
        }
    }
    ~Child() { terminate(); }
};

int connect_to(const std::string& path) {
    sockaddr_un address{};
    require(path.size() < sizeof(address.sun_path), "test socket path too long");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket failed");
    timeval timeout{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return fd;
    ::close(fd);
    return -1;
}

void write_all(int fd, const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n > 0) sent += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else throw std::runtime_error("send failed");
    }
}

std::string read_exact(int fd, std::size_t size) {
    std::string bytes(size, '\0');
    std::size_t got = 0;
    while (got < size) {
        const auto n = ::recv(fd, bytes.data() + got, size - got, 0);
        if (n > 0) got += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else throw std::runtime_error("response read failed");
    }
    return bytes;
}

void expect_closed(int fd, const char* message) {
    char byte = 0;
    const auto n = ::recv(fd, &byte, 1, 0);
    require(n == 0, message);
}

runtime::ipc::Frame receive_frame(int fd) {
    const auto header = read_exact(fd, 10);
    const auto* h = reinterpret_cast<const unsigned char*>(header.data());
    const auto length = std::uint32_t(h[0]) | (std::uint32_t(h[1]) << 8) |
        (std::uint32_t(h[2]) << 16) | (std::uint32_t(h[3]) << 24);
    require(length <= runtime::ipc::max_payload, "response length invalid");
    return runtime::ipc::decode(header + read_exact(fd, length));
}

runtime::ipc::Frame request(int fd, std::uint16_t type, std::uint32_t id, const std::string& payload,
                            bool fragmented = false) {
    const auto bytes = runtime::ipc::encode({type, id, payload});
    require(static_cast<unsigned char>(bytes[0]) == payload.size() &&
            static_cast<unsigned char>(bytes[4]) == type &&
            static_cast<unsigned char>(bytes[6]) == (id & 0xff), "little-endian request header");
    if (fragmented) {
        write_all(fd, bytes.substr(0, 3));
        std::this_thread::sleep_for(20ms);
        write_all(fd, bytes.substr(3));
    } else write_all(fd, bytes);
    return receive_frame(fd);
}

template <typename Predicate>
void until(Predicate predicate, std::chrono::seconds limit, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error(message);
}

int read_pid(const std::filesystem::path& path) {
    std::ifstream file(path);
    int pid = -1;
    file >> pid;
    return pid;
}

void test_config_extensions(const std::filesystem::path& path) {
    auto load = [&](const std::string& json) {
        { std::ofstream file(path); file << json; }
        return runtime::ConfigManager::load_file(path.string());
    };
    const auto defaults = load(R"({"service_name":"legacy","executable":"/bin/true"})");
    require(defaults[0].environment.empty() && defaults[0].working_directory.empty() &&
            defaults[0].shutdown_timeout == 2s, "Phase 2 config defaults");
    const auto extended = load(R"({"services":[{"service_name":"first","executable":"/bin/true","environment":["MODE=old","EMPTY=","TOKEN=a=b","MODE=new"],"working_directory":"/tmp/work dir","shutdown_timeout":1},{"service_name":"second","executable":"/bin/true","shutdown_timeout":3600}]})");
    require(extended.size() == 2 && extended[0].service_name == "first" &&
            extended[1].service_name == "second", "config declaration order");
    require(extended[0].environment == std::vector<std::string>{"MODE=old", "EMPTY=", "TOKEN=a=b", "MODE=new"} &&
            extended[0].working_directory == "/tmp/work dir" && extended[0].shutdown_timeout == 1s &&
            extended[1].shutdown_timeout == 3600s, "Phase 2 config values and boundaries");
    const std::vector<std::string> invalid_fields{
        R"("environment":{})", R"("environment":"MODE=test")", R"("environment":null)",
        R"("environment":[1])", R"("environment":[""])", R"("environment":["MODE"])",
        R"("environment":["=value"])", R"("environment":["MODE=x\u0000y"])",
        R"("working_directory":1)", R"("working_directory":null)",
        R"("working_directory":"/tmp/\u0000bad")", R"("shutdown_timeout":0)",
        R"("shutdown_timeout":-1)", R"("shutdown_timeout":3601)",
        R"("shutdown_timeout":1.5)", R"("shutdown_timeout":"2")",
        R"("shutdown_timeout":true)", R"("shutdown_timeout":null)",
        R"("shutdown_timeout":9223372036854775808)"
    };
    for (const auto& fields : invalid_fields) {
        bool rejected = false;
        try { load("{\"service_name\":\"bad\",\"executable\":\"/bin/true\"," + fields + "}"); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "invalid Phase 2 config was accepted");
    }
}

void test_ipc_extension_boundaries(const std::filesystem::path& base) {
    struct Snapshot {
        std::mutex mutex;
        runtime::ServiceStatus status;
        std::vector<runtime::Event> events;
        unsigned queries = 0;
    } snapshot;
    const std::string name = "quoted\"\\\nservice";
    snapshot.status.service_name = name;
    snapshot.status.state = runtime::ServiceState::running;
    snapshot.status.pid = 123;
    snapshot.status.start_time = runtime::Clock::now() - 30s;
    snapshot.status.heartbeat_time = runtime::Clock::now() - 20s;
    runtime::ServiceConfig definition;
    definition.service_name = name;
    definition.executable = "/bin/true";
    definition.heartbeat_timeout = 15s;
    runtime::IpcManager ipc((base / "mock-c.sock").string(), (base / "mock-s.sock").string(),
        [&](runtime::Event event) {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            snapshot.events.push_back(std::move(event));
        },
        [&](const std::string& service_name) -> std::optional<runtime::ServiceStatus> {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            ++snapshot.queries;
            if (service_name != name) return std::nullopt;
            return snapshot.status;
        }, {definition});
    ipc.start();
    int fd = connect_to((base / "mock-c.sock").string());
    require(fd >= 0, "mock IPC connection failed");
    try {
        const std::string payload = R"({"service_name":"quoted\"\\\nservice"})";
        auto frame = request(fd, 7, 50, "{}");
        require(frame.payload == R"({"services":[{"service_name":"quoted\"\\\nservice","state":"RUNNING","pid":123,"health_status":"UNHEALTHY"}]})",
                "service list escaping or stale heartbeat health");
        {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            snapshot.status.start_time = runtime::Clock::now();
            snapshot.status.heartbeat_time.reset();
        }
        frame = request(fd, 7, 51, "{}");
        require(frame.payload.find("\"health_status\":\"UNKNOWN\"") != std::string::npos,
                "exec incorrectly counted as a healthy heartbeat");
        {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            snapshot.status.start_time = runtime::Clock::now() - 30s;
        }
        frame = request(fd, 7, 52, "{}");
        require(frame.payload.find("\"health_status\":\"UNHEALTHY\"") != std::string::npos,
                "missing first heartbeat after timeout not unhealthy");
        frame = request(fd, 6, 53, payload);
        require(frame.type == 6, "mock restart not acknowledged");
        frame = request(fd, 6, 54, payload);
        require(frame.type == 6, "pending duplicate restart not acknowledged");
        {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            require(snapshot.events.size() == 1 && snapshot.events[0].type == runtime::EventType::stop,
                    "pending restart was duplicated or START submitted before stop");
            snapshot.status.state = runtime::ServiceState::stopped;
            // A state label alone is insufficient: the old PID still exists.
        }
        frame = request(fd, 7, 55, "{}");
        {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            require(snapshot.events.size() == 1, "restart did not wait for PID clearance");
            snapshot.status.pid = -1;
        }
        ::close(fd); fd = -1; // Accepted restart must survive control-client disconnect.
        until([&] {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            return snapshot.events.size() == 2;
        }, 3s, "restart did not submit START after old PID cleared");
        {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            require(snapshot.events[1].type == runtime::EventType::start &&
                    snapshot.events[1].service_name == name, "restart event ordering or service name");
            snapshot.status.state = runtime::ServiceState::running;
            snapshot.status.pid = 456;
        }
        fd = connect_to((base / "mock-c.sock").string());
        require(fd >= 0, "mock IPC reconnect failed");
        frame = request(fd, 6, 56, payload);
        require(frame.type == 6, "second mock restart failed");
        frame = request(fd, 2, 57, payload);
        require(frame.type == 2, "STOP cancellation failed");
        unsigned queries;
        {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            require(snapshot.events.size() == 4, "unexpected events before cancellation check");
            snapshot.status.state = runtime::ServiceState::stopped;
            snapshot.status.pid = -1;
            queries = snapshot.queries;
        }
        // Wake and observe several subsequent server iterations without assuming
        // which thread resumes first after an acknowledgement is received.
        until([&] {
            frame = request(fd, 7, 58, "{}");
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            return snapshot.queries >= queries + 3;
        }, 3s, "mock IPC did not continue serving");
        {
            std::lock_guard<std::mutex> lock(snapshot.mutex);
            require(snapshot.events.size() == 4, "STOP did not cancel pending restart");
        }
        ::close(fd); fd = -1;
        ipc.stop();
    } catch (...) {
        if (fd >= 0) ::close(fd);
        throw;
    }
}

void test_service_list_limits(const std::filesystem::path& base) {
    auto check = [&](std::vector<runtime::ServiceConfig> definitions, bool unavailable,
                      const std::string& expected) {
        runtime::IpcManager ipc((base / "list-c.sock").string(), (base / "list-s.sock").string(),
            [](runtime::Event) {},
            [](const std::string& name) -> std::optional<runtime::ServiceStatus> {
                if (name == "missing") return std::nullopt;
                runtime::ServiceStatus status;
                status.service_name = name;
                return status;
            }, std::move(definitions));
        ipc.start();
        const int fd = connect_to((base / "list-c.sock").string());
        require(fd >= 0, "service list limit connection failed");
        try {
            const auto frame = request(fd, 7, 59, "{}");
            require(frame.request_id == 59, "service list limit request ID");
            if (unavailable) {
                require(frame.type == 255 && frame.payload.find("\"code\":1003") != std::string::npos,
                        "unavailable or oversized service list not rejected");
                const auto followup = request(fd, 3, 60, R"({"service_name":"small"})");
                require(followup.type == 3, "server failed after service list error");
            } else require(frame.type == 7 && frame.payload == expected, "empty or ordered service list");
            ::close(fd);
            ipc.stop();
        } catch (...) { ::close(fd); throw; }
    };
    check({}, false, R"({"services":[]})");
    runtime::ServiceConfig first, second;
    first.service_name = "z";
    first.executable = "/bin/true";
    second.service_name = "a";
    second.executable = "/bin/true";
    check({first, second}, false,
        R"({"services":[{"service_name":"z","state":"CREATED","pid":-1,"health_status":"UNKNOWN"},{"service_name":"a","state":"CREATED","pid":-1,"health_status":"UNKNOWN"}]})");
    first.service_name.assign(runtime::ipc::max_payload, 'x');
    check({first}, true, "");
    first.service_name = "missing";
    check({first}, true, "");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    char name[] = "/tmp/p1ipcXXXXXX";
    const char* dir = ::mkdtemp(name);
    if (!dir) return 2;
    const std::filesystem::path base(dir);
    const auto control = (base / "c.sock").string();
    const auto service = (base / "s.sock").string();
    const auto pidfile = base / "fake.pid";
    const auto config = base / "config.json";
    Child runtime;
    try {
        test_config_extensions(config);
        test_ipc_extension_boundaries(base);
        test_service_list_limits(base);
        std::ofstream(config) << "{\"service_name\":\"fake_service\",\"executable\":\""
            << argv[2] << "\",\"arguments\":[\"" << service << "\",\"" << pidfile.string()
            << "\"],\"heartbeat_timeout\":15,\"restart_policy\":\"never\","
            << "\"environment\":[\"IPC_TEST=phase2\"],\"working_directory\":\"" << base.string()
            << "\",\"shutdown_timeout\":2}";
        runtime.pid = ::fork();
        if (runtime.pid == 0) {
            ::execl(argv[1], argv[1], config.c_str(), control.c_str(), service.c_str(), nullptr);
            _exit(127);
        }
        require(runtime.pid > 0, "fork failed");
        int fd = -1;
        until([&] { fd = connect_to(control); return fd >= 0; }, 5s, "runtime IPC did not start");
        const std::string valid = R"({"service_name":"fake_service"})";
        auto frame = request(fd, 7, 0xaabbccdd, "{}", true);
        require(frame.type == 7 && frame.request_id == 0xaabbccdd &&
                frame.payload == R"({"services":[{"service_name":"fake_service","state":"CREATED","pid":-1,"health_status":"UNKNOWN"}]})",
                "GET_SERVICE_LIST initial snapshot or request ID");
        frame = request(fd, 7, 30, valid);
        require(frame.type == 255 && frame.request_id == 30 &&
                frame.payload.find("\"code\":1002") != std::string::npos,
                "GET_SERVICE_LIST accepted unexpected fields");
        frame = request(fd, 6, 31, "{}");
        require(frame.type == 255 && frame.request_id == 31, "RESTART_SERVICE accepted missing name");
        frame = request(fd, 6, 32, R"({"service_name":"unknown"})");
        require(frame.type == 255 && frame.request_id == 32 &&
                frame.payload.find("\"code\":1001") != std::string::npos, "unknown restart service error");
        frame = request(fd, 6, 33, R"({"service_name":"fake_service","timestamp":1})");
        require(frame.type == 255 && frame.request_id == 33, "RESTART_SERVICE accepted extra fields");
        frame = request(fd, 3, 0x11223344, R"({"service_name":)");
        require(frame.type == 255 && frame.request_id == 0x11223344 &&
                frame.payload.find("\"code\":1002") != std::string::npos, "malformed JSON error");
        frame = request(fd, 4, 11, R"({"service_name":"fake_service","timestamp":1})");
        require(frame.type == 255 && frame.request_id == 11, "HEARTBEAT accepted on control channel");
        frame = request(fd, 1, 0x12345678, valid, true);
        require(frame.type == 1 && frame.request_id == 0x12345678 &&
                frame.payload == R"({"result":"OK","state":"STARTING"})", "START response or request ID");
        until([&] {
            frame = request(fd, 3, 2, valid);
            return frame.payload.find("\"state\":\"RUNNING\"") != std::string::npos;
        }, 5s, "service did not reach RUNNING");
        require(frame.type == 3 && frame.request_id == 2 &&
                frame.payload.find("\"restart_count\":0") != std::string::npos,
                "QUERY_STATUS response");
        write_all(fd, runtime::ipc::encode({3, 101, valid}) +
            runtime::ipc::encode({3, 102, valid}));
        const auto first = receive_frame(fd);
        const auto second = receive_frame(fd);
        require(first.type == 3 && first.request_id == 101 &&
                second.type == 3 && second.request_id == 102,
                "coalesced frames or request IDs were not preserved");
        const int half_closed = connect_to(control);
        require(half_closed >= 0, "half-close test connection failed");
        write_all(half_closed, runtime::ipc::encode({3, 103, valid}));
        require(::shutdown(half_closed, SHUT_WR) == 0, "half-close failed");
        const auto half_response = receive_frame(half_closed);
        require(half_response.type == 3 && half_response.request_id == 103,
                "control response lost after client write half-close");
        ::close(half_closed);
        const int oversized = connect_to(control);
        require(oversized >= 0, "oversized frame test connection failed");
        std::string bad_header(10, '\0');
        const auto bad_length = runtime::ipc::max_payload + 1;
        for (unsigned i = 0; i < 4; ++i)
            bad_header[i] = static_cast<char>(bad_length >> (8 * i));
        write_all(oversized, bad_header);
        expect_closed(oversized, "oversized IPC frame was not rejected");
        ::close(oversized);
        const int incomplete = connect_to(control);
        require(incomplete >= 0, "incomplete frame test connection failed");
        write_all(incomplete, runtime::ipc::encode({3, 104, valid}).substr(0, 5));
        require(::shutdown(incomplete, SHUT_WR) == 0, "incomplete frame half-close failed");
        expect_closed(incomplete, "incomplete IPC frame was not rejected");
        ::close(incomplete);
        until([&] {
            frame = request(fd, 3, 3, valid);
            return frame.payload.find("\"heartbeat_time\":0") == std::string::npos;
        }, 8s, "fake service heartbeat not received");
        require(frame.payload.find("\"heartbeat_time\":") != std::string::npos,
                "heartbeat field missing");
        frame = request(fd, 7, 34, "{}");
        require(frame.type == 7 && frame.request_id == 34 &&
                frame.payload == "{\"services\":[{\"service_name\":\"fake_service\",\"state\":\"RUNNING\",\"pid\":" +
                    std::to_string(read_pid(pidfile)) + ",\"health_status\":\"HEALTHY\"}]}",
                "GET_SERVICE_LIST running PID/health snapshot");

        const int service_fd = connect_to(service);
        require(service_fd >= 0, "service channel connection failed");
        write_all(service_fd, runtime::ipc::encode({4, 17,
            R"({"service_name":"fake_service","timestamp":1})"}) +
            runtime::ipc::encode({3, 18, valid}));
        // ERROR proves the preceding heartbeat on this connection was parsed.
        const auto service_error = receive_frame(service_fd);
        require(service_error.type == 255 && service_error.request_id == 18,
                "service channel framing or direction check failed");
        write_all(service_fd, runtime::ipc::encode({7, 35, "{}"}) +
            runtime::ipc::encode({6, 36, valid}));
        const auto rejected_list = receive_frame(service_fd);
        const auto rejected_restart = receive_frame(service_fd);
        require(rejected_list.type == 255 && rejected_list.request_id == 35 &&
                rejected_restart.type == 255 && rejected_restart.request_id == 36,
                "Phase 2 control commands accepted on service channel");

        const int before_restart = read_pid(pidfile);
        write_all(fd, runtime::ipc::encode({6, 37, valid}) + runtime::ipc::encode({6, 38, valid}));
        const auto restart_ack = receive_frame(fd);
        const auto duplicate_ack = receive_frame(fd);
        require(restart_ack.type == 6 && restart_ack.request_id == 37 &&
                restart_ack.payload == R"({"result":"OK"})" &&
                duplicate_ack.type == 6 && duplicate_ack.request_id == 38 &&
                duplicate_ack.payload == R"({"result":"OK"})", "RESTART_SERVICE acknowledgements");
        const auto restart_notification = receive_frame(service_fd);
        require(restart_notification.type == 5 && restart_notification.request_id == 0 &&
                restart_notification.payload == R"({"event":"SERVICE_STOP","service_name":"fake_service"})",
                "RESTART_SERVICE stop notification");
        until([&] {
            frame = request(fd, 7, 39, "{}");
            const int replacement = read_pid(pidfile);
            return replacement > 0 && replacement != before_restart &&
                frame.payload.find("\"pid\":" + std::to_string(replacement)) != std::string::npos &&
                frame.payload.find("\"state\":\"RUNNING\"") != std::string::npos;
        }, 6s, "manual restart did not replace the process under never policy");
        require(::kill(before_restart, 0) < 0 && errno == ESRCH, "restart launched before old child was reaped");

        frame = request(fd, 3, 4, R"({"service_name":"unknown"})");
        require(frame.type == 255 && frame.request_id == 4 &&
                frame.payload.find("\"code\":1001") != std::string::npos, "unknown service error");
        frame = request(fd, 2, 5, valid);
        require(frame.type == 2 && frame.request_id == 5 && frame.payload == R"({"result":"OK"})",
                "STOP response");
        const auto notification = receive_frame(service_fd);
        require(notification.type == 5 && notification.request_id == 0 &&
                notification.payload == R"({"event":"SERVICE_STOP","service_name":"fake_service"})",
                "SERVICE_STOP EVENT missing on service channel");
        ::close(service_fd);
        until([&] {
            frame = request(fd, 3, 6, valid);
            return frame.payload.find("\"state\":\"STOPPED\"") != std::string::npos;
        }, 5s, "service did not stop");

        const int old_pid = read_pid(pidfile);
        frame = request(fd, 7, 40, "{}");
        require(frame.payload == R"({"services":[{"service_name":"fake_service","state":"STOPPED","pid":-1,"health_status":"UNKNOWN"}]})",
                "GET_SERVICE_LIST stopped snapshot");
        frame = request(fd, 6, 7, valid);
        require(frame.type == 6 && frame.request_id == 7, "stopped RESTART_SERVICE response");
        until([&] {
            frame = request(fd, 3, 8, valid);
            return frame.payload.find("\"state\":\"RUNNING\"") != std::string::npos &&
                read_pid(pidfile) > 0 && read_pid(pidfile) != old_pid;
        }, 5s, "second fake service did not start");
        const int new_pid = read_pid(pidfile);
        until([&] {
            frame = request(fd, 3, 9, valid);
            return frame.payload.find("\"heartbeat_time\":0") == std::string::npos;
        }, 8s, "second heartbeat not received");
        require(::kill(new_pid, SIGUSR1) == 0, "could not pause fake service heartbeats");
        until([&] {
            frame = request(fd, 3, 10, valid);
            return frame.payload.find("\"state\":\"FAILED\"") != std::string::npos;
        }, 35s, "heartbeat timeout did not fail service");
        until([&] {
            return ::kill(new_pid, 0) < 0 && errno == ESRCH;
        }, 5s, "failed service process was not reaped");
        frame = request(fd, 7, 41, "{}");
        require(frame.payload.find("\"state\":\"FAILED\"") != std::string::npos &&
                frame.payload.find("\"health_status\":\"UNHEALTHY\"") != std::string::npos,
                "GET_SERVICE_LIST failed health snapshot");
        frame = request(fd, 6, 19, valid);
        require(frame.type == 6 && frame.request_id == 19,
                "failed RESTART_SERVICE response");
        until([&] {
            frame = request(fd, 3, 20, valid);
            return frame.payload.find("\"state\":\"RUNNING\"") != std::string::npos &&
                read_pid(pidfile) > 0 && read_pid(pidfile) != new_pid;
        }, 5s, "third fake service did not start");
        const int stubborn_pid = read_pid(pidfile);
        require(::kill(stubborn_pid, SIGSTOP) == 0, "could not suspend fake service");
        frame = request(fd, 2, 21, valid);
        require(frame.type == 2 && frame.request_id == 21, "stubborn STOP response");
        until([&] {
            frame = request(fd, 3, 22, valid);
            return frame.payload.find("\"state\":\"STOPPED\"") != std::string::npos;
        }, 6s, "SIGTERM-immune service did not reach STOPPED");
        ::close(fd);
        std::cout << "Phase 1/2 IPC and configuration integration passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Phase 1/2 IPC integration failed: " << error.what() << '\n';
        runtime.terminate();
        std::filesystem::remove_all(base);
        return 1;
    }
    runtime.terminate();
    std::filesystem::remove_all(base);
    return 0;
}
