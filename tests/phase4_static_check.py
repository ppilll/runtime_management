"""Phase4 source/contract audit, using Python's standard library only.

Checks declarations, assertions, registration, ownership and frozen interfaces.
Does not compile, execute C++, launch children, or open sockets. The matrix is
test-source coverage, never evidence that those C++ scenarios have passed.
"""
import ast
import re
import subprocess
import sys

from phase2_static_check import ROOT, check_source, check_registration, masked_source, require
from phase3_validation_static_check import (check_explicit_target_coverage, check_scenario_coverage,
                                            check_review_fixes)
from phase3_aggregation_static_check import check_aggregate_contract, check_event_and_aggregation_wiring
from phase3_ipc_static_check import check_protocol, check_wiring as check_ipc_wiring

BASELINE = "2ccbc733a1386c46917e6aee2fc7e0680e4321e9"
PROTECTED = (
    "include/runtime/process_supervisor.hpp", "src/service/process_supervisor.cpp",
    "include/runtime/device_state.hpp", "include/runtime/device_state_manager.hpp",
    "src/runtime/device_state_manager.cpp", "src/ipc/frame.hpp", "src/ipc/frame.cpp",
    "src/ipc/ipc_manager.hpp", "src/ipc/main.cpp", "src/logger/logger.cpp",
    "include/runtime/logger.hpp", "tools/fake_service/main.cpp",
    "tests/device_state_manager_tests.cpp", "tests/process_lifecycle_tests.cpp",
    "tests/phase2_static_check.py", "tests/phase3_state_static_check.py", "tests/phase3_ipc_static_check.py",
)

# Each entry points to observable assertions, not a placeholder test name.
# U/I distinction and remaining execution gaps are recorded in the review.
COVERAGE = {
    "01": [("recovery_coordination", "real_crash_candidate_success_and_reap")],
    "02": [("recovery_manager", "policy_backoff_exhaustion_and_duplicate_budget"),
           ("recovery_coordination", "real_retry_exhaustion_terminal_once")],
    "03": [("recovery_dependency", "heartbeat_deadline_and_reaping"),
           ("recovery_coordination", "real_heartbeat_cleanup_before_replacement")],
    "04": [("recovery_manager", "bound_attempt_failure_invalidates_old_execution"),
           ("recovery_coordination", "queue_late_success_duplicate_faults_and_terminal_gate")],
    "05": [("recovery_manager", "policy_matrix_and_absolute_deadline"),
           ("recovery_coordination", "real_timeout_before_due_without_launch")],
    "06": [("recovery_manager", "policy_backoff_exhaustion_and_duplicate_budget"),
           ("recovery_coordination", "queue_late_success_duplicate_faults_and_terminal_gate")],
    "07": [("recovery_manager", "deadline_result_idempotency_and_producer_time"),
           ("recovery_coordination", "queue_late_success_duplicate_faults_and_terminal_gate"),
           ("device_ipc", "real_recovery_terminal_reaches_device_subscription")],
    "08": [("recovery_dependency", "failure_propagation_and_recovery_cancellation"),
           ("recovery_coordination", "real_dependency_stop_closure_requires_explicit_start")],
    "09": [("recovery_manager", "manual_never_failure_and_shutdown_in_each_phase"),
           ("recovery_coordination", "shutdown_backoff_and_manual_stop_fifo")],
    "10": [("recovery_coordination", "real_retry_exhaustion_terminal_once"),
           ("device_ipc", "real_recovery_terminal_reaches_device_subscription")],
    "11": [("event_aggregation", "lifecycle_retry_exhaustion_reaches_device_state"),
           ("recovery_coordination", "real_optional_terminal_and_partial_resource_recovery")],
    "12": [("event_aggregation", "partial_recovery_case4"),
           ("event_aggregation", "recovery_rechecks_other_critical_failures"),
           ("recovery_coordination", "real_optional_terminal_and_partial_resource_recovery")],
    "13": [("recovery_manager", "policy_matrix_and_absolute_deadline"),
           ("recovery_dependency", "service_manager_has_no_automatic_retry_owner")],
    "14": [("recovery_manager", "policy_lifetime_success_cancel_and_manual_budget")],
    "15": [("recovery_manager", "policy_lifetime_success_cancel_and_manual_budget"),
           ("ipc_integration", "ipc_extension_boundaries")],
    "16": [("recovery_manager", "manual_supersede_coalesce_and_preparation"),
           ("recovery_dependency", "explicit_stop_and_blocked_launch")],
    "17": [("recovery_manager", "manual_never_failure_and_shutdown_in_each_phase"),
           ("recovery_coordination", "manual_launch_failure_creates_one_automatic_episode")],
    "18": [("event_aggregation", "stale_recovery_generation_rejected"),
           ("recovery_coordination", "real_crash_candidate_success_and_reap")],
    "19": [("runtime_core", "monitor_instance_reuse_and_cleanup_generation"),
           ("recovery_coordination", "real_manual_restart_and_stale_instance_facts")],
    "19b": [("recovery_dependency", "timeout_finalization_preserves_grace_pid_and_instance")],
    "20": [("recovery_manager", "policy_blocked_invariant_manual_failure_and_launch_gate")],
    "21": [("recovery_manager", "policy_matrix_and_absolute_deadline"),
           ("recovery_coordination", "signal_and_observer_failure_during_recovery_cleanup")],
    "22": [("recovery_manager", "multi_due_order_one_launch_and_offline_cancellation"),
           ("recovery_coordination", "queue_late_success_duplicate_faults_and_terminal_gate")],
    "23": [("runtime_core", "recovery_timeout_config_validation")],
    "24": [("recovery_dependency", "shutdown_deadline_uses_all_service_grace_periods"),
           ("phase2_integration", "runtime_shutdown_respects_long_grace_period")],
    "25": [("ipc_integration", "ipc_extension_boundaries")],
    "26": [("device_ipc", "running_health_requires_confirmed_heartbeats"),
           ("device_ipc", "producer_overflow_disconnect_and_resubscribe"),
           ("device_ipc", "unread_client_output_bound")],
    "27": [("recovery_dependency", "invalid_graphs_have_no_side_effects")],
    "28": [("recovery_manager", "overflow_and_registry"),
           ("recovery_manager", "deadline_result_idempotency_and_producer_time")],
    "29": [("recovery_dependency", "timeout_finalization_preserves_grace_pid_and_instance"),
           ("recovery_dependency", "timeout_after_force_does_not_restart_cleanup_grace")],
    "30": [("recovery_manager", "multi_due_order_one_launch_and_offline_cancellation"),
           ("recovery_coordination", "offline_cancels_other_task_and_leaves_no_recovering_service")],
    "31": [("recovery_manager", "policy_blocked_invariant_manual_failure_and_launch_gate")],
    "32": [("recovery_coordination", "untrusted_recovery_ingress_cannot_clear_resource")],
}


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def baseline(name):
    return subprocess.run(["git", "show", BASELINE + ":" + name], cwd=ROOT,
                          check=True, capture_output=True).stdout.decode("utf-8-sig").replace("\r\n", "\n")


def check_freeze():
    for name in PROTECTED:
        require(read(name) == baseline(name), "frozen interface/backend changed: " + name)
    pattern = r"void test_runtime_shutdown_respects_long_grace_period\(\).*?(?=\nvoid |\n\} // namespace)"
    old = re.search(pattern, baseline("tests/phase2_integration_tests.cpp"), re.S)
    new = re.search(pattern, read("tests/phase2_integration_tests.cpp"), re.S)
    require(old and new and old.group() == new.group(), "six-second/PID/reap regression was weakened")
    old_cmake, new_cmake = baseline("tests/CMakeLists.txt"), read("tests/CMakeLists.txt")
    for pattern in (r"add_executable\(\s*(\w+)\s+([^\s)]+)\s*\)",
                    r"add_test\(\s*NAME\s+(\w+)\s+COMMAND\s+(\w+)"):
        old = set(re.findall(pattern, old_cmake))
        new = set(re.findall(pattern, new_cmake))
        require(old <= new, "legacy targets/registrations were removed or rerouted")
    for name in ("recovery_manager_tests", "recovery_coordination_tests"):
        require(re.search(r"add_executable\(" + name + r"\s+" + name + r"\.cpp\)", new_cmake),
                "new recovery executable missing: " + name)
    require(not re.search(r"\b(?:WILL_FAIL|DISABLED)\b", new_cmake), "masked CTest failure")
    require("src/runtime/recovery_manager.cpp" in read("CMakeLists.txt"), "RM absent from runtime_core")
    require(not (ROOT / "include/service").exists() and not (ROOT / "include/ipc").exists(), "new header tree")


def check_ownership_and_bridge():
    rm = masked_source(read("src/runtime/recovery_manager.cpp"))
    sm = masked_source(read("src/service/service_manager.cpp"))
    runtime = masked_source(read("src/runtime/runtime_manager.cpp"))
    ipc = masked_source(read("src/ipc/ipc_manager.cpp"))
    aggregation = masked_source(read("src/runtime/service_aggregation.cpp"))
    require(not re.search(r"\b(?:restart_at|restart_delay|maximum_restarts|restart_policy)\b", sm), "legacy SM owner")
    require(not re.search(r"\b(?:service_recoveries_|service_cause_|recovery_exhausted|restart_policy)\b", runtime),
            "legacy Runtime owner")
    require(not re.search(r"\b(?:pending_restarts|cancel_restarts)\b", ipc), "legacy IPC poll")
    readers = set()
    for folder in ("src", "include"):
        for path in (ROOT / folder).rglob("*"):
            if path.suffix not in (".cpp", ".hpp"): continue
            source = masked_source(path.read_text(encoding="utf-8-sig"))
            if re.search(r"\brestart_policy\b", source): readers.add(path.relative_to(ROOT).as_posix())
    require(readers == {"include/runtime/config_manager.hpp", "src/config/config_manager.cpp",
                        "src/runtime/recovery_manager.cpp"}, "policy read/definition escaped RM/config: " + str(readers))
    require(rm.count("++slot.attempts_reserved_total") == 1 and "maximum_reservations = 5" in rm and
            "2u << slot.attempts_reserved_total" in rm, "budget/delay owner changed")
    require("++service.status.restart_count" not in sm and sm.count("processes_.start(") == 1,
            "SM added a retry producer or a second process launch primitive")
    for snippet in ("if (settled_terminal) return;", "std::stable_sort", "active.request.deadline - checkpoint",
                    "returned_at >= active.request.deadline", "now >= slot.active->request.deadline",
                    "slot.active->due.reset();", "slot.active->operation.reset();",
                    "slot.active->latest_fault_generation = reply.captured.generation",
                    "current.launched_generation == *result.execution_generation",
                    "now < active.request.deadline", "writer_executor_->releaseRecovery"):
        require(snippet in rm, "RM deadline/bridge boundary missing: " + snippet)
    require(not re.search(r"\b(?:thread|async|fork|execve|waitpid|system|reboot)\b", rm), "new recovery worker/backend")
    for snippet in ("lifecycle_work_.push_back(change)", "recovery_->classifyExit",
                    "event.instance_generation = status.launched_generation",
                    "change.cause == ServiceChangeCause::recovery_finalization",
                    "recovery_->observe(*event.recovery_result, Clock::now())",
                    "event.runtime_event->type == RuntimeEventType::resource_warning",
                    "event.device_state_event->health_target", "recovery_->cancelAll(Clock::now())"):
        require(snippet in runtime, "Runtime gate missing: " + snippet)
    callback = runtime[runtime.index("void RuntimeManager::service_state_changed"):runtime.index("namespace {", runtime.index("void RuntimeManager::service_state_changed"))]
    require(not re.search(r"(?:recovery_|services_|dispatcher_)(?:\.|->)", callback), "callback reentered owners")
    loop = runtime[runtime.index("while (running_ && !signal_received)"):]
    require(loop.index("reap_children();") < loop.index("services_.tick(Clock::now());") <
            loop.index("recovery_->tick(Clock::now());"), "writer checkpoint order changed")
    offline = runtime[runtime.index("if (device_states_.query().current == DeviceState::offline)"):runtime.index("void RuntimeManager::handle_event")]
    require("recovery_->stopAutomatic" in offline and "services_.stopService(result.service_name" in offline,
            "OFFLINE revoked RM tasks without settling cancelled SM lifecycles")
    require("std::numeric_limits<std::uint64_t>::max()" in sm and "throw std::overflow_error" in sm and
            "throw std::overflow_error" in rm, "generation can wrap")
    for snippet in ("health.candidate_generation = event.generation", "if (health.recovery_context) return false;",
                    "context->execution_generation != health.recovery_context->execution_generation",
                    "if (critical_resource) return DeviceState::error;", "resource_warnings_.erase(event.source)"):
        require(snippet in aggregation, "candidate/partial resource protection missing: " + snippet)
    require("EventType::restart_request, name" in ipc and "notify_stop(name);" in ipc, "type6 route/notification lost")


def check_matrix():
    plan_ids = set(re.findall(r"\| P4-(\d\d(?:b)?) \|", read("docs/P4/TEST_PLAN.md")))
    require(plan_ids == set(COVERAGE), "test plan IDs and evidence mapping differ")
    functions = set()
    for case, references in COVERAGE.items():
        for module, suffix in references:
            path = "tests/" + module + "_tests.cpp"
            name = "test_" + suffix
            source = masked_source(read(path))
            # Parameterized fixtures have an argument; test_body handles the
            # zero-argument tests, and this bounded matcher handles the others.
            match = re.search(r"\bvoid\s+" + name + r"\([^)]*\)\s*\{(.*?)^\}", source, re.S | re.M)
            has_assertion = match and "require(" in match.group(1)
            if match and module == "event_aggregation" and ".expect(DeviceState::" in match.group(1):
                # This fixture's state oracle is itself a require, not an empty
                # helper. Verify that oracle before crediting its callsites.
                has_assertion = bool(re.search(r"void expect\(DeviceState state\)\s*\{\s*"
                                               r"require\(states\.query\(\)\.current == state", source))
            require(has_assertion, "P4-" + case + " lacks observable assertions: " + name)
            require(name + "(" in source[source.index("int main("):], "uninvoked evidence: " + name)
            functions.add((path, name))
    report = read("docs/P4/validation_review.md")
    require(set(re.findall(r"\| P4-(\d\d(?:b)?) \|", report)) == set(COVERAGE), "review omits matrix evidence")
    return len(functions)


def main():
    try:
        files = sorted(path for folder in ("include", "src", "tools", "tests")
                       for path in (ROOT / folder).rglob("*") if path.suffix in (".cpp", ".hpp"))
        scenarios = sum(check_source(path) for path in files)
        targets, registrations = check_registration()
        check_freeze()
        check_ownership_and_bridge()
        check_explicit_target_coverage()
        check_scenario_coverage()
        check_review_fixes()
        check_aggregate_contract()
        check_event_and_aggregation_wiring()
        check_protocol()
        check_ipc_wiring()
        evidence = check_matrix()
        artifacts = files + list((ROOT / "tests").glob("*.py")) + list((ROOT / "docs/P4").glob("*.py")) + [ROOT / "docs/P4" / name for name in
            ("validation_review.md", "review_fixes.md")]
        for path in artifacts:
            source = path.read_text(encoding="utf-8-sig")
            require(source.endswith("\n") and all(line == line.rstrip() for line in source.splitlines()),
                    "whitespace/newline: " + str(path.relative_to(ROOT)))
            require(not re.search(r"^(?:<{7}|={7}|>{7})(?:\s|$)", source, re.M), "conflict: " + str(path))
            if path.suffix == ".py": ast.parse(source, filename=str(path))
        for name in ("README.md", "DESIGN_FREEZE_CANDIDATE.md", "PACKAGE_VALIDATION.md",
                     "validation_review.md", "review_fixes.md"):
            path = ROOT / "docs/P4" / name
            for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", path.read_text(encoding="utf-8-sig")):
                if "://" in target: continue
                require((path.parent / target.split("#", 1)[0]).is_file(), "broken task document link: " + target)
    except (ValueError, OSError, SyntaxError, subprocess.CalledProcessError) as error:
        print("FAIL (static): " + str(error), file=sys.stderr)
        return 1
    print(f"PASS (static): {len(files)} C++ files, {scenarios} main-wired tests, {targets} targets/{registrations} CTest entries")
    print(f"PASS (static): {len(PROTECTED)} frozen files, original six-second/PID/reap regression and old registrations")
    print("PASS (static): single policy owner, captured bridge, cancellation/timeout/ingress and IPC contracts")
    print(f"PASS (static): P4-01..32 plus P4-19b map to {evidence} assertion-bearing test functions; 441 target triples retained")
    print("LIMIT: source-only evidence; no C++ compilation, runtime tests, concurrency or ARM64 verification")
    return 0


if __name__ == "__main__":
    sys.exit(main())
