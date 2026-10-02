"""Thread 2 source/contract checks; no compiler, CMake or C++ execution.

Scenario checks traverse extracted C++ transition declarations. They do not
execute ServiceAggregation::evaluate/handle or establish concurrency safety.
"""
from pathlib import Path
import re
import sys

from phase2_static_check import check_registration, check_source, masked_source, require
from phase3_state_static_check import check_contract, check_wiring, enum_members


ROOT = Path(__file__).resolve().parent.parent
ARTIFACTS = (
    "include/runtime/event.hpp", "src/runtime/event.cpp",
    "include/runtime/service_aggregation.hpp", "src/runtime/service_aggregation.cpp",
    "include/runtime/runtime_manager.hpp", "src/runtime/runtime_manager.cpp",
    "include/runtime/device_state.hpp", "include/runtime/device_state_manager.hpp",
    "src/runtime/device_state_manager.cpp", "tests/event_aggregation_tests.cpp",
    "tests/phase3_aggregation_static_check.py", "CMakeLists.txt", "tests/CMakeLists.txt",
    "docs/P3/EVENT_MODEL.md", "docs/P3/AGGREGATION_RULE.md",
    "docs/P3/event_aggregation_implementation.md",
)
EVENTS = {"service_started", "service_failed", "service_stopped", "heartbeat_timeout",
          "resource_warning", "recovery_start", "recovery_success", "recovery_failed"}


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def edges(name):
    source = masked_source(read("src/runtime/device_state_manager.cpp"))
    table = re.search(r"\b" + name + r"\s*\{\{(.*?)\}\};", source, re.S)
    require(table is not None, "missing explicit transition table: " + name)
    triples = re.findall(r"\{\s*DeviceState::(\w+)\s*,\s*DeviceStateEventType::(\w+)\s*,"
                         r"\s*DeviceState::(\w+)\s*\}", table.group(1))
    require(len(triples) == len(set(triples)), "duplicate transition triple")
    size = re.search(r"std::array<Transition,\s*(\d+)>\s+" + name, source)
    require(size and int(size.group(1)) == len(triples), "wrong explicit table size")
    return set(triples)


def check_aggregate_contract():
    original = edges("transitions")
    extra = edges("aggregation_transitions")
    rows = re.findall(r"^\|\s*(READY|RUNNING|WARNING|ERROR|RECOVERING)\s*\|\s*(\w+)\s*\|"
                      r"\s*(READY|RUNNING|WARNING|ERROR|OFFLINE)\s*\|\s*$",
                      read("docs/P3/AGGREGATION_RULE.md"), re.M)
    documented = {(source.lower(), event, target.lower()) for source, event, target in rows}
    require(len(rows) == len(extra) == 15 and extra == documented, "aggregate table differs from documentation")
    allowed = original | extra
    require(not any(source == "offline" for source, _, _ in allowed), "OFFLINE is not terminal")
    # Expected device paths from the task, checked against extracted declarations.
    paths = {
        "case1 vision": ("running", [("warning", "warning")]),
        "case2 control": ("running", [("critical_failure", "error")]),
        "case3 priority": ("running", [("warning", "warning"), ("failure_escalated", "error")]),
        "case4 partial recovery": ("running", [("warning", "warning"), ("failure_escalated", "error"),
                                                ("recovery_started", "recovering"),
                                                ("recovery_succeeded", "warning"), ("issue_recovered", "running")]),
        "startup failure": ("ready", [("critical_failure", "error")]),
        "new recovery failure": ("recovering", [("critical_failure", "error")]),
        "exhausted recovery": ("recovering", [("recovery_failed", "offline")]),
        "remaining critical failure": ("recovering", [("recovery_succeeded", "error")]),
        "uncoordinated partial recovery": ("error", [("issue_recovered", "warning")]),
    }
    for label, (state, triggers) in paths.items():
        for trigger, target in triggers:
            require((state, trigger, target) in allowed, "missing scenario edge: " + label)
            state = target
    implementation = masked_source(read("src/runtime/device_state_manager.cpp"))
    require("event.health_target ? aggregate_next_state(state_.current, event)" in implementation,
            "aggregate target not checked by state owner")
    require("edge.to == *event.health_target" in implementation, "state owner does not validate target")
    return len(paths), len(allowed)


def check_event_and_aggregation_wiring():
    require(enum_members(read("include/runtime/event.hpp"), "RuntimeEventType") == EVENTS,
            "required internal event kinds differ")
    dispatcher = read("src/runtime/event.cpp")
    labels = dict(re.findall(r'case RuntimeEventType::(\w+): return "([A-Z_]+)";', dispatcher))
    require(labels == {event: event.upper() for event in EVENTS}, "internal event labels differ")
    dispatcher = masked_source(dispatcher)
    require("pending_.push(std::move(event));" in dispatcher and "pending_.front()" in dispatcher and
            "pending_.pop();" in dispatcher and "subscriber(event);" in dispatcher and
            "if (dispatching_) return;" in dispatcher, "missing FIFO/nested dispatch mechanism")
    source = read("src/runtime/service_aggregation.cpp")
    clean = masked_source(source)
    require(EVENTS <= set(re.findall(r"(?:case|==)\s+RuntimeEventType::(\w+)", clean)),
            "aggregation does not handle every internal event")
    for name, criticality in (("control_service", "high"), ("vision_service", "medium"), ("ota_service", "low")):
        require(re.search(r'name == "' + name + r'"\) policy = \{ServiceCriticality::' + criticality, source),
                "wrong default criticality: " + name)
    require("options_.vision_required" in clean and "options_.policies.find(name)" in clean,
            "missing static configuration/vision override")
    require(clean.index("if (recovery_failed_)") < clean.index("if (critical_failure)") < clean.index("if (warning)"),
            "aggregate priority order changed")
    for snippet in ("resource_warnings_[event.source] = event.severity", "resource_warnings_.erase(event.source)",
                    "health.heartbeat_lost || health.policy.criticality == ServiceCriticality::high",
                    "const auto target = evaluate();", "trigger.health_target = target;", "states_.handle(trigger)",
                    "current == DeviceState::offline || current == target"):
        require(snippet in clean, "missing health aggregation mechanism: " + snippet)
    runtime = masked_source(read("src/runtime/runtime_manager.cpp"))
    for snippet in ("aggregation_.add_service(config.service_name, config.autostart)",
                    "void RuntimeManager::post(RuntimeEvent event)", "envelope.runtime_event = std::move(event)",
                    "service_state_changed(change)", "aggregation_.handle(event)",
                    "change.failure_type == FailureType::heartbeat_timeout", "RuntimeEventType::heartbeat_timeout",
                    "shutting_down_ = true;"):
        require(snippet in runtime, "missing runtime adapter: " + snippet)
    # Phase4 captures cause/context in SM callbacks instead of a Runtime-local
    # pointer. Check the replacement causal drain, retaining the fact adapter.
    require("lifecycle_work_.push_back(change)" in runtime and
            "auto change = std::move(lifecycle_work_.front())" in runtime and
            "if (!shutting_down_) apply_change(change);" in runtime,
            "captured lifecycle facts are not drained by the writer")
    require(re.search(r"services_\.tick\(Clock::now\(\)\);\s*drain_work\(\);\s*"
                      r"recovery_->tick\(Clock::now\(\)\);\s*drain_work\(\);", runtime),
            "SM/RM timer facts are not causally drained")
    require("dispatcher_.drain();" in runtime[runtime.index("void RuntimeManager::drain_work"):],
            "missing dispatcher causal drain")
    cmake = read("CMakeLists.txt")
    require("src/runtime/service_aggregation.cpp" in cmake, "aggregation missing from runtime_core")
    tests = read("tests/CMakeLists.txt")
    require("add_test(NAME phase3_event_aggregation_unit COMMAND event_aggregation_tests)" in tests,
            "aggregation tests are not registered")
    require("set_tests_properties(phase3_event_aggregation_unit PROPERTIES TIMEOUT 10)" in tests,
            "aggregation test timeout missing")
    cases = read("tests/event_aggregation_tests.cpp")
    for function in ("test_vision_failure_case1", "test_control_failure_case2",
                     "test_multiple_failure_priority_case3", "test_partial_recovery_case4"):
        require("void " + function + "()" in cases, "missing requested C++ scenario: " + function)


def main():
    try:
        files = sorted(path for directory in ("include", "src", "tools", "tests")
                       for path in (ROOT / directory).rglob("*") if path.suffix in (".hpp", ".cpp"))
        scenarios = sum(check_source(path) for path in files)
        targets, registrations = check_registration()
        check_contract()
        check_wiring()
        paths, triples = check_aggregate_contract()
        check_event_and_aggregation_wiring()
        for name in ARTIFACTS:
            source = read(name)
            require(source.endswith("\n"), "missing final newline: " + name)
            require(all(line == line.rstrip() for line in source.splitlines()), "trailing whitespace: " + name)
            require(not re.search(r"^(?:<{7}|={7}|>{7})(?:\s|$)", source, re.M), "conflict marker: " + name)
    except (ValueError, OSError) as error:
        print("FAIL: " + str(error), file=sys.stderr)
        return 1
    print(f"PASS: {len(files)} C++ files; {scenarios} test functions; {targets} targets, {registrations} CTest entries")
    print("PASS: eight internal event types/labels; static criticality, priority and runtime adapter structure")
    print(f"PASS: nine original plus 15 aggregate triples ({triples} total); {paths} extracted-table scenario paths")
    print("PASS: four requested C++ scenarios registered; dispatcher FIFO structure; artifact whitespace")
    print("LIMIT: source/contract checks only; C++ execution, types/linking and concurrency remain unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
