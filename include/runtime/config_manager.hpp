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
};

class ConfigManager {
public:
    static std::vector<ServiceConfig> load_file(const std::string& path);
};

} // namespace runtime
