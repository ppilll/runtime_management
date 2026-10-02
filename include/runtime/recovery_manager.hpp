#pragma once

#include "runtime/recovery.hpp"
#include "runtime/config_manager.hpp"
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace runtime {

// All mutations belong to one Runtime writer; query returns an independent copy.
class RecoveryManager {
public:
    RecoveryManager(std::vector<std::string> services, const RecoveryExecutor& executor);
    using Now = std::function<RecoveryClock::time_point()>;
    using LaunchGate = std::function<bool()>;
    // Definitions must be in SM startup_order (topology/name tie). Production
    // must use this constructor; the names-only constructor is the T1 identity harness.
    RecoveryManager(std::vector<ServiceConfig> definitions, RecoveryExecutor& executor,
                    Now clock = [] { return RecoveryClock::now(); }, LaunchGate launch_gate = [] { return true; });
    RecoveryAdmission submit(RecoveryRequest request, RecoveryClock::time_point now);
    ExitDisposition classifyExit(const std::string& name, int raw_status) const;
    void tick(RecoveryClock::time_point now);

    // The identity harness accepts a supplied duration. Configured RM always
    // applies its static policy/timeout; this overload cannot bypass them.
    RecoveryAdmission submit(RecoveryRequest request, RecoveryClock::time_point now,
                             RecoveryClock::duration timeout);
    std::optional<RecoveryOperation> beginExecution(const std::string& service_name,
                                                   std::uint64_t recovery_generation,
                                                   RecoveryClock::time_point now);
    // Stop/reap preparation can advance the expected lifecycle token. This is
    // captured operation metadata, not a new failure/admission.
    bool observePreparation(const RecoveryOperation& operation, std::uint64_t generation);
    bool bindExecution(const RecoveryOperation& operation, std::uint64_t execution_generation,
                       int launched_pid, bool launch_succeeded);
    bool observeFailure(const std::string& service_name, const RecoveryContext& context,
                        std::uint64_t fault_generation, FailureType type, const std::string& reason);
    bool observeFailure(const std::string&, const RecoveryContext&, std::uint64_t,
                        FailureType, const std::string&, RecoveryClock::time_point now);

    // Both the inspection and actual finish use the same full predicate.
    // completed_at is normalized to this writer checkpoint, never trusted.
    bool acceptsResult(const RecoveryResult& result, RecoveryClock::time_point now) const;
    bool observe(const RecoveryResult& result, RecoveryClock::time_point now);
    bool cancel(const std::string& service_name, RecoveryTerminalReason reason,
                RecoveryClock::time_point now);
    void cancelAll(RecoveryClock::time_point now); // Permanent shutdown admission gate.
    void stopAutomatic(RecoveryClock::time_point now); // OFFLINE; manual lifecycle commands remain available.
    std::optional<RecoverySnapshot> query(const std::string& service_name) const;
    // Canonical, sealed finish receipts. Drain per writer turn; do not repost
    // them through the external active-result gate after the active is removed.
    std::vector<RecoveryResult> takeResults();
    std::vector<RecoveryStart> takeStarts();
    static std::uint64_t nextGeneration(std::uint64_t previous);

private:
    bool acceptsLocked(const RecoveryResult& result, const RecoveryExecutionSnapshot& current,
                       const RecoverySnapshot& slot, RecoveryClock::time_point now) const;
    void finishLocked(RecoverySnapshot& slot, RecoveryResult result, RecoveryClock::time_point now);
    void cancelLocked(RecoverySnapshot& slot, RecoveryTerminalReason reason, RecoveryClock::time_point now);
    bool observeFailureCore(const std::string&, const RecoveryContext&, std::uint64_t, FailureType, const std::string&);
    bool terminal(const std::string&, RecoveryOutcome, RecoveryTerminalReason, RecoveryClock::time_point);
    void reserveLocked(RecoverySnapshot&, RecoveryClock::time_point);
    void prepare(const std::string&, RecoveryClock::time_point);
    bool configured() const { return writer_executor_ != nullptr; }
    const RecoveryExecutor& executor_; // Must outlive this core and all writer use.
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RecoverySnapshot> services_;
    std::vector<RecoveryResult> completed_; // Transient output work, not history.
    std::vector<RecoveryStart> starts_; // One receipt per admitted automatic episode with an attempt.
    bool shutdown_ = false;
    bool automatic_stopped_ = false;
    RecoveryExecutor* writer_executor_ = nullptr;
    std::unordered_map<std::string, ServiceConfig> definitions_;
    std::vector<std::string> order_;
    Now clock_;
    LaunchGate launch_gate_;
};

} // namespace runtime
