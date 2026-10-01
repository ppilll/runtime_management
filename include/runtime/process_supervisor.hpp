#pragma once

#include "runtime/config_manager.hpp"
#include <string>
#include <vector>

namespace runtime {

struct ProcessExit {
    int pid;
    int status;
};

class ProcessSupervisor {
public:
    virtual ~ProcessSupervisor() = default;
    virtual int start(const ServiceConfig& config) = 0;
    virtual void stop(int pid) = 0;
    // Escalation after the service manager's graceful-stop deadline.
    virtual void force_stop(int pid) = 0;
    virtual std::vector<ProcessExit> reap() = 0;
};

class PosixProcessSupervisor final : public ProcessSupervisor {
public:
    ~PosixProcessSupervisor() override;
    int start(const ServiceConfig& config) override;
    void stop(int pid) override;
    void force_stop(int pid) override;
    std::vector<ProcessExit> reap() override;

private:
    std::vector<int> children_;
};

} // namespace runtime
