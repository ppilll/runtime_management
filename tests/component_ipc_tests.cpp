#include "../src/ipc/frame.hpp"
#include "../src/ipc/ipc_manager.hpp"
#include "runtime/runtime_manager.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace runtime {
// Read-only mailbox observation for deterministic pressure tests. Credentials
// still come from real sockets; this seam never manufactures a peer or session.
class ComponentHealthTestAccess {
public:
    static std::size_t outstanding(RuntimeManager& core) {
        std::lock_guard<std::mutex> lock(core.component_mailbox_->mutex);
        return core.component_mailbox_->outstanding;
    }
};
}

namespace {
using namespace runtime;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
void until(Predicate predicate, std::chrono::seconds budget = 8s) {
    const auto deadline = Clock::now() + budget;
    while (!predicate()) {
        require(Clock::now() < deadline, "predicate deadline exceeded");
        std::this_thread::sleep_for(2ms);
    }
}

struct Directory {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("p6-ipc-" + std::to_string(::getpid()) + "-" + std::to_string(Clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directory(root); }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::string path(const std::string& name) const { return (root / name).string(); }
};

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::string& path, const std::string& text) {
    const auto pending = path + ".pending";
    std::ofstream output(pending, std::ios::binary);
    output << text;
    output.close();
    require(static_cast<bool>(output), "fixture write failed");
    std::filesystem::rename(pending, path);
}

struct Socket {
    int fd = -1;
    explicit Socket(const std::string& path) {
        fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(fd >= 0, "socket failed");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        require(path.size() < sizeof(address.sun_path), "socket path too long");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        timeval timeout{4, 0};
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

std::string read_exact(int fd, std::size_t size) {
    std::string bytes(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const auto n = ::recv(fd, bytes.data() + offset, size - offset, 0);
        if (n > 0) offset += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else throw std::runtime_error("receive failed");
    }
    return bytes;
}

ipc::Frame receive(int fd) {
    const auto header = read_exact(fd, ipc::header_size);
    const auto* bytes = reinterpret_cast<const unsigned char*>(header.data());
    const std::uint32_t length = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
        (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    require(length <= ipc::max_payload, "oversized reply");
    return ipc::decode(header + read_exact(fd, length));
}

ipc::Frame request(int fd, std::uint16_t type, const std::string& payload, std::uint32_t id = 7) {
    send_all(fd, ipc::encode({type, id, payload}));
    auto reply = receive(fd);
    require(reply.request_id == id, "request/reply ID mismatch");
    return reply;
}

void expect_error(const ipc::Frame& reply, unsigned code) {
    require(reply.type == 255 && reply.payload.find("\"code\":" + std::to_string(code)) != std::string::npos,
            "wrong error/code");
    require(reply.payload.find("session_id") == std::string::npos &&
            reply.payload.find("runtime_epoch") == std::string::npos, "error leaked identity");
}

// Only extracts known string-valued reply fields, not a production JSON parser.
std::string field(const std::string& json, const std::string& key) {
    const auto prefix = "\"" + key + "\":\"";
    const auto begin = json.find(prefix);
    require(begin != std::string::npos, "reply field missing");
    const auto value = begin + prefix.size();
    const auto end = json.find('"', value);
    require(end != std::string::npos, "reply string unterminated");
    return json.substr(value, end - value);
}

std::string changed(std::string text, const std::string& from, const std::string& to) {
    const auto pos = text.find(from);
    require(pos != std::string::npos, "fixture replacement missing");
    text.replace(pos, from.size(), to);
    return text;
}

const std::string bind = R"({"version":1,"op":"bind","service_name":"worker"})";
const std::string epoch = "00112233445566778899aabbccddeeff";

std::string report(const std::string& runtime_epoch, const std::string& session, unsigned seq = 1,
                   unsigned revision = 12, bool issue = false) {
    return "{\"version\":1,\"op\":\"report\",\"runtime_epoch\":\"" + runtime_epoch +
        "\",\"session_id\":\"" + session + "\",\"report_seq\":\"" + std::to_string(seq) +
        "\",\"component\":\"mcu_link\",\"status\":\"" + (issue ? "UNHEALTHY" : "HEALTHY") +
        "\",\"reason_code\":\"" + (issue ? "link_lost" : "recovered") +
        "\",\"reason\":\"owner\",\"projection_revision\":\"" + std::to_string(revision) +
        "\",\"link_epoch\":\"2\",\"peer_boot_id\":0,\"sample_kind\":\"" + (issue ? "issue" : "current") + "\"}";
}

RuntimeConfig configuration() {
    ServiceConfig service;
    service.service_name = "worker";
    service.executable = "/proc/self/exe";
    service.autostart = true;
    service.heartbeat_timeout = 120s;
    service.shutdown_timeout = 1s;
    RuntimeConfig config{{service}, {}};
    config.health.components.emplace_back("mcu_link", "worker", ServiceHealthPolicy{ServiceCriticality::high, true});
    return config;
}

struct Harness {
    Directory directory;
    RuntimeConfig config = configuration();
    IpcManager::DeviceStateSink changes;
    std::unique_ptr<RuntimeManager> core;
    std::unique_ptr<IpcManager> server;
    std::thread writer;
    std::exception_ptr failure;
    unsigned command_id = 0;
    std::string binding;
    int child_pid = -1;

    explicit Harness(bool run = false) {
        config.services.front().arguments = {"--producer", directory.path("service.sock"), directory.root.string()};
        core = std::make_unique<RuntimeManager>(config, AggregationOptions{},
            [this](const DeviceStateSnapshot& state) { if (changes) changes(state); }, ResourceInputMode::external);
        server = std::make_unique<IpcManager>(directory.path("control.sock"), directory.path("service.sock"),
            [this](Event event) { core->post(std::move(event)); },
            [this](const std::string& name) { return core->query(name); }, config.services,
            [this] { return core->queryDeviceState(); }, std::make_shared<ComponentTransportAdapter>(*core),
            [this] { return core->queryComponentHealth(); });
        changes = server->device_state_sink();
        server->start();
        if (run) {
            try { start(); }
            catch (...) {
                if (writer.joinable()) {
                    core->post(Event{EventType::shutdown, {}, Clock::now()});
                    writer.join();
                }
                server->stop();
                throw;
            }
        }
    }
    void start() {
        writer = std::thread([this] { try { core->run(); } catch (...) { failure = std::current_exception(); } });
        until([&] { return core->query("worker")->state == ServiceState::running; });
        child_pid = core->query("worker")->pid;
        require(child_pid > 0 && child_pid != ::getpid(), "producer is not an independent child");
        write_file(directory.path("go"), "ready");
        until([&] { return std::filesystem::exists(directory.path("bound")); });
        binding = read_file(directory.path("bound"));
        require(field(binding, "result") == "OK", "managed bind failed");
        require(field(binding, "instance_generation") == std::to_string(core->query("worker")->launched_generation),
                "bind used status generation instead of launch identity");
    }
    ipc::Frame command(std::uint16_t type, const std::string& payload = "") {
        const auto suffix = std::to_string(++command_id);
        write_file(directory.path("command" + suffix), std::to_string(type) + '\n' + payload);
        until([&] { return std::filesystem::exists(directory.path("reply" + suffix)); });
        const auto text = read_file(directory.path("reply" + suffix));
        const auto split = text.find('\n');
        require(split != std::string::npos, "child reply malformed");
        return {static_cast<std::uint16_t>(std::stoul(text.substr(0, split))), command_id + 100, text.substr(split + 1)};
    }
    std::string sample(unsigned seq = 1, unsigned revision = 12, bool issue = false) const {
        return report(field(binding, "runtime_epoch"), field(binding, "session_id"), seq, revision, issue);
    }
    void reconnect() {
        require(command(65535).payload == "closed", "close command failed");
        require(command(65534).payload == "connected", "reconnect command failed");
        const auto reply = command(11, bind);
        require(reply.type == 11 && field(reply.payload, "result") == "OK", "rebind failed");
        binding = reply.payload;
    }
    void finish() {
        if (writer.joinable()) {
            core->post(Event{EventType::shutdown, {}, Clock::now()});
            writer.join();
        }
        server->stop();
        if (failure) std::rethrow_exception(failure);
    }
    ~Harness() {
        if (writer.joinable()) {
            core->post(Event{EventType::shutdown, {}, Clock::now()});
            writer.join();
        }
        server->stop();
    }
};

// This program is the actual foreground executable launched/reaped by SM.
// File gates control normalized test actions; connected UDS fds are never
// inherited, delegated or wrapped. No serial/MCU implementation is present.
int producer(const std::string& socket_path, const std::string& directory) {
    const auto path = [&](const std::string& name) { return (std::filesystem::path(directory) / name).string(); };
    until([&] { return std::filesystem::exists(path("go")); });
    auto socket = std::make_unique<Socket>(socket_path);
    auto reply = request(socket->fd, 11, bind, 1);
    write_file(path("bound"), reply.payload);
    for (unsigned i = 1; ; ++i) {
        until([&] { return std::filesystem::exists(path("command" + std::to_string(i))); }, 60s);
        const auto text = read_file(path("command" + std::to_string(i)));
        const auto split = text.find('\n');
        const auto type = static_cast<std::uint16_t>(std::stoul(text.substr(0, split)));
        const auto payload = text.substr(split + 1);
        if (type == 65533) return 31;
        if (type == 65535) { socket.reset(); reply = {type, i + 100, "closed"}; }
        else if (type == 65534) { socket = std::make_unique<Socket>(socket_path); reply = {type, i + 100, "connected"}; }
        else if (type == 65532) {
            send_all(socket->fd, ipc::encode({4, 0, R"({"service_name":"worker","timestamp":-1})"}) +
                                 ipc::encode({11, i + 100, payload}));
            reply = receive(socket->fd);
            require(reply.request_id == i + 100, "coincident heartbeat produced a success ACK");
        }
        else if (type == 4) {
            send_all(socket->fd, ipc::encode({type, i + 100, payload}));
            reply = {type, i + 100, "sent"};
        } else reply = request(socket->fd, type, payload, i + 100);
        write_file(path("reply" + std::to_string(i)), std::to_string(reply.type) + '\n' + reply.payload);
    }
}

void test_strict_health_parser_and_direction() {
    Harness harness;
    Socket service(harness.directory.path("service.sock"));
    Socket control(harness.directory.path("control.sock"));
    expect_error(request(control.fd, 11, bind), 1002);
    expect_error(request(service.fd, 5, R"({"event":"DEVICE_STATE_CHANGED"})"), 1002);
    expect_error(request(service.fd, 11, bind, 0), 1002);
    const auto valid = report(epoch, "1");
    std::vector<std::string> invalid = {
        "null", "[]", "{", bind + "{}", R"({"version":1,"op":"bind"})",
        changed(bind, "\"version\":1", "\"version\":true"),
        changed(bind, "\"version\":1", "\"version\":1.0"),
        changed(bind, "\"version\":1", "\"version\":2"),
        changed(bind, "\"version\":1", "\"version\":1,\"version\":1"),
        changed(bind, "\"version\":1", "\"version\":1,\"pid\":1"),
        changed(bind, "\"worker\"", "null"),
        changed(valid, "\"peer_boot_id\":0", "\"peer_boot_id\":true"),
        changed(valid, "\"peer_boot_id\":0", "\"peer_boot_id\":-1"),
        changed(valid, "\"peer_boot_id\":0", "\"peer_boot_id\":4294967296"),
        changed(valid, "\"peer_boot_id\":0", "\"peer_boot_id\":\"0\""),
        changed(valid, "\"reason\":\"owner\"", "\"reason\":null"),
        changed(valid, "\"reason\":\"owner\"", "\"reason\":\"\\ud800\""),
        changed(valid, "\"owner\"", "\"" + std::string(257, 'x') + "\""),
        changed(valid, "\"owner\"", "\"" + std::string("\xc0\xaf", 2) + "\""),
        changed(valid, "\"recovered\"", "\"report_expired\""),
        changed(valid, "\"recovered\"", "\"link_lost\""),
        changed(valid, "\"current\"", "\"issue\""),
        changed(valid, epoch, "00112233445566778899AABBCCDDEEFF"),
        changed(valid, "\"session_id\":\"1\"", "\"session_id\":1")
    };
    for (const auto& number : {"0", "01", "+1", "-1", "1.0", "18446744073709551616", ""})
        invalid.push_back(changed(valid, "\"session_id\":\"1\"", "\"session_id\":\"" + std::string(number) + "\""));
    for (const auto& text : invalid) expect_error(request(service.fd, 11, text), 1002);
    require(ComponentHealthTestAccess::outstanding(*harness.core) == 0, "malformed payload reached writer mailbox");
    expect_error(request(service.fd, 11, std::string(4097, ' ')), 1002);
    expect_error(request(control.fd, 9, R"({"include_components":true})"), 1002);
    expect_error(request(control.fd, 9, R"({"include_components":1,"include_components":1})"), 1002);
}

void test_async_ack_queue_cap_tombstone_and_control_fairness() {
    Harness harness; // Deliberately do not run writer: admission acceptance is not ACK.
    std::vector<std::unique_ptr<Socket>> peers;
    for (unsigned i = 0; i < 32; ++i) {
        peers.push_back(std::make_unique<Socket>(harness.directory.path("service.sock")));
        send_all(peers.back()->fd, ipc::encode({11, i + 1, bind}));
        until([&] { return ComponentHealthTestAccess::outstanding(*harness.core) == i + 1; });
    }
    pollfd pending{peers.front()->fd, POLLIN, 0};
    require(::poll(&pending, 1, 0) == 0, "ACK sent before writer apply");
    expect_error(request(peers.front()->fd, 11, bind, 90), 1003);
    Socket extra(harness.directory.path("service.sock"));
    expect_error(request(extra.fd, 11, bind), 1003);
    require(ComponentHealthTestAccess::outstanding(*harness.core) == 32, "queue exceeded 32 operations");
    // Close with a full queue, then start writer. Revoked records cannot bind;
    // the unrelated control connection remains usable throughout pressure.
    peers.clear();
    Socket control(harness.directory.path("control.sock"));
    require(request(control.fd, 8, "{}").type == 8, "health pressure starved control query");
    harness.start();
    require(!harness.binding.empty(), "new child session was blocked by old tombstones");
    require(harness.core->queryComponentHealth().front().fact.identity.pid == harness.child_pid,
            "queued foreign peer acquired producer identity");
}

void test_integer_null_utf8_and_exact_payload_boundaries() {
    Harness harness;
    std::vector<std::unique_ptr<Socket>> peers;
    const auto valid = report(epoch, "1");
    std::vector<std::string> accepted = {
        changed(valid, "\"peer_boot_id\":0", "\"peer_boot_id\":null"),
        changed(valid, "\"peer_boot_id\":0", "\"peer_boot_id\":4294967295"),
        changed(valid, "\"owner\"", "\"" + std::string(256, 'x') + "\""),
        changed(valid, "\"session_id\":\"1\"", "\"session_id\":\"9223372036854775808\""),
        changed(valid, "\"session_id\":\"1\"", "\"session_id\":\"18446744073709551615\"")
    };
    std::string utf8;
    for (unsigned i = 0; i < 128; ++i) utf8 += "\xc2\xa3";
    accepted.push_back(changed(valid, "\"owner\"", '"' + utf8 + '"'));
    accepted.push_back(bind + std::string(component_payload_limit - bind.size(), ' '));
    for (const auto& text : accepted) {
        peers.push_back(std::make_unique<Socket>(harness.directory.path("service.sock")));
        send_all(peers.back()->fd, ipc::encode({11, 1, text}));
        until([&] { return ComponentHealthTestAccess::outstanding(*harness.core) == peers.size(); });
        pollfd pending{peers.back()->fd, POLLIN, 0};
        require(::poll(&pending, 1, 0) == 0, "well-formed boundary payload rejected before writer");
    }
    require(accepted.back().size() == 4096, "exact size fixture wrong");
    Socket extra(harness.directory.path("service.sock"));
    expect_error(request(extra.fd, 11, bind + std::string(4097 - bind.size(), ' ')), 1002);
}

void test_real_peer_bound_heartbeat_and_committed_query() {
    Harness harness(true);
    Socket impostor(harness.directory.path("service.sock"));
    expect_error(request(impostor.fd, 11, bind), 1004); // Same UID, wrong real PID.
    Socket unbound(harness.directory.path("service.sock"));
    expect_error(request(unbound.fd, 4, R"({"service_name":"worker","timestamp":9223372036854775807})"), 1004);
    const auto before = *harness.core->query("worker");
    const auto ack = harness.command(65532, harness.sample()); // HB+REPORT in one write.
    require(ack.type == 11 && field(ack.payload, "result") == "APPLIED", "report was not applied");
    until([&] { return harness.core->query("worker")->heartbeat_time.has_value(); });
    const auto committed = harness.core->queryComponentHealth().front();
    require(committed.fact.observation_fresh && committed.fact.sample.status == ComponentHealthStatus::healthy,
            "ACK preceded committed component cache");
    require(committed.fact.sample.peer_boot_id && *committed.fact.sample.peer_boot_id == 0, "boot zero lost");
    Socket control(harness.directory.path("control.sock"));
    const auto legacy = request(control.fd, 9, "{}");
    const auto extended = request(control.fd, 9, R"({"include_components":1})");
    require(legacy.payload.find("components") == std::string::npos, "legacy health reply changed");
    require(extended.payload.find(legacy.payload.substr(0, legacy.payload.size() - 1)) == 0,
            "opt-in changed original health fields");
    require(extended.payload.find("\"producer\":\"worker\"") != std::string::npos &&
            extended.payload.find("\"observation_fresh\":true") != std::string::npos, "component query omitted metadata");
    require(harness.core->query("worker")->pid == before.pid && harness.core->query("worker")->restart_count == 0,
            "link report changed process identity/budget");
    const auto pid = harness.child_pid;
    harness.finish();
    int status = 0;
    errno = 0;
    require(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD, "Runtime did not exclusively reap producer");
}

void test_reconnect_replay_revision_and_epoch_isolation() {
    Harness harness(true);
    const auto issue = harness.sample(1, 12, true);
    require(harness.command(11, issue).type == 11, "issue not applied");
    const auto received = harness.core->queryComponentHealth().front().fact.received_at;
    require(harness.command(11, issue).type == 11, "duplicate issue not ACKed");
    require(harness.core->queryComponentHealth().front().fact.received_at == received, "duplicate renewed lease");
    expect_error(harness.command(11, changed(issue, "\"owner\"", "\"changed\"")), 1002);
    const auto old_binding = harness.binding;
    harness.reconnect();
    require(field(old_binding, "session_id") != field(harness.binding, "session_id"), "session reused on reconnect");
    require(harness.command(11, harness.sample(1, 12, true)).type == 11, "lost-ACK issue confirmation rejected");
    require(harness.command(11, harness.sample(2, 12)).type == 11, "same revision issue/current rejected");
    expect_error(harness.command(11, harness.sample(3, 11)), 1002);
    expect_error(harness.command(11, harness.sample(3, 12, true)), 1002);
    auto wrong_epoch = field(harness.binding, "runtime_epoch");
    wrong_epoch[0] = wrong_epoch[0] == '0' ? '1' : '0';
    expect_error(harness.command(11, changed(harness.sample(3, 13), field(harness.binding, "runtime_epoch"), wrong_epoch)), 1004);
    harness.reconnect();
    expect_error(harness.command(11, report(field(old_binding, "runtime_epoch"), field(old_binding, "session_id"), 9, 99)), 1004);
    require(harness.core->query("worker")->restart_count == 0, "transport errors triggered process recovery");
}

void test_unknown_registry_duplicate_bind_and_normalized_owner_failures() {
    Harness harness(true);
    const auto same = harness.command(11, bind);
    require(same.payload == harness.binding, "same-connection bind changed session");
    expect_error(harness.command(11, changed(bind, "worker", "unknown")), 1001);
    expect_error(harness.command(11, changed(harness.sample(), "mcu_link", "unknown")), 1001);
    const auto before = *harness.core->query("worker");
    const auto open_failed = changed(harness.sample(1, 12, true), "link_lost", "open_failed");
    require(harness.command(11, open_failed).type == 11, "normalized open failure rejected");
    require(harness.core->queryDeviceState().current == DeviceState::error &&
            harness.core->query("worker")->state == ServiceState::running &&
            harness.core->query("worker")->pid == before.pid && harness.core->query("worker")->restart_count == before.restart_count,
            "owner failure changed process policy");
    require(harness.command(11, harness.sample(2, 12)).type == 11, "owner recovery rejected");
    const auto committed = harness.core->queryComponentHealth(Clock::now() + 24h).front();
    require(committed.fact.observation_fresh && committed.report_age && *committed.report_age >= 24h,
            "query advanced writer lease/state");
}

void test_restart_runtime_epoch_and_old_instance_report() {
    std::string old_binding;
    {
        Harness first(true);
        old_binding = first.binding;
        require(first.command(11, first.sample()).type == 11, "first runtime report rejected");
        first.finish();
    }
    Harness next(true);
    require(field(old_binding, "runtime_epoch") != field(next.binding, "runtime_epoch"), "Runtime epoch repeated");
    expect_error(next.command(11, report(field(old_binding, "runtime_epoch"), field(old_binding, "session_id"))), 1004);
    require(!next.core->queryComponentHealth().front().fact.observation_fresh, "old Runtime report made replacement fresh");
}

void test_new_launch_rejects_previous_session_and_resets_owner_bounds() {
    Harness harness(true);
    require(harness.command(11, harness.sample(1, 19, true)).type == 11, "old launch issue rejected");
    const auto old_binding = harness.binding;
    const auto old_generation = harness.core->query("worker")->launched_generation;
    harness.core->post(Event{EventType::stop, "worker", Clock::now()});
    until([&] {
        const auto state = harness.core->query("worker");
        return state->state == ServiceState::stopped && state->pid <= 0;
    });
    std::filesystem::remove(harness.directory.path("go"));
    std::filesystem::remove(harness.directory.path("bound"));
    for (unsigned i = 1; i <= harness.command_id; ++i) {
        std::filesystem::remove(harness.directory.path("command" + std::to_string(i)));
        std::filesystem::remove(harness.directory.path("reply" + std::to_string(i)));
    }
    harness.command_id = 0;
    harness.core->post(Event{EventType::start, "worker", Clock::now()});
    until([&] { return harness.core->query("worker")->state == ServiceState::running; });
    require(harness.core->query("worker")->launched_generation != old_generation, "launch identity not changed");
    write_file(harness.directory.path("go"), "ready");
    until([&] { return std::filesystem::exists(harness.directory.path("bound")); });
    harness.binding = read_file(harness.directory.path("bound"));
    expect_error(harness.command(11, report(field(old_binding, "runtime_epoch"), field(old_binding, "session_id"), 2, 20)), 1004);
    harness.reconnect();
    require(harness.command(11, harness.sample(1, 1)).type == 11, "new launch inherited old revision lower bound");
    require(harness.core->queryComponentHealth().front().fact.identity.launched_generation ==
            harness.core->query("worker")->launched_generation, "new committed identity differs from SM launch");
}

void test_subscription_and_link_loss_preserve_process_identity() {
    Harness harness(true);
    Socket control(harness.directory.path("control.sock"));
    require(request(control.fd, 10, R"({"event":"DEVICE_STATE_CHANGED"})").type == 10, "subscribe ACK missing");
    require(harness.command(11, harness.sample()).type == 11, "initial report failed");
    const auto running = receive(control.fd);
    require(running.type == 5 && running.request_id == 0 && running.payload.find("RUNNING") != std::string::npos,
            "link readiness did not notify device state");
    const auto before = *harness.core->query("worker");
    require(harness.command(11, harness.sample(2, 13, true)).type == 11, "link loss issue rejected");
    const auto loss = receive(control.fd);
    require(loss.type == 5 && loss.request_id == 0 && loss.payload.find("ERROR") != std::string::npos &&
            loss.payload.find("component:mcu_link") != std::string::npos, "loss notification has wrong source/state");
    require(harness.core->query("worker")->pid == before.pid &&
            harness.core->query("worker")->launched_generation == before.launched_generation &&
            harness.core->query("worker")->restart_count == before.restart_count, "link loss restarted producer");
    require(harness.command(11, harness.sample(3, 13)).type == 11, "current recovery failed");
    const auto recovered = receive(control.fd);
    require(recovered.type == 5 && recovered.payload.find("RUNNING") != std::string::npos,
            "recovery notification missing");
}

void test_disconnect_and_process_exit_are_independent() {
    Harness harness(true);
    require(harness.command(11, harness.sample()).type == 11, "healthy report failed");
    const auto instance = *harness.core->query("worker");
    require(harness.command(65535).payload == "closed", "disconnect command failed");
    until([&] { return !harness.core->queryComponentHealth().front().fact.observation_fresh; });
    const auto lost = harness.core->queryComponentHealth().front();
    require(lost.fact.sample.reason_code == ComponentReason::observation_unavailable &&
            harness.core->query("worker")->pid == instance.pid && harness.core->query("worker")->state == ServiceState::running,
            "report disconnect became process failure");
    const auto suffix = std::to_string(++harness.command_id);
    write_file(harness.directory.path("command" + suffix), "65533\n");
    until([&] { return harness.core->query("worker")->state == ServiceState::failed; });
    require(!harness.core->queryComponentHealth().front().fact.observation_fresh &&
            harness.core->query("worker")->restart_count == 0, "real exit bypassed normal lifecycle policy");
}

template <typename Function>
void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid configuration accepted");
}

void test_health_config_legacy_strict_schema_and_wrapper() {
    Directory directory;
    const auto path = directory.path("config.json");
    const std::string service = R"({"service_name":"worker","executable":"/proc/self/exe","autostart":true,"heartbeat_timeout":4})";
    const std::string component = R"({"component":"mcu_link","producer_service":"worker","required":true,"criticality":"HIGH"})";
    const auto root = "{\"services\":[" + service + "],\"monitoring\":{\"sample_interval_seconds\":3}";
    for (const auto& legacy : {service, root + '}'}) {
        write_file(path, legacy);
        require(ConfigManager::load_runtime_file(path).health.components.empty(), "legacy config gained a component gate");
    }
    const auto config = root + ",\"health\":{\"components\":[" + component + "]}}";
    write_file(path, config);
    require(ConfigManager::load_runtime_file(path).health.components.size() == 1 &&
            ConfigManager::load_runtime_file(path).monitoring.sample_interval_seconds == 3s,
            "health discarded monitoring/registry");
    require(ConfigManager::load_file(path).size() == 1, "legacy wrapper lost services");
    write_file(path, root + R"(,"health":{"services":[{"service_name":"worker","required":false,"criticality":"LOW"}],"components":[]}})");
    require(!ConfigManager::load_runtime_file(path).health.services.at("worker").required,
            "explicit service policy lost");
    write_file(path, service.substr(0, service.size() - 1) + ",\"health\":{\"components\":[" + component + "]}}");
    require(ConfigManager::load_runtime_file(path).health.components.size() == 1, "single root health not supported");
    const std::vector<std::string> invalid = {
        changed(config, "\"health\":{", "\"health\":{\"unknown\":1,"),
        changed(config, "\"required\":true", "\"required\":1"),
        changed(config, "\"required\":true,", ""),
        changed(config, "\"criticality\":\"HIGH\"", "\"criticality\":null"),
        changed(config, "\"HIGH\"", "\"high\""),
        changed(config, "\"producer_service\":\"worker\"", "\"producer_service\":\"other\""),
        changed(config, "\"mcu_link\"", "\"other\""),
        changed(config, "\"components\":[" + component + "]", "\"components\":[" + component + ',' + component + "]"),
        changed(config, "\"autostart\":true", "\"autostart\":false"),
        changed(config, "\"heartbeat_timeout\":4", "\"heartbeat_timeout\":3"),
        root + ",\"health\":null}", root + ",\"health\":{\"services\":{}}}",
        root + R"(,"health":{"services":[{"service_name":"worker","required":true,"criticality":"HIGH"},{"service_name":"worker","required":true,"criticality":"HIGH"}]}})"
    };
    for (const auto& text : invalid) {
        write_file(path, text);
        rejects([&] { (void)ConfigManager::load_runtime_file(path); });
        rejects([&] { (void)ConfigManager::load_file(path); });
    }
}

void test_config_policy_merges_and_conflicts() {
    auto config = configuration();
    config.health.services["worker"] = {ServiceCriticality::high, true};
    AggregationOptions options;
    options.policies = config.health.services;
    options.components = config.health.components;
    { RuntimeManager core(config, options, {}, ResourceInputMode::external);
      require(core.queryComponentHealth().size() == 1, "same policy did not merge"); }
    options.policies["worker"].required = false;
    rejects([&] { RuntimeManager core(config, options); });
    options.policies = config.health.services;
    options.components.front().policy.criticality = ServiceCriticality::low;
    rejects([&] { RuntimeManager core(config, options); });
    config.health.components.front().producer_service = "missing";
    rejects([&] { RuntimeManager core(config); });
}

void test_config_registry_limits_and_legacy_vision_option() {
    RuntimeConfig config;
    for (unsigned i = 0; i < 33; ++i) {
        ServiceConfig service;
        service.service_name = "service" + std::to_string(i);
        service.executable = "/proc/self/exe";
        config.services.push_back(service);
        if (i < 32) config.health.services[service.service_name] = {ServiceCriticality::low, false};
    }
    ConfigManager::validate(config.health, config.services);
    require(config.health.services.size() == 32, "32-policy fixture wrong");
    config.health.services["service32"] = {ServiceCriticality::low, false};
    rejects([&] { ConfigManager::validate(config.health, config.services); });
    config.health.services.clear();
    config.health.services["missing"] = {ServiceCriticality::low, false};
    rejects([&] { ConfigManager::validate(config.health, config.services); });
    auto legacy = configuration();
    legacy.health = {};
    legacy.services.front().service_name = "vision_service";
    AggregationOptions options;
    options.vision_required = true;
    options.policies["vision_service"] = {ServiceCriticality::low, false};
    { RuntimeManager core(legacy, options, {}, ResourceInputMode::external);
      require(core.queryComponentHealth().empty(), "legacy programmatic option gained a component"); }
    legacy.health.services = options.policies;
    rejects([&] { RuntimeManager core(legacy, options); });
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--producer") return producer(argv[2], argv[3]);
        test_strict_health_parser_and_direction();
        test_async_ack_queue_cap_tombstone_and_control_fairness();
        test_integer_null_utf8_and_exact_payload_boundaries();
        test_real_peer_bound_heartbeat_and_committed_query();
        test_reconnect_replay_revision_and_epoch_isolation();
        test_unknown_registry_duplicate_bind_and_normalized_owner_failures();
        test_restart_runtime_epoch_and_old_instance_report();
        test_new_launch_rejects_previous_session_and_resets_owner_bounds();
        test_subscription_and_link_loss_preserve_process_identity();
        test_disconnect_and_process_exit_are_independent();
        test_health_config_legacy_strict_schema_and_wrapper();
        test_config_policy_merges_and_conflicts();
        test_config_registry_limits_and_legacy_vision_option();
        std::cout << "component IPC tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "component IPC tests: " << error.what() << '\n';
        return 1;
    }
}
