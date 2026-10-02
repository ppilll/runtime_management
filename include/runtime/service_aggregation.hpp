#pragma once

#include "runtime/device_state_manager.hpp"
#include "runtime/event.hpp"
#include <map>
#include <string>

namespace runtime {

enum class ServiceCriticality { high, medium, low };

struct ServiceHealthPolicy {
    ServiceCriticality criticality = ServiceCriticality::low;
    bool required = false; // Gates initial readiness, separate from failure severity.
};

struct AggregationOptions {
    bool vision_required = false;
    // Static overrides, supplied when constructing RuntimeManager; no rule engine.
    std::map<std::string, ServiceHealthPolicy> policies;
};

// All mutations are serialized on the runtime event loop. Queries go through
// DeviceStateManager, which owns the mutex-protected public snapshot.
class ServiceAggregation {
public:
    explicit ServiceAggregation(DeviceStateManager& states, AggregationOptions options = {});
    void add_service(const std::string& name, bool autostart = false);
    bool handle(const RuntimeEvent& event); // False for invalid metadata/type/service.
    void refresh(Clock::time_point at = Clock::now());

private:
    struct Health {
        ServiceHealthPolicy policy;
        bool running = false;
        bool failed = false;
        bool stopped = false;
        bool heartbeat_lost = false;
        bool recovering = false;
        std::uint64_t generation = 0;
    };
    DeviceState evaluate() const;
    void reconcile(const RuntimeEvent& event);
    void transition(DeviceStateEventType type, DeviceState target, const RuntimeEvent& event);

    DeviceStateManager& states_;
    const AggregationOptions options_;
    std::map<std::string, Health> services_; // Stable iteration for priority evaluation.
    std::map<std::string, ResourceSeverity> resource_warnings_;
    bool device_recovering_ = false;
    bool recovery_failed_ = false;
};

} // namespace runtime
