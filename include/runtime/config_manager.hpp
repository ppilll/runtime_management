#pragma once

#include <chrono>
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
};

class ConfigManager {
public:
    static std::vector<ServiceConfig> load_file(const std::string& path);
};

} // namespace runtime
