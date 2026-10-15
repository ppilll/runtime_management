#pragma once

#include "runtime/health_policy.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace runtime {

using ComponentClock = std::chrono::steady_clock;
inline constexpr std::chrono::milliseconds component_report_interval{1000};
inline constexpr std::chrono::milliseconds component_observation_lease{3500};
inline constexpr std::size_t component_ingress_limit = 32;
inline constexpr std::size_t component_payload_limit = 4096;

enum class ComponentHealthStatus { unknown, connecting, healthy, degraded, unhealthy };
enum class ComponentReason {
    initial, opening, negotiating, ready, response_timeout, link_lost, open_failed,
    hello_rejected, startup_not_ready, peer_reboot, recovered, observation_unavailable,
    report_expired, owner_stopped
};
enum class ComponentSampleKind { current, issue };

const char* component_status_name(ComponentHealthStatus status);
const char* component_reason_name(ComponentReason reason);

struct ComponentPolicyEntry {
    ComponentPolicyEntry(std::string key, std::string producer, ServiceHealthPolicy health_policy)
        : component(std::move(key)), producer_service(std::move(producer)), policy(health_policy) {}
    std::string component;
    std::string producer_service;
    ServiceHealthPolicy policy; // Explicit; no service-name-derived component policy.
};

struct ComponentSample {
    std::string component;
    ComponentHealthStatus status = ComponentHealthStatus::unknown;
    ComponentReason reason_code = ComponentReason::initial;
    std::string reason;
    std::uint64_t projection_revision = 0;
    std::uint64_t link_epoch = 0;
    std::optional<std::uint32_t> peer_boot_id; // Zero is a valid ID, not null.
    ComponentSampleKind sample_kind = ComponentSampleKind::current;
};

bool same_component_sample(const ComponentSample& a, const ComponentSample& b);
bool valid_component_sample(const ComponentSample& sample); // Owner reasons only, UTF-8 <=256 bytes.

struct ComponentIdentity {
    std::string service_name;
    int pid = -1;
    std::uint64_t launched_generation = 0;
    std::string runtime_epoch;
    std::uint64_t session_id = 0;
};

struct ComponentHealthFact {
    ComponentSample sample;
    ComponentIdentity identity;
    std::optional<ComponentClock::time_point> received_at;
    bool observation_fresh = false;
};

struct ComponentHealthSnapshot {
    ComponentHealthFact fact;
    ServiceHealthPolicy policy;
    std::optional<std::chrono::milliseconds> report_age;
};

// Created by the transport from SO_PEERCRED and server monotonic time. Neither
// the producer JSON nor public Runtime post() may supply this capability.
struct ComponentPeer {
    int pid = -1;
    unsigned uid = 0;
    unsigned gid = 0;
    std::uint64_t connection_token = 0;
    ComponentClock::time_point accepted_at{};
    std::atomic<bool> revoked{false};
    std::atomic<bool> pending{false};
};

struct ComponentReport {
    std::string runtime_epoch;
    std::uint64_t session_id = 0;
    std::uint64_t report_seq = 0;
    ComponentSample sample;
};

struct ComponentReply {
    unsigned error = 0; // Existing 1001..1003 and identity/RETRY_BIND 1004.
    std::uint32_t request_id = 0;
    std::string op;
    std::string result;
    ComponentIdentity identity;
    std::uint64_t report_seq = 0;
    std::uint64_t projection_revision = 0;
};

class ComponentReplySink {
public:
    virtual ~ComponentReplySink() = default;
    // Writer callback must not throw; enqueue a bounded reply, never socket I/O.
    virtual void completed(const ComponentReply& reply) = 0;
};

enum class ComponentIngressKind { bind, report, heartbeat };
struct ComponentIngress {
    ComponentIngressKind kind = ComponentIngressKind::bind;
    std::shared_ptr<ComponentPeer> peer;
    std::string service_name;
    ComponentReport report;
    ComponentClock::time_point received_at{};
    std::uint32_t request_id = 0;
    std::weak_ptr<ComponentReplySink> reply;
};

} // namespace runtime
