#include "ipc_manager.hpp"
#include "frame.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace runtime {
namespace {
struct Client {
    bool service;
    std::string input;
    std::string output;
    std::string service_name;
    bool input_closed = false;
};

void close_fd(int& fd) {
    if (fd >= 0) { ::close(fd); fd = -1; }
}

int listen_on(const std::string& path) {
    sockaddr_un address{};
    if (path.empty() || path.size() >= sizeof(address.sun_path))
        throw std::invalid_argument("invalid Unix socket path");
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(fd, 16) < 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "bind/listen " + path);
    }
    return fd;
}

bool valid_utf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        const unsigned count = first >= 0xc2 && first <= 0xdf ? 1 :
            first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (count == 0 || i + count > text.size()) return false;
        unsigned cp = first & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
        for (unsigned n = 0; n < count; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
            (count == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

// The Phase 1 command payloads are flat JSON objects with string and integer values.
class Payload {
public:
    explicit Payload(const std::string& text) : text_(text) {
        if (!valid_utf8(text_)) fail();
        space(); take('{');
        if (!next('}')) {
            do {
                const auto key = string();
                space(); take(':');
                space();
                std::string value;
                bool number = false;
                if (peek() == '"') value = string();
                else {
                    number = true;
                    const auto begin = pos_;
                    if (peek() == '-') ++pos_;
                    if (pos_ == text_.size() || text_[pos_] < '0' || text_[pos_] > '9') fail();
                    if (text_[pos_] == '0') ++pos_;
                    else while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
                    value = text_.substr(begin, pos_ - begin);
                }
                if (key != "service_name" && key != "timestamp") fail();
                if (!values_.emplace(key, std::make_pair(value, number)).second) fail();
                if (next('}')) break;
                take(',');
            } while (true);
        }
        space();
        if (pos_ != text_.size()) fail();
    }
    std::string service_name() const {
        auto it = values_.find("service_name");
        if (it == values_.end() || it->second.second || it->second.first.empty()) fail();
        return it->second.first;
    }
    void heartbeat_timestamp() const {
        auto it = values_.find("timestamp");
        if (values_.size() != 2 || it == values_.end() || !it->second.second) fail();
        try { (void)std::stoll(it->second.first); }
        catch (const std::exception&) { fail(); }
    }
    void command_fields() const { if (values_.size() != 1) fail(); }
private:
    [[noreturn]] static void fail() { throw std::invalid_argument("invalid IPC JSON payload"); }
    void space() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                    text_[pos_] == '\t' || text_[pos_] == '\r')) ++pos_; }
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    bool next(char c) { space(); if (peek() == c) { ++pos_; return true; } return false; }
    void take(char c) { if (!next(c)) fail(); }
    unsigned hex4() {
        if (pos_ + 4 > text_.size()) fail();
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            result <<= 4;
            if (c >= '0' && c <= '9') result |= c - '0';
            else if (c >= 'a' && c <= 'f') result |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') result |= c - 'A' + 10;
            else fail();
        }
        return result;
    }
    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    std::string string() {
        space(); take('"');
        std::string out;
        while (pos_ < text_.size()) {
            const unsigned char c = text_[pos_++];
            if (c == '"') return out;
            if (c < 0x20) fail();
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (pos_ == text_.size()) fail();
            const char escaped = text_[pos_++];
            switch (escaped) {
            case '"': case '\\': case '/': out.push_back(escaped); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (pos_ + 2 > text_.size() || text_[pos_++] != '\\' || text_[pos_++] != 'u') fail();
                    const unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) fail();
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) fail();
                append_utf8(out, cp);
                break;
            }
            default: fail();
            }
        }
        fail();
    }
    const std::string& text_;
    std::size_t pos_ = 0;
    std::unordered_map<std::string, std::pair<std::string, bool>> values_;
};

std::string json_escape(const std::string& value) {
    std::string out;
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]);
        } else out.push_back(static_cast<char>(c));
    }
    return out;
}

std::int64_t epoch_seconds(Clock::time_point point) {
    const auto system_point = std::chrono::system_clock::now() + (point - Clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(system_point.time_since_epoch()).count();
}

} // namespace

IpcManager::IpcManager(std::string control_path, std::string service_path, Post post, Query query)
    : control_path_(std::move(control_path)), service_path_(std::move(service_path)),
      post_(std::move(post)), query_(std::move(query)) {
    if (!post_ || !query_ || control_path_ == service_path_)
        throw std::invalid_argument("IPC requires distinct paths and runtime callbacks");
}

IpcManager::~IpcManager() { stop(); }

void IpcManager::start() {
    if (running_.exchange(true)) throw std::logic_error("IPC already running");
    try {
        control_fd_ = listen_on(control_path_);
        service_fd_ = listen_on(service_path_);
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");
        for (int fd : {control_fd_, service_fd_}) {
            epoll_event event{};
            event.events = EPOLLIN;
            event.data.fd = fd;
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0)
                throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
        started_ = true;
        thread_ = std::thread([this] { serve(); });
    } catch (...) {
        running_ = false;
        const bool had_control = control_fd_ >= 0;
        const bool had_service = service_fd_ >= 0;
        close_fd(epoll_fd_); close_fd(control_fd_); close_fd(service_fd_);
        std::error_code ignored;
        if (had_control) std::filesystem::remove(control_path_, ignored);
        if (had_service) std::filesystem::remove(service_path_, ignored);
        started_ = false;
        throw;
    }
}

void IpcManager::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    close_fd(epoll_fd_); close_fd(control_fd_); close_fd(service_fd_);
    // Only paths bound by this instance are removed.
    if (started_) {
        std::error_code ignored;
        std::filesystem::remove(control_path_, ignored);
        std::filesystem::remove(service_path_, ignored);
        started_ = false;
    }
}

void IpcManager::serve() {
    std::unordered_map<int, Client> clients;
    auto remove_client = [&](int fd) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        clients.erase(fd);
    };
    auto respond = [](Client& client, const ipc::Frame& frame) { client.output += ipc::encode(frame); };
    auto error = [&](Client& client, std::uint32_t id, int code, const char* message) {
        respond(client, {static_cast<std::uint16_t>(ipc::Type::error), id,
            "{\"result\":\"ERROR\",\"code\":" + std::to_string(code) +
            ",\"message\":\"" + message + "\"}"});
    };
    auto handle = [&](Client& client, const ipc::Frame& frame) {
        try {
            const Payload payload(frame.payload);
            const auto name = payload.service_name();
            const auto status = query_(name);
            if (!status) { error(client, frame.request_id, 1001, "invalid service"); return; }
            if (client.service && frame.type == static_cast<std::uint16_t>(ipc::Type::heartbeat)) {
                payload.heartbeat_timestamp();
                client.service_name = name;
                post_(Event{EventType::heartbeat, name, Clock::now()});
                return; // HEARTBEAT is one-way.
            }
            if (!client.service && frame.type == static_cast<std::uint16_t>(ipc::Type::start)) {
                payload.command_fields();
                post_(Event{EventType::start, name, Clock::now()});
                respond(client, {frame.type, frame.request_id, "{\"result\":\"OK\",\"state\":\"STARTING\"}"});
            } else if (!client.service && frame.type == static_cast<std::uint16_t>(ipc::Type::stop)) {
                payload.command_fields();
                for (auto& [fd, peer] : clients) {
                    if (peer.service && peer.service_name == name)
                        respond(peer, {static_cast<std::uint16_t>(ipc::Type::event), 0,
                            "{\"event\":\"SERVICE_STOP\",\"service_name\":\"" + json_escape(name) + "\"}"});
                }
                post_(Event{EventType::stop, name, Clock::now()});
                respond(client, {frame.type, frame.request_id, "{\"result\":\"OK\"}"});
            } else if (!client.service && frame.type == static_cast<std::uint16_t>(ipc::Type::query_status)) {
                payload.command_fields();
                respond(client, {frame.type, frame.request_id,
                    "{\"service_name\":\"" + json_escape(name) + "\",\"state\":\"" + state_name(status->state) +
                    "\",\"heartbeat_time\":" + (status->heartbeat_time ? std::to_string(epoch_seconds(*status->heartbeat_time)) : "0") +
                    ",\"restart_count\":" + std::to_string(status->restart_count) + "}"});
            } else error(client, frame.request_id, 1002, "invalid message type");
        } catch (const std::exception&) { error(client, frame.request_id, 1002, "invalid payload"); }
    };
    while (running_) {
        epoll_event ready[32]{};
        const int count = ::epoll_wait(epoll_fd_, ready, 32, 200);
        if (count < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < count; ++i) {
            const int fd = ready[i].data.fd;
            if (fd == control_fd_ || fd == service_fd_) {
                while (true) {
                    const int client_fd = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) { if (errno == EINTR) continue; break; }
                    clients.emplace(client_fd, Client{fd == service_fd_, {}, {}, {}, false});
                    epoll_event event{}; event.events = EPOLLIN | EPOLLRDHUP; event.data.fd = client_fd;
                    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) remove_client(client_fd);
                }
                continue;
            }
            auto it = clients.find(fd);
            if (it == clients.end()) continue;
            Client& client = it->second;
            bool closed = (ready[i].events & EPOLLERR) != 0;
            if (ready[i].events & EPOLLIN) {
                char buffer[4096];
                while (!closed) {
                    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
                    if (n > 0) client.input.append(buffer, static_cast<std::size_t>(n));
                    else if (n == 0) { client.input_closed = true; break; }
                    else if (errno == EINTR) continue;
                    else { if (errno != EAGAIN && errno != EWOULDBLOCK) closed = true; break; }
                }
                while (!closed && client.input.size() >= ipc::header_size) {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(client.input.data());
                    const std::uint32_t length = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
                        (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
                    if (length > ipc::max_payload) { closed = true; break; }
                    if (client.input.size() < ipc::header_size + length) break;
                    handle(client, ipc::decode(client.input.substr(0, ipc::header_size + length)));
                    client.input.erase(0, ipc::header_size + length);
                }
            }
            if (ready[i].events & (EPOLLRDHUP | EPOLLHUP)) client.input_closed = true;
            if (client.input_closed && !client.input.empty()) closed = true;
            if (!closed && !client.output.empty()) {
                while (!client.output.empty()) {
                    const auto n = ::send(fd, client.output.data(), client.output.size(), MSG_NOSIGNAL);
                    if (n > 0) client.output.erase(0, static_cast<std::size_t>(n));
                    else if (n < 0 && errno == EINTR) continue;
                    else { if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) closed = true; break; }
                }
            }
            if (closed || (client.input_closed && client.output.empty())) remove_client(fd);
            else {
                epoll_event event{};
                event.events = EPOLLRDHUP;
                if (!client.input_closed) event.events |= EPOLLIN;
                if (!client.output.empty()) event.events |= EPOLLOUT;
                event.data.fd = fd;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);
            }
            // STOP can enqueue an EVENT on a different service connection.
            for (const auto& [peer_fd, peer] : clients) {
                if (peer_fd == fd || peer.output.empty()) continue;
                epoll_event event{};
                event.events = EPOLLRDHUP | EPOLLOUT;
                if (!peer.input_closed) event.events |= EPOLLIN;
                event.data.fd = peer_fd;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, peer_fd, &event);
            }
        }
    }
    for (const auto& [fd, client] : clients) ::close(fd);
}

} // namespace runtime
