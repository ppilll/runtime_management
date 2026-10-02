#include "runtime/recovery_manager.hpp"
#include <limits>
#include <algorithm>
#include <sys/wait.h>
#include <stdexcept>
#include <utility>

namespace runtime {
namespace {

bool validOrigin(RecoveryOrigin origin) {
    return origin == RecoveryOrigin::automatic_failure || origin == RecoveryOrigin::manual_restart;
}

bool automaticFailure(FailureType type) {
    switch (type) {
    case FailureType::process_crash:
    case FailureType::clean_exit_restart:
    case FailureType::heartbeat_timeout:
    case FailureType::startup_failure:
    case FailureType::startup_timeout: return true;
    case FailureType::manual_request: return false;
    }
    return false;
}

bool cancellation(RecoveryTerminalReason reason) {
    switch (reason) {
    case RecoveryTerminalReason::explicit_stop:
    case RecoveryTerminalReason::dependency_stop:
    case RecoveryTerminalReason::shutdown:
    case RecoveryTerminalReason::superseded:
    case RecoveryTerminalReason::superseded_manual: return true;
    default: return false;
    }
}

bool terminalEligibleState(RecoveryExecutionState state) {
    return state == RecoveryExecutionState::failed || state == RecoveryExecutionState::recovering ||
           state == RecoveryExecutionState::starting || state == RecoveryExecutionState::running;
}

bool sameContext(const RecoveryContext& a, const RecoveryContext& b) {
    return a.recovery_generation == b.recovery_generation &&
           a.initial_fault_generation == b.initial_fault_generation &&
           a.execution_generation == b.execution_generation && a.origin == b.origin;
}

bool sameOperation(const RecoveryOperation& a, const RecoveryOperation& b) {
    return a.service_name == b.service_name &&
           a.expected_current_generation == b.expected_current_generation && sameContext(a.context, b.context) &&
           a.deadline == b.deadline && a.finalize_only == b.finalize_only;
}

RecoveryResult resultFor(const RecoveryActiveRecord& active, unsigned reserved,
                         RecoveryOutcome outcome, RecoveryTerminalReason reason) {
    RecoveryResult result;
    result.service_name = active.request.service_name;
    result.recovery_generation = active.context.recovery_generation;
    result.origin = active.context.origin;
    result.initial_fault_generation = active.context.initial_fault_generation;
    result.latest_fault_generation = active.latest_fault_generation;
    result.execution_generation = active.context.execution_generation;
    result.outcome = outcome;
    result.terminal_reason = reason;
    result.attempts_reserved_total = reserved;
    result.launched_pid = active.launched_pid;
    result.failure_type = active.request.failure_type;
    result.latest_failure_type = active.latest_failure_type;
    return result;
}

} // namespace

RecoveryManager::RecoveryManager(std::vector<std::string> services, const RecoveryExecutor& executor)
    : executor_(executor) {
    for (const auto& name : services) {
        if (name.empty() || !services_.emplace(name, RecoverySnapshot{}).second)
            throw std::invalid_argument("invalid recovery service registry");
    }
}

std::uint64_t RecoveryManager::nextGeneration(std::uint64_t previous) {
    if (previous == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("recovery generation exhausted");
    return previous + 1;
}

RecoveryAdmission RecoveryManager::submit(RecoveryRequest request, RecoveryClock::time_point now,
                                          RecoveryClock::duration timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutdown_) return RecoveryAdmission::rejected_shutdown;
    if (automatic_stopped_ && request.origin == RecoveryOrigin::automatic_failure)
        return RecoveryAdmission::suppressed_policy;
    const auto found = services_.find(request.service_name);
    if (found == services_.end() || !validOrigin(request.origin) || request.reason.empty() ||
        request.service_generation == 0 || request.recovery_generation != 0 ||
        request.accepted_at != RecoveryClock::time_point{} || request.deadline != RecoveryClock::time_point{} ||
        (request.origin == RecoveryOrigin::automatic_failure ? !automaticFailure(request.failure_type)
                                                            : request.failure_type != FailureType::manual_request) ||
        timeout <= RecoveryClock::duration::zero()) return RecoveryAdmission::invalid;
    if (configured()) {
        const auto& definition = definitions_.at(request.service_name);
        if (request.origin == RecoveryOrigin::automatic_failure &&
            (definition.restart_policy == RestartPolicy::never ||
             (request.failure_type == FailureType::clean_exit_restart && definition.restart_policy != RestartPolicy::always)))
            return RecoveryAdmission::suppressed_policy;
        timeout = ConfigManager::recoveryTimeout(definition);
    }
    if (now > RecoveryClock::time_point::max() - timeout)
        throw std::overflow_error("recovery deadline out of range");
    auto& slot = found->second;
    const auto current = executor_.snapshot(request.service_name);
    if (!current || current->generation == 0) return RecoveryAdmission::stale;
    if (slot.active) {
        const auto& active = *slot.active;
        // Identical admitted facts can arrive after the launch bridge. They
        // coalesce only while the executor still matches that active binding.
        const bool duplicate_manual = request.origin == RecoveryOrigin::manual_restart &&
                                      active.request.origin == RecoveryOrigin::manual_restart;
        const bool duplicate_fault = request.origin == active.request.origin &&
            request.failure_type == active.request.failure_type &&
            request.service_generation == active.request.service_generation;
        if ((duplicate_manual || duplicate_fault) && current->generation == active.expected_generation &&
            (request.service_generation == active.request.service_generation ||
             request.service_generation == active.expected_generation)) return RecoveryAdmission::coalesced;
        // A bound attempt failure belongs to observeFailure(), not submit().
        if (request.origin == RecoveryOrigin::automatic_failure &&
            active.request.origin == RecoveryOrigin::automatic_failure &&
            request.service_generation == active.latest_fault_generation &&
            request.failure_type == active.latest_failure_type &&
            current->generation == active.expected_generation) return RecoveryAdmission::coalesced;
        // A different description/type cannot create a new episode on an
        // already bound lifecycle token. A new independent fault needs a new token.
        if (request.origin == RecoveryOrigin::automatic_failure &&
            request.service_generation <= active.expected_generation) return RecoveryAdmission::stale;
    }
    if (request.service_generation != current->generation) return RecoveryAdmission::stale;
    if (request.origin == RecoveryOrigin::automatic_failure &&
        current->state != RecoveryExecutionState::failed) return RecoveryAdmission::stale;
    if (!slot.active && request.origin == RecoveryOrigin::automatic_failure && slot.last_result &&
        slot.last_result->origin == request.origin &&
        ((slot.last_result->initial_fault_generation == request.service_generation &&
          slot.last_result->failure_type == request.failure_type) ||
         (slot.last_result->latest_fault_generation == request.service_generation &&
          slot.last_result->latest_failure_type == request.failure_type))) return RecoveryAdmission::coalesced;
    if (!slot.active && request.origin == RecoveryOrigin::automatic_failure && slot.last_result &&
        slot.last_result->origin == RecoveryOrigin::automatic_failure &&
        request.service_generation <= slot.last_result->latest_fault_generation) return RecoveryAdmission::stale;
    // Check overflow before superseding the old episode; it must not partially
    // cancel an active operation then fail to allocate its replacement identity.
    if (configured() && request.origin == RecoveryOrigin::automatic_failure && slot.attempts_reserved_total < 5) {
        const auto delay = std::chrono::seconds(std::min(2u << slot.attempts_reserved_total, 60u));
        if (now > RecoveryClock::time_point::max() - delay)
            throw std::overflow_error("recovery due out of range");
    }
    const auto generation = nextGeneration(slot.recovery_generation);
    request.recovery_generation = generation;
    request.accepted_at = now;
    request.deadline = now + timeout;
    RecoveryActiveRecord active;
    active.request = std::move(request);
    active.state = active.request.origin == RecoveryOrigin::automatic_failure ? RecoveryState::backoff
                                                                            : RecoveryState::executing;
    active.context = RecoveryContext{generation, active.request.service_generation, {}, active.request.origin};
    active.expected_generation = active.request.service_generation;
    active.latest_fault_generation = active.request.service_generation;
    active.latest_failure_type = active.request.failure_type;
    const auto old_context = slot.active ? std::optional<RecoveryContext>(slot.active->context) : std::nullopt;
    if (slot.active) cancelLocked(slot, active.request.origin == RecoveryOrigin::manual_restart
                                       ? RecoveryTerminalReason::superseded_manual
                                       : RecoveryTerminalReason::superseded, now);
    slot.recovery_generation = generation;
    slot.active = std::move(active);
    const auto name = slot.active->request.service_name;
    if (configured() && slot.active->context.origin == RecoveryOrigin::automatic_failure)
        reserveLocked(slot, now);
    lock.unlock();
    if (configured()) {
        if (old_context) writer_executor_->releaseRecovery(name, *old_context);
        prepare(name, now);
        const auto admitted = query(name);
        if (admitted && admitted->active && admitted->active->prepared && admitted->active->due &&
            admitted->active->context.origin == RecoveryOrigin::automatic_failure) {
            std::lock_guard<std::mutex> receipt_lock(mutex_);
            starts_.push_back({name, admitted->active->context, admitted->active->expected_generation, now});
        }
        if (admitted && admitted->active && admitted->active->context.origin == RecoveryOrigin::automatic_failure &&
            !admitted->active->due)
            terminal(name, RecoveryOutcome::failed, RecoveryTerminalReason::retry_exhausted, now);
    }
    return RecoveryAdmission::admitted;
}

std::optional<RecoveryOperation> RecoveryManager::beginExecution(const std::string& name,
                                                                std::uint64_t generation,
                                                                RecoveryClock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(name);
    if (shutdown_ || found == services_.end() || !found->second.active) return {};
    auto& active = *found->second.active;
    if (generation == 0 || generation != active.context.recovery_generation ||
        now >= active.request.deadline || active.finalizing || active.operation || active.context.execution_generation) return {};
    const auto current = executor_.snapshot(name);
    if (!current || current->generation != active.expected_generation) return {};
    if (configured() && (!active.prepared || current->pid > 0 ||
        (active.context.origin == RecoveryOrigin::automatic_failure && (!active.due || now < *active.due)))) return {};
    // Captured authorization only; tick calls the checked SM primitive outside this lock.
    RecoveryOperation operation{name, active.expected_generation, active.context, active.request.deadline};
    active.operation = operation;
    active.due.reset();
    active.state = RecoveryState::executing;
    return operation;
}

bool RecoveryManager::observePreparation(const RecoveryOperation& operation, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(operation.service_name);
    if (shutdown_ || found == services_.end() || !found->second.active) return false;
    auto& active = *found->second.active;
    if (!active.operation || !sameOperation(operation, *active.operation) ||
        active.context.execution_generation || generation == 0 || generation < active.expected_generation) return false;
    const auto current = executor_.snapshot(operation.service_name);
    if (!current || current->generation != generation ||
        (current->state != RecoveryExecutionState::stopping && current->state != RecoveryExecutionState::stopped &&
         current->state != RecoveryExecutionState::recovering)) return false;
    active.expected_generation = generation;
    return true;
}

bool RecoveryManager::bindExecution(const RecoveryOperation& operation, std::uint64_t generation,
                                    int pid, bool succeeded) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(operation.service_name);
    if (shutdown_ || found == services_.end() || !found->second.active) return false;
    auto& active = *found->second.active;
    if (!active.operation || !sameOperation(operation, *active.operation) || generation == 0 || pid <= 0) return false;
    if (active.context.execution_generation && *active.context.execution_generation != generation) return false;
    if (!active.context.execution_generation && generation <= active.expected_generation) return false;
    const auto current = executor_.snapshot(operation.service_name);
    if (!current || current->generation != generation || current->launched_generation != generation ||
        current->pid != pid || (succeeded ? current->state != RecoveryExecutionState::running
                                         : current->state != RecoveryExecutionState::starting)) return false;
    if (active.launched_pid && *active.launched_pid != pid) return false;
    if (active.launch_succeeded && !succeeded) return false;
    active.context.execution_generation = generation;
    active.expected_generation = generation;
    active.launched_pid = pid;
    active.launch_succeeded = succeeded;
    return true;
}

bool RecoveryManager::observeFailure(const std::string& name, const RecoveryContext& context,
        std::uint64_t generation, FailureType type, const std::string& reason) {
    if (configured()) return observeFailure(name, context, generation, type, reason, clock_());
    return observeFailureCore(name, context, generation, type, reason);
}

bool RecoveryManager::observeFailureCore(const std::string& name, const RecoveryContext& context,
                                     std::uint64_t generation, FailureType type, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(name);
    if (shutdown_ || found == services_.end() || !found->second.active || !automaticFailure(type) || reason.empty())
        return false;
    auto& active = *found->second.active;
    if (active.finalizing || !active.operation || !sameContext(context, active.context) ||
        generation == 0 || generation <= active.expected_generation) return false;
    const auto current = executor_.snapshot(name);
    if (!current || current->generation != generation || current->state != RecoveryExecutionState::failed ||
        (context.execution_generation && current->pid > 0 &&
         current->launched_generation != *context.execution_generation)) return false;
    active.expected_generation = generation;
    active.latest_fault_generation = generation;
    active.latest_failure_type = type;
    active.context.execution_generation.reset(); // Old execution can never finish this episode.
    active.operation.reset();
    active.launched_pid.reset();
    active.launch_succeeded = false;
    active.state = RecoveryState::backoff;
    return true;
}

bool RecoveryManager::acceptsLocked(const RecoveryResult& result, const RecoveryExecutionSnapshot& current,
                                    const RecoverySnapshot& slot, RecoveryClock::time_point now) const {
    if (shutdown_ || !slot.active) return false;
    const auto& active = *slot.active;
    if (active.finalizing) return false;
    if (result.service_name != active.request.service_name || result.recovery_generation == 0 ||
        result.recovery_generation != active.context.recovery_generation || result.origin != active.context.origin ||
        result.initial_fault_generation == 0 || result.initial_fault_generation != active.context.initial_fault_generation ||
        result.latest_fault_generation != active.latest_fault_generation ||
        result.execution_generation != active.context.execution_generation ||
        result.attempts_reserved_total != slot.attempts_reserved_total || result.failure_type != active.request.failure_type ||
        result.latest_failure_type != active.latest_failure_type ||
        result.launched_pid != active.launched_pid || current.generation != active.expected_generation) return false;
    if (result.execution_generation && current.pid > 0 &&
        current.launched_generation != *result.execution_generation) return false;
    switch (result.outcome) {
    case RecoveryOutcome::success:
        return result.terminal_reason == RecoveryTerminalReason::completed &&
               active.state == RecoveryState::executing && active.operation && active.launch_succeeded &&
               result.execution_generation && *result.execution_generation != 0 && result.launched_pid &&
               *result.launched_pid > 0 && current.generation == *result.execution_generation &&
               current.launched_generation == *result.execution_generation && current.pid == *result.launched_pid &&
               current.state == RecoveryExecutionState::running && now < active.request.deadline;
    case RecoveryOutcome::failed:
        return !configured() && (result.terminal_reason == RecoveryTerminalReason::retry_exhausted ||
                result.terminal_reason == RecoveryTerminalReason::execution_failure) &&
               terminalEligibleState(current.state) &&
               now < active.request.deadline;
    case RecoveryOutcome::timeout:
        return !configured() && result.terminal_reason == RecoveryTerminalReason::recovery_timeout &&
               terminalEligibleState(current.state) &&
               now >= active.request.deadline;
    case RecoveryOutcome::cancelled: return false; // Only explicit RM cancellation can mint this result.
    }
    return false;
}

bool RecoveryManager::acceptsResult(const RecoveryResult& result, RecoveryClock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(result.service_name);
    if (found == services_.end()) return false;
    const auto current = executor_.snapshot(result.service_name);
    return current && acceptsLocked(result, *current, found->second, now);
}

bool RecoveryManager::observe(const RecoveryResult& result, RecoveryClock::time_point now) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = services_.find(result.service_name);
    if (found == services_.end()) return false;
    const auto current = executor_.snapshot(result.service_name);
    if (!current || !acceptsLocked(result, *current, found->second, now)) return false;
    const auto context = found->second.active->context;
    finishLocked(found->second, result, now);
    lock.unlock();
    if (configured()) writer_executor_->releaseRecovery(result.service_name, context);
    return true;
}

void RecoveryManager::finishLocked(RecoverySnapshot& slot, RecoveryResult result, RecoveryClock::time_point now) {
    result.completed_at = now;
    // Allocate output before mutating state. No callback can reenter lifecycle
    // execution between sealing the result and revoking the active binding.
    auto last = result;
    completed_.push_back(std::move(result));
    slot.active.reset();
    slot.last_result = std::move(last);
}

void RecoveryManager::cancelLocked(RecoverySnapshot& slot, RecoveryTerminalReason reason,
                                   RecoveryClock::time_point now) {
    finishLocked(slot, resultFor(*slot.active, slot.attempts_reserved_total, RecoveryOutcome::cancelled, reason), now);
}

bool RecoveryManager::cancel(const std::string& name, RecoveryTerminalReason reason, RecoveryClock::time_point now) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = services_.find(name);
    if (!cancellation(reason) || found == services_.end() || !found->second.active) return false;
    const auto context = found->second.active->context;
    cancelLocked(found->second, reason, now);
    lock.unlock();
    if (configured()) writer_executor_->releaseRecovery(name, context);
    return true;
}

void RecoveryManager::cancelAll(RecoveryClock::time_point now) {
    { std::lock_guard<std::mutex> lock(mutex_); shutdown_ = true; }
    for (const auto& entry : services_) cancel(entry.first, RecoveryTerminalReason::shutdown, now);
}

void RecoveryManager::stopAutomatic(RecoveryClock::time_point now) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (automatic_stopped_) return;
        automatic_stopped_ = true;
    }
    // Revoke every task pending at the OFFLINE checkpoint. Subsequent manual
    // commands may execute, while new automatic admissions stay suppressed.
    for (const auto& entry : services_) {
        const auto slot = query(entry.first);
        if (slot->active)
            cancel(entry.first, RecoveryTerminalReason::superseded, now);
    }
}

std::optional<RecoverySnapshot> RecoveryManager::query(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(name);
    if (found == services_.end()) return {};
    return found->second;
}

std::vector<RecoveryResult> RecoveryManager::takeResults() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecoveryResult> results;
    results.swap(completed_);
    return results;
}

std::vector<RecoveryStart> RecoveryManager::takeStarts() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecoveryStart> starts;
    starts.swap(starts_);
    return starts;
}

RecoveryManager::RecoveryManager(std::vector<ServiceConfig> definitions, RecoveryExecutor& executor, Now clock, LaunchGate launch_gate)
    : executor_(executor), writer_executor_(&executor), clock_(std::move(clock)), launch_gate_(std::move(launch_gate)) {
    if (!clock_ || !launch_gate_) throw std::invalid_argument("recovery clock is required");
    for (auto& definition : definitions) {
        ConfigManager::validate(definition);
        const auto name = definition.service_name;
        if (!services_.emplace(name, RecoverySnapshot{}).second)
            throw std::invalid_argument("duplicate recovery service");
        order_.push_back(name);
        definitions_.emplace(name, std::move(definition));
    }
}

RecoveryAdmission RecoveryManager::submit(RecoveryRequest request, RecoveryClock::time_point now) {
    if (!configured()) return RecoveryAdmission::invalid;
    const auto found = definitions_.find(request.service_name);
    if (found == definitions_.end()) return RecoveryAdmission::invalid;
    return submit(std::move(request), now, ConfigManager::recoveryTimeout(found->second));
}

ExitDisposition RecoveryManager::classifyExit(const std::string& name, int raw_status) const {
    const auto found = definitions_.find(name);
    if (found == definitions_.end()) throw std::invalid_argument("unknown recovery service");
    const bool clean = raw_status >= 0 && WIFEXITED(raw_status) && WEXITSTATUS(raw_status) == 0;
    return clean && found->second.restart_policy != RestartPolicy::always ?
        ExitDisposition::normal_stopped : ExitDisposition::recoverable_failure;
}

void RecoveryManager::reserveLocked(RecoverySnapshot& slot, RecoveryClock::time_point now) {
    constexpr unsigned maximum_reservations = 5;
    slot.active->due.reset(); // Exhaustion must revoke the previous attempt's due permission.
    if (slot.attempts_reserved_total >= maximum_reservations) return;
    const auto delay = std::chrono::seconds(std::min(2u << slot.attempts_reserved_total, 60u));
    if (now > RecoveryClock::time_point::max() - delay)
        throw std::overflow_error("recovery due out of range");
    ++slot.attempts_reserved_total; // Lifetime cumulative: never refunded or reset.
    slot.active->due = now + delay;
    slot.active->state = RecoveryState::backoff;
}

void RecoveryManager::prepare(const std::string& name, RecoveryClock::time_point now) {
    const auto slot = query(name);
    if (!slot || !slot->active) return;
    const auto& active = *slot->active;
    RecoveryOperation operation{name, active.expected_generation, active.context, active.request.deadline};
    operation.finalize_only = active.context.origin == RecoveryOrigin::automatic_failure && !active.due;
    const auto reply = writer_executor_->prepareRecovery(operation, slot->attempts_reserved_total, now);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& current = services_.at(name);
        if (!current.active || current.active->context.recovery_generation != operation.context.recovery_generation) return;
        if (reply.step == RecoveryStep::prepared && reply.captured.generation >= active.expected_generation) {
            current.active->expected_generation = reply.captured.generation;
            current.active->prepared = true;
        }
    }
    const auto checkpoint = std::max(now, clock_());
    if (checkpoint >= active.request.deadline)
        terminal(name, RecoveryOutcome::timeout, RecoveryTerminalReason::recovery_timeout, checkpoint);
    else if (reply.step != RecoveryStep::prepared)
        terminal(name, RecoveryOutcome::failed, RecoveryTerminalReason::execution_failure, checkpoint);
}

bool RecoveryManager::terminal(const std::string& name, RecoveryOutcome outcome,
                               RecoveryTerminalReason reason, RecoveryClock::time_point now) {
    RecoveryActiveRecord active;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& slot = services_.at(name);
        if (!slot.active || slot.active->finalizing) return false;
        // Timeout wins at the checkpoint. Revoke operation/due before calling SM.
        if (now >= slot.active->request.deadline) {
            outcome = RecoveryOutcome::timeout;
            reason = RecoveryTerminalReason::recovery_timeout;
        }
        slot.active->finalizing = true;
        slot.active->due.reset();
        active = *slot.active;
        slot.active->operation.reset();
    }
    const RecoveryOperation operation{name, active.expected_generation, active.context, active.request.deadline};
    const auto reply = writer_executor_->finishRecoveryFailure(operation, active.expected_generation, reason, now);
    now = std::max(now, clock_());
    if (now >= active.request.deadline) {
        outcome = RecoveryOutcome::timeout;
        reason = RecoveryTerminalReason::recovery_timeout;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    auto& slot = services_.at(name);
    if (!slot.active || slot.active->context.recovery_generation != active.context.recovery_generation) return false;
    if (reply.step == RecoveryStep::prepared && reply.captured.state == RecoveryExecutionState::failed &&
        reply.captured.generation >= active.expected_generation) {
        slot.active->expected_generation = reply.captured.generation;
        slot.active->latest_fault_generation = reply.captured.generation;
    } else {
        // Failed invariant finalization still seals exactly one diagnostic result;
        // no query of a new token can relabel an old execution.
        outcome = RecoveryOutcome::failed;
        reason = RecoveryTerminalReason::execution_failure;
    }
    finishLocked(slot, resultFor(*slot.active, slot.attempts_reserved_total, outcome, reason), now);
    lock.unlock();
    writer_executor_->releaseRecovery(name, active.context);
    return true;
}

bool RecoveryManager::observeFailure(const std::string& name, const RecoveryContext& context,
        std::uint64_t generation, FailureType type, const std::string& reason, RecoveryClock::time_point now) {
    if (!observeFailureCore(name, context, generation, type, reason)) return false;
    if (!configured()) return true;
    const auto slot = query(name);
    if (now >= slot->active->request.deadline)
        return terminal(name, RecoveryOutcome::timeout, RecoveryTerminalReason::recovery_timeout, now);
    if (context.origin == RecoveryOrigin::manual_restart) {
        terminal(name, RecoveryOutcome::failed, RecoveryTerminalReason::execution_failure, now);
        // The failed manual launch is a separate automatic fact, not an auto attempt.
        RecoveryRequest request;
        request.service_name = name;
        request.service_generation = generation;
        request.failure_type = type;
        request.reason = reason;
        submit(std::move(request), now);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reserveLocked(services_.at(name), now);
    }
    const auto reserved = query(name);
    writer_executor_->projectRestartCount(name, reserved->attempts_reserved_total);
    if (!reserved->active->due)
        terminal(name, RecoveryOutcome::failed, RecoveryTerminalReason::retry_exhausted, now);
    else {
        // Failed -> RECOVERING on the same episode, replacing the invalidated
        // execution context with the next captured fault context.
        writer_executor_->releaseRecovery(name, context);
        prepare(name, now);
    }
    return true;
}

void RecoveryManager::tick(RecoveryClock::time_point now) {
    if (!configured()) return;
    // First settle all absolute deadlines. At most one replacement launch per turn.
    bool settled_terminal = false;
    for (const auto& name : order_) {
        const auto slot = query(name);
        const auto current = executor_.snapshot(name);
        if (slot->active && current && current->generation != slot->active->expected_generation) {
            cancel(name, RecoveryTerminalReason::superseded, now);
            continue;
        }
        if (slot->active && now >= slot->active->request.deadline) {
            terminal(name, RecoveryOutcome::timeout, RecoveryTerminalReason::recovery_timeout, now);
            settled_terminal = true;
        }
    }
    // Give the writer a checkpoint to publish terminal health (possibly OFFLINE)
    // and cancel affected tasks before authorizing another service's launch.
    if (settled_terminal) return;
    std::vector<std::string> candidates;
    for (const auto& name : order_) {
        const auto slot = query(name);
        if (slot->active && !slot->active->finalizing && slot->active->prepared &&
            (!slot->active->due || now >= *slot->active->due)) candidates.push_back(name);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [&](const std::string& a, const std::string& b) {
        const auto x = query(a)->active;
        const auto y = query(b)->active;
        return x->due.value_or(x->request.accepted_at) < y->due.value_or(y->request.accepted_at);
    });
    for (const auto& name : candidates) {
        const auto slot = query(name);
        const auto active = *slot->active;
        const RecoveryOperation readiness{name, active.expected_generation, active.context, active.request.deadline};
        if (!launch_gate_()) { cancelAll(now); return; }
        const bool ready = writer_executor_->recoveryReady(readiness, now, launch_gate_);
        // Readiness may synchronously launch manual prerequisites. Check real return time.
        const auto checkpoint = std::max(now, clock_());
        if (checkpoint >= active.request.deadline) {
            terminal(name, RecoveryOutcome::timeout, RecoveryTerminalReason::recovery_timeout, checkpoint);
            return;
        }
        if (!ready) {
            // Manual readiness may launch one prerequisite; return to the writer
            // to drain its facts before any other recovery execution.
            if (active.context.origin == RecoveryOrigin::manual_restart) break;
            continue;
        }
        if (active.request.deadline - checkpoint < std::chrono::seconds(1)) continue;
        if (!launch_gate_()) { cancelAll(checkpoint); return; }
        const auto operation = beginExecution(name, active.context.recovery_generation, checkpoint);
        if (!operation) continue;
        const auto reply = writer_executor_->launchRecoveryAttempt(*operation, active.expected_generation,
            active.request.deadline - checkpoint, checkpoint);
        const auto returned_at = std::max(checkpoint, clock_());
        if (!launch_gate_()) { cancelAll(returned_at); return; }
        if (reply.step == RecoveryStep::launched &&
            bindExecution(*operation, reply.captured.generation, reply.captured.pid, true)) {
            if (returned_at >= active.request.deadline)
                terminal(name, RecoveryOutcome::timeout, RecoveryTerminalReason::recovery_timeout, returned_at);
            else {
                const auto bound = query(name);
                observe(resultFor(*bound->active, bound->attempts_reserved_total,
                    RecoveryOutcome::success, RecoveryTerminalReason::completed), returned_at);
            }
        } else if (reply.step == RecoveryStep::failed) {
            // The reply is captured before callbacks. No completion-time token query.
            if (!observeFailure(name, operation->context, reply.captured.generation,
                    reply.failure_type, "recovery launch failed", returned_at))
                terminal(name, RecoveryOutcome::failed, RecoveryTerminalReason::execution_failure, returned_at);
        } else if (reply.step == RecoveryStep::blocked) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& current = services_.at(name);
            if (current.active) {
                current.active->operation.reset();
                current.active->state = RecoveryState::backoff;
                current.active->due = checkpoint;
            }
        } else terminal(name, RecoveryOutcome::failed, RecoveryTerminalReason::execution_failure, returned_at);
        break;
    }
}

} // namespace runtime
