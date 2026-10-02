"""T3 source/contract audit only; no compiler, CMake, CTest or C++ execution.

Preserves the historical P3 checks. Their old SM policy/adapter expectations
are reported separately; this audit verifies the replacement ownership wiring.
"""
from pathlib import Path
import ast
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from phase2_static_check import check_source, check_registration, masked_source, require
from phase3_state_static_check import check_contract, check_wiring, enum_members
from phase3_aggregation_static_check import check_aggregate_contract, EVENTS
from phase3_validation_static_check import check_explicit_target_coverage, check_scenario_coverage
from phase3_ipc_static_check import check_protocol, check_wiring as check_ipc_wiring


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def main():
    files = sorted(p for folder in ("include", "src", "tools", "tests")
                   for p in (ROOT / folder).rglob("*") if p.suffix in (".cpp", ".hpp"))
    scenarios = sum(check_source(p) for p in files)
    targets, registrations = check_registration()
    check_contract()
    check_wiring()
    check_aggregate_contract()
    check_explicit_target_coverage()
    check_scenario_coverage()
    check_protocol()
    check_ipc_wiring()

    runtime = masked_source(read("src/runtime/runtime_manager.cpp"))
    header = masked_source(read("include/runtime/runtime_manager.hpp"))
    rm = masked_source(read("src/runtime/recovery_manager.cpp"))
    sm = masked_source(read("src/service/service_manager.cpp"))
    aggregation = masked_source(read("src/runtime/service_aggregation.cpp"))
    ipc = masked_source(read("src/ipc/ipc_manager.cpp"))
    monitor = masked_source(read("src/monitor/monitor.cpp"))
    require(enum_members(read("include/runtime/event.hpp"), "RuntimeEventType") == EVENTS,
            "internal RuntimeEventType contract changed")
    require("std::thread" not in rm and "std::thread" not in read("include/runtime/recovery_manager.hpp"),
            "RM introduced a recovery thread")

    require(not re.search(r"\b(?:service_recoveries_|service_cause_|recovery_exhausted|restart_policy)\b",
                          runtime + header), "Runtime retained a policy/terminal inference owner")
    require(not re.search(r"\b(?:restart_at|restart_delay|maximum_restarts|restart_policy)\b", sm),
            "SM retained an automatic policy owner")
    require(not re.search(r"\b(?:pending_restarts|cancel_restarts)\b", ipc), "IPC retained restart polling")
    require(rm.count("++slot.attempts_reserved_total") == 1, "reservation has multiple producers")
    require("slot.active->due.reset();" in rm[rm.index("void RecoveryManager::reserveLocked"):],
            "exhaustion retained launch permission")
    require("RecoveryManager::takeStarts" in rm and "starts_.push_back" in rm and
            "RecoveryManager::stopAutomatic" in rm and "automatic_stopped_" in rm,
            "canonical admission/OFFLINE gate missing")
    require("if (settled_terminal) return;" in rm and
            rm.index("if (settled_terminal) return;") < rm.index("std::vector<std::string> candidates;"),
            "a pending terminal can be overtaken by another due launch")
    require("std::make_unique<RecoveryManager>(std::move(ordered), services_" in runtime,
            "production RM not built from topology-ordered SM definitions")
    callback = runtime[runtime.index("void RuntimeManager::service_state_changed"):
                       runtime.index("namespace {", runtime.index("void RuntimeManager::service_state_changed"))]
    require("lifecycle_work_.push_back(change)" in callback and
            not re.search(r"(?:recovery_|services_|dispatcher_)(?:\.|->)", callback), "callback reenters lifecycle")
    loop = runtime[runtime.index("while (running_ && !signal_received)"):]
    require(loop.index("reap_children();") < loop.index("services_.tick(Clock::now());") <
            loop.index("recovery_->tick(Clock::now());"), "reap/facts/SM/RM checkpoint order changed")
    require("services_.handleProcessExit(event, recovery_->classifyExit" in runtime and
            "event.instance_generation = status.launched_generation" in runtime and
            "if (!event.instance_generation || *event.instance_generation == 0) return;" in runtime,
            "reap instance/disposition gate missing")
    require("events.back().instance_generation = watch.instance_generation" in monitor and
            "service.status.launched_generation);" in sm, "Monitor lost the captured child identity")
    require("EventType::restart_request, name" in ipc and
            "[this](const std::string& name) { post(Event{EventType::restart_request" in runtime,
            "manual IPC/facade routes differ")
    require("event.runtime_event->type == RuntimeEventType::resource_warning" in runtime and
            "recovery_->observe(*event.recovery_result, Clock::now())" in runtime and
            "event.device_state_event->health_target" in runtime,
            "public ingress can bypass the result owner")
    require("change.cause == ServiceChangeCause::recovery_finalization" in runtime and
            "recovery_->observeFailure(change.service_name, change.operation->context" in runtime,
            "captured failure/finalization routing missing")
    require("recovery_->takeStarts()" in runtime and "recovery_->takeResults()" in runtime and
            "aggregation_.complete_manual(result)" in runtime and "recovery_->stopAutomatic" in runtime,
            "sealed output/manual/OFFLINE routing missing")
    cleanup = runtime[runtime.index("    shutting_down_ = true;", runtime.index("void RuntimeManager::run")):]
    require(cleanup.index("recovery_->cancelAll") < cleanup.index("services_.stop_all") and
            "services_.shutdown_deadline().value_or(Clock::now())" in cleanup and
            "if (!active) break;" in cleanup and "std::rethrow_exception(failure)" in cleanup,
            "shutdown cancellation/grace/reap/error preservation changed")
    require("health.candidate_generation = event.generation" in aggregation and
            "health.candidate_generation != event.generation" in aggregation and
            "context->execution_generation != health.recovery_context->execution_generation" in aggregation and
            "if (health.recovery_context) return false;" in aggregation,
            "candidate/episode/execution/ordinary START aggregation gate missing")
    require(aggregation.index("if (critical_resource)") < aggregation.index("if (critical_failure)"),
            "service recovery can clear critical resources")
    for snippet in ("resource_warnings_[event.source] = event.severity", "resource_warnings_.erase(event.source)",
                    "health.heartbeat_lost || health.policy.criticality == ServiceCriticality::high"):
        require(snippet in aggregation, "health severity/source lock missing: " + snippet)

    protected = (
        "include/runtime/process_supervisor.hpp", "src/service/process_supervisor.cpp",
        "include/runtime/device_state.hpp", "include/runtime/device_state_manager.hpp",
        "src/runtime/device_state_manager.cpp", "src/ipc/frame.hpp", "src/ipc/frame.cpp",
        "src/ipc/ipc_manager.hpp", "src/ipc/main.cpp", "tools/fake_service/main.cpp",
        "tests/phase2_static_check.py", "tests/phase3_state_static_check.py",
        "tests/phase3_aggregation_static_check.py", "tests/phase3_ipc_static_check.py",
        "tests/phase3_validation_static_check.py", "tests/device_state_manager_tests.cpp",
    )
    for name in protected:
        baseline = subprocess.run(["git", "show", "HEAD:" + name], cwd=ROOT,
                                  check=True, capture_output=True).stdout.decode("utf-8-sig")
        require(read(name) == baseline.replace("\r\n", "\n"), "protected path changed: " + name)
    shutdown = read("tests/phase2_integration_tests.cpp")
    baseline = subprocess.run(["git", "show", "HEAD:tests/phase2_integration_tests.cpp"], cwd=ROOT,
                              check=True, capture_output=True).stdout.decode("utf-8-sig").replace("\r\n", "\n")
    pattern = r"void test_runtime_shutdown_respects_long_grace_period\(\).*?(?=\nvoid |\n\} // namespace)"
    current_regression = re.search(pattern, shutdown, re.S)
    original_regression = re.search(pattern, baseline, re.S)
    require(current_regression and original_regression and
            current_regression.group() == original_regression.group(), "six-second/PID regression changed")
    for folder in ("include/runtime", "src/runtime", "src/monitor", "tests"):
        for path in (ROOT / folder).glob("*"):
            if path.suffix not in (".hpp", ".cpp", ".py"): continue
            source = path.read_text(encoding="utf-8-sig")
            require(source.endswith("\n") and all(line == line.rstrip() for line in source.splitlines()),
                    "whitespace/newline: " + str(path.relative_to(ROOT)))
            if path.suffix == ".py": ast.parse(source, filename=str(path))
    ast.parse(Path(__file__).read_text(encoding="utf-8-sig"))
    for name in ("docs/P4/thread3_static_check.py", "docs/P4/implementation_thread3.md"):
        source = read(name)
        require(source.endswith("\n") and all(line == line.rstrip() for line in source.splitlines()),
                "task artifact whitespace/newline: " + name)
    print(f"PASS: {len(files)} C++ files, {scenarios} main-wired test functions, {targets} targets/{registrations} CTest entries")
    print("PASS: RM owner, callback queue, causal checkpoint order, generation bridge, sealed output and ingress gates")
    print("PASS: unchanged device/IPC contracts, 441-entry matrix, source severity and six-second shutdown regression")
    print(f"PASS: {len(protected)} protected files unchanged; source whitespace and Python syntax")
    print("LIMIT: static source/contract checks only; C++ types/linking and runtime behavior remain unverified")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, SyntaxError) as error:
        print("FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
