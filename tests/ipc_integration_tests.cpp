#include "../src/ipc/frame.hpp"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
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
        std::ofstream(config) << "{\"service_name\":\"fake_service\",\"executable\":\""
            << argv[2] << "\",\"arguments\":[\"" << service << "\",\"" << pidfile.string()
            << "\"],\"heartbeat_timeout\":15,\"restart_policy\":\"never\"}";
        runtime.pid = ::fork();
        if (runtime.pid == 0) {
            ::execl(argv[1], argv[1], config.c_str(), control.c_str(), service.c_str(), nullptr);
            _exit(127);
        }
        require(runtime.pid > 0, "fork failed");
        int fd = -1;
        until([&] { fd = connect_to(control); return fd >= 0; }, 5s, "runtime IPC did not start");
        const std::string valid = R"({"service_name":"fake_service"})";
        auto frame = request(fd, 3, 0x11223344, R"({"service_name":)");
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

        const int service_fd = connect_to(service);
        require(service_fd >= 0, "service channel connection failed");
        write_all(service_fd, runtime::ipc::encode({4, 17,
            R"({"service_name":"fake_service","timestamp":1})"}) +
            runtime::ipc::encode({3, 18, valid}));
        // ERROR proves the preceding heartbeat on this connection was parsed.
        const auto service_error = receive_frame(service_fd);
        require(service_error.type == 255 && service_error.request_id == 18,
                "service channel framing or direction check failed");

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
        frame = request(fd, 1, 7, valid);
        require(frame.type == 1 && frame.request_id == 7, "second START response");
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
        }, 35s, "three missed heartbeats did not fail service");
        until([&] {
            return ::kill(new_pid, 0) < 0 && errno == ESRCH;
        }, 5s, "failed service process was not reaped");
        frame = request(fd, 1, 19, valid);
        require(frame.type == 1 && frame.request_id == 19,
                "third START response");
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
        std::cout << "Phase 1 IPC integration passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Phase 1 IPC integration failed: " << error.what() << '\n';
        runtime.terminate();
        std::filesystem::remove_all(base);
        return 1;
    }
    runtime.terminate();
    std::filesystem::remove_all(base);
    return 0;
}
