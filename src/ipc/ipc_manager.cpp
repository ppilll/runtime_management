#include "ipc_manager.hpp"
#include "frame.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace runtime {
struct IpcManager::DeviceEvents {
    struct Record { std::uint64_t sequence; std::string payload; };
    std::mutex mutex;
    std::deque<Record> pending;
    std::uint64_t sequence = 0;
    std::uint64_t lost_through = 0;
    std::size_t bytes = 0;
};

namespace {
constexpr std::size_t max_queued_output = 4 * (ipc::header_size + ipc::max_payload);
struct Client {
    bool service;
    std::string input;
    std::string output;
    std::string service_name;
    bool input_closed = false;
    bool subscribed = false;
    std::uint64_t subscribed_after = 0;
    bool output_overflow = false;
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

// Command payloads remain flat JSON objects with string and integer values.
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
                if (key != "service_name" && key != "timestamp" && key != "event") fail();
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
    void command_fields() const { if (values_.size() != 1 || !values_.count("service_name")) fail(); }
    void list_fields() const { if (!values_.empty()) fail(); }
    void subscription_fields() const {
        const auto it = values_.find("event");
        if (values_.size() != 1 || it == values_.end() || it->second.second ||
            it->second.first != "DEVICE_STATE_CHANGED") fail();
    }
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

std::string device_json(const DeviceStateSnapshot& state) {
    if (!valid_utf8(state.reason) || !valid_utf8(state.source))
        throw std::invalid_argument("invalid device snapshot UTF-8");
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        state.timestamp.time_since_epoch()).count();
    return "{\"state\":\"" + std::string(state_name(state.current)) +
        "\",\"timestamp\":" + std::to_string(timestamp) +
        ",\"reason\":\"" + json_escape(state.reason) + "\"}";
}

const char* device_health(DeviceState state) {
    switch (state) {
    case DeviceState::running: return "HEALTHY";
    case DeviceState::warning: return "DEGRADED";
    case DeviceState::error: case DeviceState::offline: return "UNHEALTHY";
    default: return "UNKNOWN";
    }
}

const char* health_status(const ServiceStatus& status, std::chrono::seconds timeout,
                          Clock::time_point now) {
    if (status.state == ServiceState::failed) return "UNHEALTHY";
    if (status.state != ServiceState::running || status.pid <= 0) return "UNKNOWN";
    // Do not treat successful exec alone as proof of heartbeat health.
    auto reference = status.start_time;
    const bool has_heartbeat = status.heartbeat_time &&
        (!reference || *status.heartbeat_time >= *reference);
    if (has_heartbeat) reference = status.heartbeat_time;
    if (!reference || *reference > now) return "UNKNOWN";
    if (now - *reference >= timeout) return "UNHEALTHY";
    return has_heartbeat ? "HEALTHY" : "UNKNOWN";
}

} // namespace

IpcManager::IpcManager(std::string control_path, std::string service_path, Post post, Query query,
                       std::vector<ServiceConfig> definitions, QueryDevice query_device)
    : control_path_(std::move(control_path)), service_path_(std::move(service_path)),
      post_(std::move(post)), query_(std::move(query)), definitions_(std::move(definitions)),
      query_device_(std::move(query_device)), device_events_(std::make_shared<DeviceEvents>()) {
    if (!post_ || !query_ || control_path_ == service_path_)
        throw std::invalid_argument("IPC requires distinct paths and runtime callbacks");
    std::unordered_set<std::string> names;
    for (const auto& definition : definitions_) {
        if (definition.service_name.empty() || definition.heartbeat_timeout <= std::chrono::seconds::zero() ||
            !names.insert(definition.service_name).second)
            throw std::invalid_argument("invalid static IPC service definitions");
    }
}

IpcManager::~IpcManager() { stop(); }

IpcManager::DeviceStateSink IpcManager::device_state_sink() const {
    return [weak = std::weak_ptr<DeviceEvents>(device_events_)](const DeviceStateSnapshot& state) {
        const auto events = weak.lock();
        if (!events) return;
        std::string payload;
        try {
            payload = "{\"event\":\"DEVICE_STATE_CHANGED\",\"previous_state\":\"" +
                std::string(state_name(state.previous)) + "\",\"source\":\"" + json_escape(state.source) +
                "\"," + device_json(state).substr(1);
        } catch (const std::exception&) {
            // A gap is explicit: affected subscribers will be disconnected.
        }
        std::lock_guard<std::mutex> lock(events->mutex);
        const auto sequence = ++events->sequence;
        if (payload.empty() || payload.size() > ipc::max_payload) {
            events->lost_through = sequence;
            return;
        }
        events->bytes += payload.size();
        events->pending.push_back({sequence, std::move(payload)});
        while (events->bytes > max_queued_output || events->pending.size() > 1024) {
            events->lost_through = std::max(events->lost_through, events->pending.front().sequence);
            events->bytes -= events->pending.front().payload.size();
            events->pending.pop_front();
        }
    };
}

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
    auto respond = [](Client& client, const ipc::Frame& frame) {
        const auto encoded = ipc::encode(frame);
        if (client.output.size() + encoded.size() > max_queued_output) client.output_overflow = true;
        else if (!client.output_overflow) client.output += encoded;
    };
    auto error = [&](Client& client, std::uint32_t id, int code, const char* message) {
        respond(client, {static_cast<std::uint16_t>(ipc::Type::error), id,
            "{\"result\":\"ERROR\",\"code\":" + std::to_string(code) +
            ",\"message\":\"" + message + "\"}"});
    };
    auto notify_stop = [&](const std::string& name) {
        for (auto& [fd, peer] : clients) {
            if (peer.service && peer.service_name == name)
                respond(peer, {static_cast<std::uint16_t>(ipc::Type::event), 0,
                    "{\"event\":\"SERVICE_STOP\",\"service_name\":\"" + json_escape(name) + "\"}"});
        }
    };
    auto handle = [&](Client& client, const ipc::Frame& frame) {
        bool device_snapshot_request = false;
        try {
            const Payload payload(frame.payload);
            if (frame.type == static_cast<std::uint16_t>(ipc::Type::get_device_state) ||
                frame.type == static_cast<std::uint16_t>(ipc::Type::get_health) ||
                frame.type == static_cast<std::uint16_t>(ipc::Type::subscribe_event)) {
                if (client.service) { error(client, frame.request_id, 1002, "invalid message type"); return; }
                const bool subscribe = frame.type == static_cast<std::uint16_t>(ipc::Type::subscribe_event);
                if (subscribe) payload.subscription_fields();
                else payload.list_fields();
                if (!query_device_) { error(client, frame.request_id, 1003, "device snapshot unavailable"); return; }
                if (subscribe) {
                    // Locking with the producer defines the subscription boundary.
                    // Repeating the same subscription does not reset it or duplicate delivery.
                    if (!client.subscribed) {
                        std::lock_guard<std::mutex> lock(device_events_->mutex);
                        client.subscribed_after = device_events_->sequence;
                        client.subscribed = true;
                    }
                    respond(client, {frame.type, frame.request_id,
                        "{\"result\":\"OK\",\"event\":\"DEVICE_STATE_CHANGED\"}"});
                    return;
                }
                device_snapshot_request = true;
                DeviceStateSnapshot state;
                try { state = query_device_(); }
                catch (const std::exception&) { error(client, frame.request_id, 1003, "device snapshot unavailable"); return; }
                std::string result = device_json(state);
                if (frame.type == static_cast<std::uint16_t>(ipc::Type::get_health)) {
                    result = "{\"device_state\":" + result + ",\"service_summary\":{\"services\":[";
                    std::size_t healthy = 0, unhealthy = 0, unknown = 0;
                    bool first = true;
                    const auto now = Clock::now();
                    for (const auto& definition : definitions_) {
                        const auto status = query_(definition.service_name);
                        if (!status) { error(client, frame.request_id, 1003, "service snapshot unavailable"); return; }
                        const std::string health = health_status(*status, definition.heartbeat_timeout, now);
                        if (health == "HEALTHY") ++healthy;
                        else if (health == "UNHEALTHY") ++unhealthy;
                        else ++unknown;
                        if (!first) result += ',';
                        first = false;
                        result += "{\"service_name\":\"" + json_escape(definition.service_name) +
                            "\",\"state\":\"" + state_name(status->state) + "\",\"pid\":" + std::to_string(status->pid) +
                            ",\"health_status\":\"" + health + "\"}";
                        if (result.size() > ipc::max_payload) break;
                    }
                    const char* overall_health = device_health(state.current);
                    if (state.current == DeviceState::running) {
                        // Lifecycle readiness alone does not confirm heartbeat health.
                        // Use the rows already returned, without mutating device state.
                        if (unhealthy != 0) overall_health = "UNHEALTHY";
                        else if (unknown != 0 || definitions_.empty()) overall_health = "UNKNOWN";
                    }
                    result += "],\"total\":" + std::to_string(definitions_.size()) +
                        ",\"healthy\":" + std::to_string(healthy) + ",\"unhealthy\":" + std::to_string(unhealthy) +
                        ",\"unknown\":" + std::to_string(unknown) + "},\"health\":{\"status\":\"" +
                        overall_health + "\",\"reason\":\"" + json_escape(state.reason) + "\"}}";
                }
                if (result.size() > ipc::max_payload) { error(client, frame.request_id, 1003, "device response exceeds payload limit"); return; }
                respond(client, {frame.type, frame.request_id, std::move(result)});
                return;
            }
            if (frame.type == static_cast<std::uint16_t>(ipc::Type::get_service_list)) {
                if (client.service) { error(client, frame.request_id, 1002, "invalid message type"); return; }
                payload.list_fields();
                std::string result = "{\"services\":[";
                bool first = true;
                for (const auto& definition : definitions_) {
                    const auto status = query_(definition.service_name);
                    if (!status) { error(client, frame.request_id, 1003, "service snapshot unavailable"); return; }
                    if (!first) result += ',';
                    first = false;
                    result += "{\"service_name\":\"" + json_escape(definition.service_name) +
                        "\",\"state\":\"" + state_name(status->state) + "\",\"pid\":" + std::to_string(status->pid) +
                        ",\"health_status\":\"" + health_status(*status, definition.heartbeat_timeout, Clock::now()) + "\"}";
                    if (result.size() + 2 > ipc::max_payload) {
                        error(client, frame.request_id, 1003, "service list exceeds payload limit"); return;
                    }
                }
                result += "]}";
                respond(client, {frame.type, frame.request_id, std::move(result)});
                return;
            }
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
                notify_stop(name);
                post_(Event{EventType::stop, name, Clock::now()});
                respond(client, {frame.type, frame.request_id, "{\"result\":\"OK\"}"});
            } else if (!client.service && frame.type == static_cast<std::uint16_t>(ipc::Type::restart_service)) {
                payload.command_fields();
                post_(Event{EventType::restart_request, name, Clock::now()});
                notify_stop(name);
                respond(client, {frame.type, frame.request_id, "{\"result\":\"OK\"}"});
            } else if (!client.service && frame.type == static_cast<std::uint16_t>(ipc::Type::query_status)) {
                payload.command_fields();
                respond(client, {frame.type, frame.request_id,
                    "{\"service_name\":\"" + json_escape(name) + "\",\"state\":\"" + state_name(status->state) +
                    "\",\"heartbeat_time\":" + (status->heartbeat_time ? std::to_string(epoch_seconds(*status->heartbeat_time)) : "0") +
                    ",\"restart_count\":" + std::to_string(status->restart_count) + "}"});
            } else error(client, frame.request_id, 1002, "invalid message type");
        } catch (const std::exception&) {
            error(client, frame.request_id, device_snapshot_request ? 1003 : 1002,
                  device_snapshot_request ? "device snapshot unavailable" : "invalid payload");
        }
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
            if (closed || client.output_overflow || (client.input_closed && client.output.empty())) remove_client(fd);
            else {
                epoll_event event{};
                event.events = EPOLLRDHUP;
                if (!client.input_closed) event.events |= EPOLLIN;
                if (!client.output.empty()) event.events |= EPOLLOUT;
                event.data.fd = fd;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);
            }
            // STOP/RESTART_SERVICE can enqueue an EVENT on another connection.
            for (const auto& [peer_fd, peer] : clients) {
                if (peer_fd == fd || peer.output.empty()) continue;
                epoll_event event{};
                event.events = EPOLLRDHUP | EPOLLOUT;
                if (!peer.input_closed) event.events |= EPOLLIN;
                event.data.fd = peer_fd;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, peer_fd, &event);
            }
        }
        // Only this epoll thread owns clients/output. Runtime callbacks enqueue copies.
        std::deque<DeviceEvents::Record> changes;
        std::uint64_t lost_through;
        {
            std::lock_guard<std::mutex> lock(device_events_->mutex);
            changes.swap(device_events_->pending);
            device_events_->bytes = 0;
            lost_through = device_events_->lost_through;
        }
        for (auto it = clients.begin(); it != clients.end();) {
            const int fd = it->first;
            auto& client = it->second;
            if (client.subscribed) {
                if (client.subscribed_after < lost_through) client.output_overflow = true;
                for (const auto& change : changes) {
                    if (change.sequence <= client.subscribed_after) continue;
                    respond(client, {static_cast<std::uint16_t>(ipc::Type::event), 0, change.payload});
                    client.subscribed_after = change.sequence;
                }
            }
            ++it;
            if (client.output_overflow) { remove_client(fd); continue; }
            if (!client.output.empty()) {
                epoll_event event{};
                event.events = EPOLLRDHUP | EPOLLOUT;
                if (!client.input_closed) event.events |= EPOLLIN;
                event.data.fd = fd;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);
            }
        }
    }
    for (const auto& [fd, client] : clients) ::close(fd);
}

} // namespace runtime
