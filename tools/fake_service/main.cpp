#include "../../src/ipc/frame.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t running = 1;
volatile std::sig_atomic_t heartbeats_enabled = 1;
void stop(int) { running = 0; }
void pause_heartbeats(int) { heartbeats_enabled = 0; }

int connect_to(const std::string& path) {
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) return -1;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return fd;
    ::close(fd);
    return -1;
}

bool send_all(int fd, const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n > 0) sent += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool receive_stop_event(int& fd, std::string& input) {
    if (fd < 0) return false;
    char buffer[4096];
    for (;;) {
        const auto n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0) input.append(buffer, static_cast<std::size_t>(n));
        else if (n == 0) {
            ::close(fd); fd = -1; input.clear(); return false;
        } else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else {
            ::close(fd); fd = -1; input.clear(); return false;
        }
    }
    while (input.size() >= runtime::ipc::header_size) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
        const auto length = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
            (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
        if (length > runtime::ipc::max_payload) {
            ::close(fd); fd = -1; input.clear(); return false;
        }
        const auto frame_size = runtime::ipc::header_size + length;
        if (input.size() < frame_size) break;
        const auto frame = runtime::ipc::decode(input.substr(0, frame_size));
        input.erase(0, frame_size);
        if (frame.type == static_cast<std::uint16_t>(runtime::ipc::Type::event) &&
            frame.payload == R"({"event":"SERVICE_STOP","service_name":"fake_service"})")
            return true;
    }
    return false;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: fake_service <service_socket> [pid_file]\n";
        return 2;
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    std::signal(SIGUSR1, pause_heartbeats); // Test hook for heartbeat timeout.
    if (argc == 3) std::ofstream(argv[2]) << ::getpid() << '\n';
    int fd = -1;
    std::string input;
    std::uint32_t id = 1;
    while (running) {
        if (heartbeats_enabled) {
            if (fd < 0) fd = connect_to(argv[1]);
            if (fd >= 0) {
                const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                const auto bytes = runtime::ipc::encode({static_cast<std::uint16_t>(runtime::ipc::Type::heartbeat), id++,
                    "{\"service_name\":\"fake_service\",\"timestamp\":" + std::to_string(timestamp) + "}"});
                if (!send_all(fd, bytes)) { ::close(fd); fd = -1; input.clear(); }
            }
        }
        for (int i = 0; i < 50 && running; ++i) {
            if (receive_stop_event(fd, input)) { running = 0; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (fd >= 0) ::close(fd);
    return 0;
}
