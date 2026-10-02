#pragma once

#include "runtime/config_manager.hpp"
#include <list>
#include <mutex>
#include <string>
#include <vector>

namespace runtime {

struct ProcessExit {
    int pid;
    int status; // Raw waitpid status; -1 if status was consumed externally.
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
    // Owns direct, foreground children. No other code may reap these children,
    // auto-reap SIGCHLD, or mutate environ while a launch snapshots it.
    // Public operations are serialized; finish calls before destruction.
    ~PosixProcessSupervisor() override;
    int start(const ServiceConfig& config) override;
    void stop(int pid) override;
    void force_stop(int pid) override;
    std::vector<ProcessExit> reap() override;

    // Phase 2 names retain the Phase 1 supervisor API used by ServiceManager.
    int launchProcess(const ServiceConfig& config) { return start(config); }
    // Sends SIGTERM; ServiceManager's shutdown_timeout escalates via force_stop.
    void terminateProcess(int pid) { stop(pid); }
    // Non-consuming checks: exit status remains available to reap().
    bool checkProcessAlive(int pid) const;
    // Returns the tracked PID (including an unreaped exit), or -1 if unknown.
    int getPid(const std::string& service_name) const;

private:
    struct Child {
        int pid = -1;
        int pid_fd = -1;
        std::string service_name;
    };
    void signal_child(int pid, int signal);
    mutable std::mutex mutex_;
    std::list<Child> children_;
};

} // namespace runtime
