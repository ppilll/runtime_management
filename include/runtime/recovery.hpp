#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace runtime {

// The same steady clock as the existing EventQueue; producer times are metadata.
using RecoveryClock = std::chrono::steady_clock;

enum class FailureType {
    process_crash, clean_exit_restart, heartbeat_timeout, startup_failure,
    startup_timeout, manual_request
};
enum class RecoveryOrigin { automatic_failure, manual_restart };
enum class RecoveryState { backoff, executing }; // No active record means IDLE.
enum class RecoveryOutcome { success, failed, timeout, cancelled };
enum class RecoveryTerminalReason {
    completed, retry_exhausted, recovery_timeout, execution_failure,
    explicit_stop, dependency_stop, shutdown, superseded, superseded_manual
};
enum class RecoveryAdmission { admitted, coalesced, suppressed_policy, stale, rejected_shutdown, invalid };

struct RecoveryContext {
    std::uint64_t recovery_generation = 0;
    std::uint64_t initial_fault_generation = 0;
    std::optional<std::uint64_t> execution_generation;
    RecoveryOrigin origin = RecoveryOrigin::automatic_failure;
};

struct RecoveryRequest {
    std::string service_name;
    RecoveryOrigin origin = RecoveryOrigin::automatic_failure;
    FailureType failure_type = FailureType::process_crash;
    std::string reason;
    std::uint64_t service_generation = 0;
    RecoveryClock::time_point producer_time{};
    // Filled exclusively by admission; producers must leave these unset.
    std::uint64_t recovery_generation = 0;
    RecoveryClock::time_point accepted_at{};
    RecoveryClock::time_point deadline{};
};

struct RecoveryResult {
    std::string service_name;
    std::uint64_t recovery_generation = 0;
    RecoveryOrigin origin = RecoveryOrigin::automatic_failure;
    std::uint64_t initial_fault_generation = 0;
    std::uint64_t latest_fault_generation = 0;
    std::optional<std::uint64_t> execution_generation;
    RecoveryOutcome outcome = RecoveryOutcome::failed;
    RecoveryTerminalReason terminal_reason = RecoveryTerminalReason::execution_failure;
    unsigned attempts_reserved_total = 0;
    RecoveryClock::time_point completed_at{};
    std::optional<int> launched_pid;
    // Bounded duplicate-fault identity retained with the sealed last result.
    FailureType failure_type = FailureType::process_crash;
    FailureType latest_failure_type = FailureType::process_crash;
};

// Captured when RM authorizes an operation, before any SM lifecycle mutation.
// expected_current_generation is the begin checkpoint. Later phases (manual
// stop -> launch) use their captured preparation token as the primitive's
// expected generation, while echoing this original operation for correlation.
// Never query a new token at completion to relabel an earlier operation.
struct RecoveryOperation {
    std::string service_name;
    std::uint64_t expected_current_generation = 0;
    RecoveryContext context;
    RecoveryClock::time_point deadline{};
    bool finalize_only = false;
};

// Read-only executor projection. SM remains the lifecycle/PID/token owner.
enum class RecoveryExecutionState { unavailable, starting, running, stopping, stopped, failed, recovering };
struct RecoveryExecutionSnapshot {
    std::uint64_t generation = 0;
    std::uint64_t launched_generation = 0;
    int pid = -1;
    RecoveryExecutionState state = RecoveryExecutionState::unavailable;
};

enum class ExitDisposition { normal_stopped, recoverable_failure };
enum class RecoveryStep { blocked, prepared, launched, failed, invalid };
struct RecoveryExecutionReply {
    RecoveryStep step = RecoveryStep::invalid;
    RecoveryExecutionSnapshot captured;
    FailureType failure_type = FailureType::startup_failure;
};

class RecoveryExecutor {
public:
    virtual ~RecoveryExecutor() = default;
    // A query only: no callback, scheduling, lifecycle mutation or RM reentry.
    virtual std::optional<RecoveryExecutionSnapshot> snapshot(const std::string& service_name) const = 0;
    // Writer-local primitives. Callbacks may enqueue facts, never reenter RM.
    virtual RecoveryExecutionReply prepareRecovery(const RecoveryOperation&, unsigned, RecoveryClock::time_point) { return {}; }
    virtual bool recoveryReady(const RecoveryOperation&, RecoveryClock::time_point,
                               const std::function<bool()>&) { return false; }
    virtual RecoveryExecutionReply launchRecoveryAttempt(const RecoveryOperation&, std::uint64_t,
        RecoveryClock::duration, RecoveryClock::time_point) { return {}; }
    virtual RecoveryExecutionReply finishRecoveryFailure(const RecoveryOperation&, std::uint64_t,
        RecoveryTerminalReason, RecoveryClock::time_point) { return {}; }
    virtual void releaseRecovery(const std::string&, const RecoveryContext&) {}
    virtual void projectRestartCount(const std::string&, unsigned) {}
};

struct RecoveryActiveRecord {
    RecoveryRequest request;
    RecoveryState state = RecoveryState::backoff;
    RecoveryContext context;
    std::uint64_t expected_generation = 0;
    std::uint64_t latest_fault_generation = 0;
    FailureType latest_failure_type = FailureType::process_crash;
    std::optional<RecoveryOperation> operation;
    std::optional<int> launched_pid;
    bool launch_succeeded = false;
    std::optional<RecoveryClock::time_point> due;
    bool prepared = false;
    bool finalizing = false;
};

struct RecoverySnapshot {
    std::uint64_t recovery_generation = 0;
    unsigned attempts_reserved_total = 0;
    std::optional<RecoveryActiveRecord> active;
    std::optional<RecoveryResult> last_result;
};

// Transient canonical admission receipt, owned and issued only by RM.
struct RecoveryStart {
    std::string service_name;
    RecoveryContext context;
    std::uint64_t generation = 0;
    RecoveryClock::time_point at{};
};

} // namespace runtime
