#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace runtime {

enum class RestartPolicy { never, on_failure, always };

struct ServiceConfig {
    std::string service_name;
    std::string executable;
    std::vector<std::string> arguments;
    bool autostart = false;
    std::vector<std::string> dependency;
    std::chrono::seconds startup_timeout{15};
    std::chrono::seconds heartbeat_timeout{15};
    RestartPolicy restart_policy = RestartPolicy::never;
    // Optional Phase 2 fields; empty environment/directory inherit from Runtime.
    std::vector<std::string> environment; // KEY=VALUE entries
    std::string working_directory;
    std::chrono::seconds shutdown_timeout{2};
    std::optional<std::chrono::seconds> recovery_timeout;
};

struct MonitoringConfig {
    std::chrono::seconds sample_interval_seconds{2};
    double cpu_warning = 80;
    double cpu_clear = 75;
    double memory_warning = 80;
    double memory_clear = 75;
    double memory_critical = 95;
    double memory_critical_clear = 90;
};

struct RuntimeConfig {
    std::vector<ServiceConfig> services;
    MonitoringConfig monitoring;
};

class ConfigManager {
public:
    static std::vector<ServiceConfig> load_file(const std::string& path);
    static RuntimeConfig load_runtime_file(const std::string& path);
    static void validate(const MonitoringConfig& config);
    // Shared by JSON, ServiceManager registration and RecoveryManager.
    static void validate(const ServiceConfig& config);
    static std::chrono::seconds recoveryTimeout(const ServiceConfig& config);
};

} // namespace runtime
