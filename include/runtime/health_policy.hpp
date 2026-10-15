#pragma once

namespace runtime {

enum class ServiceCriticality { high, medium, low };

struct ServiceHealthPolicy {
    ServiceCriticality criticality = ServiceCriticality::low;
    bool required = false; // Readiness and failure severity are independent.
};

inline bool valid_health_policy(const ServiceHealthPolicy& policy) {
    return policy.criticality == ServiceCriticality::high ||
           policy.criticality == ServiceCriticality::medium || policy.criticality == ServiceCriticality::low;
}

} // namespace runtime
