"""Thread 4 aggregate source/contract audit; Python standard library only.

Runs the existing static suites and checks the declared explicit-target matrix
against both state contracts. No C++ execution, compiler, CMake, or sockets.
A zero exit status covers these structural checks, not C++ runtime acceptance;
review and remediation records are in docs/P3/validation_review.md and review_fixes.md.
"""
import ast
import re
import sys

import phase2_static_check
import phase3_aggregation_static_check
import phase3_ipc_static_check
import phase3_state_static_check
from phase2_static_check import masked_source, require
from phase3_aggregation_static_check import edges
from phase3_state_static_check import ROOT, STATES, EVENT_NAMES


ARTIFACTS = (
    "tests/device_state_manager_tests.cpp",
    "tests/event_aggregation_tests.cpp",
    "tests/phase3_validation_static_check.py",
    "docs/P3/validation_review.md",
    "docs/P3/review_fixes.md",
    "include/runtime/event.hpp", "include/runtime/service_manager.hpp",
    "include/runtime/service_aggregation.hpp", "include/runtime/monitor.hpp",
    "include/runtime/runtime_manager.hpp", "src/service/service_manager.cpp",
    "src/runtime/service_aggregation.cpp", "src/monitor/monitor.cpp",
    "src/runtime/runtime_manager.cpp", "src/ipc/ipc_manager.cpp",
    "tests/runtime_core_tests.cpp", "tests/recovery_dependency_tests.cpp",
    "tests/phase2_integration_tests.cpp", "tests/device_ipc_tests.cpp",
    "docs/P3/STATE_TRANSITION.md", "docs/P3/DEVICE_STATE_MODEL.md",
    "docs/P3/EVENT_MODEL.md", "docs/P3/AGGREGATION_RULE.md",
    "docs/P3/MONITOR_INTEGRATION.md", "docs/P3/IPC_EXTENSION.md",
)


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def test_body(source, name):
    match = re.search(r"\bvoid\s+" + name + r"\(\)\s*\{(.*?)^\}", source, re.S | re.M)
    require(match is not None, "missing test definition: " + name)
    return match.group(1)


def check_explicit_target_coverage():
    source = masked_source(read("tests/device_state_manager_tests.cpp"))
    body = test_body(source, "test_explicit_target_matrix")
    table = re.search(r"std::array<Expected,\s*(\d+)>\s+target_allowed\s*\{\{(.*?)\}\};", body, re.S)
    require(table is not None, "missing independent explicit-target expectations")
    declared = re.findall(r"\{\s*DeviceState::(\w+)\s*,\s*DeviceStateEventType::(\w+)\s*,"
                          r"\s*DeviceState::(\w+)\s*\}", table.group(2))
    expected = edges("transitions") | edges("aggregation_transitions")
    require(int(table.group(1)) == len(declared) == len(set(declared)) == 24,
            "explicit-target test has duplicate or missing expectations")
    require(set(declared) == expected, "target test oracle differs from the reviewed state contracts")
    for enum, name, members in (("DeviceState", "states", STATES),
                                ("DeviceStateEventType", "events", set(EVENT_NAMES.values()))):
        array = re.search(r"std::array<" + enum + r",\s*(\d+)>\s+" + name + r"\s*\{\{(.*?)\}\};",
                          body, re.S)
        require(array is not None, "matrix dimension missing: " + name)
        values = re.findall(enum + r"::(\w+)", array.group(2))
        require(int(array.group(1)) == len(values) == len(members) and set(values) == members,
                "matrix dimension is incomplete: " + name)
    for snippet in ("for (const auto from : states)", "for (const auto type : events)",
                    "for (const auto to : states)", "event.health_target = to;",
                    "edge.from == from && edge.event == type && edge.to == to",
                    "accepted == 24 && rejected == 417", "same_snapshot(manager.query(), before)",
                    "notifications.empty()", "DeviceTransitionResult::invalid_transition"):
        require(snippet in body, "target matrix coverage/assertion missing: " + snippet)
    invalid = test_body(source, "test_invalid_explicit_target_and_metadata")
    for snippet in ("event.source.clear()", "event.reason.clear()", "static_cast<DeviceState>(999)",
                    "static_cast<DeviceStateEventType>(999)", "DeviceTransitionResult::invalid_metadata",
                    "same_snapshot(manager.query(), before)", "notifications.empty()"):
        require(snippet in invalid, "target validation case missing: " + snippet)
    # Count declaration-space coverage; this does not call the C++ handle function.
    combinations = len(STATES) ** 2 * len(EVENT_NAMES)
    require(combinations == 441 and combinations - len(expected) == 417, "wrong matrix cardinality")
    return combinations, len(expected)


def check_scenario_coverage():
    source = masked_source(read("tests/event_aggregation_tests.cpp"))
    recovery = test_body(source, "test_normal_startup_and_complete_critical_recovery")
    for snippet in ("fixture.running()", "RuntimeEventType::service_failed",
                    "RuntimeEventType::recovery_start", "RuntimeEventType::recovery_success",
                    "fixture.changes[i].previous == previous", "fixture.changes.size() == expected.size()"):
        require(snippet in recovery, "full recovery path/assertion missing: " + snippet)
    heartbeat = test_body(source, "test_heartbeat_timeout_and_successful_recovery")
    original = test_body(read("tests/event_aggregation_tests.cpp"), "test_heartbeat_timeout_and_successful_recovery")
    require('for (const auto* name : {"control_service", "vision_service", "ota_service"})' in original,
            "heartbeat recovery does not cover every default criticality")
    for snippet in ("RuntimeEventType::heartbeat_timeout", "fixture.expect(DeviceState::error)",
                    "fixture.expect(DeviceState::recovering)", "fixture.expect(DeviceState::running)",
                    "fixture.changes.size() == 3"):
        require(snippet in heartbeat, "heartbeat recovery path/assertion missing: " + snippet)


def check_review_fixes():
    event = masked_source(read("include/runtime/event.hpp"))
    require("std::optional<std::uint64_t> generation;" in event and
            "enum class ResourceSeverity { warning, critical };" in event,
            "missing resource severity or generation contract")
    aggregation = masked_source(read("src/runtime/service_aggregation.cpp"))
    for snippet in ("*event.generation < health.generation", "*event.generation == health.generation",
                    "if (!same_generation || !unavailable) return false;", "if (!same_generation) return false;",
                    "health.generation = *event.generation", "if (critical_resource) return DeviceState::error;",
                    "resource_warnings_[event.source] = event.severity", "resource_warnings_.erase(event.source)"):
        require(snippet in aggregation, "review remediation missing: " + snippet)
    require(aggregation.index("if (critical_resource)") < aggregation.index("if (critical_failure)"),
            "service recovery can bypass critical resource pressure")
    thresholds = read("include/runtime/monitor.hpp")
    defaults = dict(re.findall(r"double\s+(cpu_warning|memory_warning|memory_critical)\s*=\s*([\d.]+)", thresholds))
    require({key: float(value) for key, value in defaults.items()} ==
            dict(cpu_warning=80.0, memory_warning=80.0, memory_critical=95.0), "resource defaults differ from contract")
    monitor = masked_source(read("src/monitor/monitor.cpp"))
    # P5 CHANGE_LIST: typed, change-only hysteresis replaces per-sample facts.
    # Keep severity/defaults/adapter tests, and require the shared policy path.
    for snippet in ("std::isfinite(value)", "thresholds.memory_warning >= thresholds.memory_critical",
                    "observeResources(snapshot);", "snapshot.cpu.quality == MetricQuality::valid",
                    "snapshot.memory.quality == MetricQuality::valid", "std::lock_guard<std::mutex> lock(resource_mutex_)",
                    "if (previous && *previous == next) return;", "cpu_pressure_ == Pressure::warning",
                    "value <= *thresholds_.cpu_clear", "value <= *thresholds_.memory_clear",
                    "memory_pressure_ == Pressure::critical", "value <= *thresholds_.memory_critical_clear",
                    "value >= thresholds_.memory_critical", "fact.active = next != Pressure::normal",
                    "ResourceSeverity::critical : ResourceSeverity::warning", "resources_(std::move(fact))"):
        require(snippet in monitor, "resource producer wiring missing: " + snippet)
    resource_test = test_body(read("tests/runtime_core_tests.cpp"), "test_resource_monitor_thresholds")
    for snippet in ("94.9", "report_resources(0, 95", "report_resources(80, 80", "report_resources(0, 75",
                    "report_resources(0, 90", "facts.size()", "ResourceSeverity::critical",
                    "quiet_NaN", "ResourceThresholds{60, 70, 90}"):
        require(snippet in resource_test, "P5 migrated threshold oracle missing: " + snippet)
    service = masked_source(read("src/service/service_manager.cpp"))
    for snippet in ("++service.status.generation", "service.status.generation, recovery_exhausted",
                    "ServiceManager::finishRecoveryFailure", "ServiceChangeCause::recovery_finalization",
                    "ServiceManager::shutdown_deadline() const",
                    "*service.termination_deadline > *latest"):
        require(snippet in service, "lifecycle remediation missing: " + snippet)
    # P3 exhaustion migrated to RM; do not remove the terminal-chain audit.
    recovery = masked_source(read("src/runtime/recovery_manager.cpp"))
    for snippet in ("definition.restart_policy == RestartPolicy::never",
                    "slot.attempts_reserved_total >= maximum_reservations",
                    "RecoveryTerminalReason::retry_exhausted", "writer_executor_->finishRecoveryFailure",
                    "slot.active->latest_fault_generation = reply.captured.generation", "finishLocked(slot, resultFor"):
        require(snippet in recovery, "RM terminal remediation missing: " + snippet)
    runtime = masked_source(read("src/runtime/runtime_manager.cpp"))
    for snippet in ("event.generation = change.generation", "change.cause == ServiceChangeCause::recovery_finalization",
                    "for (const auto& result : recovery_->takeResults())",
                    "RuntimeEventType::recovery_failed", "services_.shutdown_deadline().value_or(Clock::now())",
                    "if (!active) break;", "if (Clock::now() >= deadline)", "std::rethrow_exception(failure)",
                    "monitor_.report_resources(cpu_percent, memory_percent, at)"):
        require(snippet in runtime, "runtime remediation wiring missing: " + snippet)
    require("Clock::now() + std::chrono::seconds(4)" not in runtime, "fixed four-second shutdown budget returned")
    ipc = read("src/ipc/ipc_manager.cpp")
    require('if (unhealthy != 0) overall_health = "UNHEALTHY"' in ipc and
            'else if (unknown != 0 || definitions_.empty()) overall_health = "UNKNOWN"' in ipc,
            "RUNNING health ignores unconfirmed/expired service heartbeats")
    required_tests = {
        "tests/event_aggregation_tests.cpp": ("test_stale_recovery_generation_rejected",
            "test_resource_critical_downgrade_and_clear", "test_lifecycle_retry_exhaustion_reaches_device_state"),
        "tests/runtime_core_tests.cpp": ("test_resource_monitor_thresholds", "test_runtime_resource_fact_adapter",
                                         "test_runtime_observer_exception_cleans_up"),
        "tests/recovery_dependency_tests.cpp": ("test_terminal_failure_metadata_and_generations",
                                               "test_shutdown_deadline_uses_all_service_grace_periods"),
        "tests/device_ipc_tests.cpp": ("test_running_health_requires_confirmed_heartbeats",
            "test_producer_overflow_disconnect_and_resubscribe", "test_unread_client_output_bound"),
    }
    for path, tests in required_tests.items():
        clean = masked_source(read(path))
        main = clean[clean.index("int main("):]
        for test in tests:
            require(re.search(r"void\s+" + test + r"\s*\(", clean) and test + "(" in main,
                    "remediation test missing/uninvoked: " + test)
    return sum(len(tests) for tests in required_tests.values())


def main():
    suites = (("Phase2", phase2_static_check), ("Phase3 state", phase3_state_static_check),
              ("Phase3 aggregation", phase3_aggregation_static_check), ("Phase3 IPC", phase3_ipc_static_check))
    failures = []
    for label, suite in suites:
        print("CHECK: " + label)
        try:
            if suite.main() != 0:
                failures.append(label)
        except (ValueError, OSError, SyntaxError) as error:
            failures.append(label + ": " + str(error))
    try:
        combinations, allowed = check_explicit_target_coverage()
        check_scenario_coverage()
        remediation_tests = check_review_fixes()
        for name in ARTIFACTS:
            source = read(name)
            require(source.endswith("\n"), "missing final newline: " + name)
            require(all(line == line.rstrip() for line in source.splitlines()), "trailing whitespace: " + name)
            require(not re.search(r"^(?:<{7}|={7}|>{7})(?:\s|$)", source, re.M), "conflict marker: " + name)
        for path in (ROOT / "tests").glob("*.py"):
            ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
    except (ValueError, OSError, SyntaxError) as error:
        failures.append("Thread4: " + str(error))
    if failures:
        print("FAIL: " + "; ".join(failures), file=sys.stderr)
        return 1
    print(f"PASS: explicit-target test declares {combinations} triples: {allowed} allowed / {combinations - allowed} rejected")
    print("PASS: full boot/critical recovery and all three service heartbeat recovery scenarios declared")
    print("PASS: Thread4 artifacts (including untracked files), Python syntax and existing test registration")
    print(f"PASS: four review fixes and health-query wiring; {remediation_tests} remediation tests declared and invoked")
    print("LIMIT: source/contract checks only; C++ behavior/concurrency unverified; remediation details in review_fixes.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
