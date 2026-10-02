"""Thread 1 source-only validation; standard library, no CMake/compiler.

Checks the declared transition table against the documentation and traverses
that extracted table. It does not execute or type-check the C++ implementation.
Run from any directory: python tests/phase3_state_static_check.py
"""
from pathlib import Path
import re
import sys

from phase2_static_check import check_registration, check_source, masked_source, require


ROOT = Path(__file__).resolve().parent.parent
ARTIFACTS = (
    "include/runtime/device_state.hpp",
    "include/runtime/device_state_manager.hpp",
    "src/runtime/device_state_manager.cpp",
    "tests/device_state_manager_tests.cpp",
    "tests/phase3_state_static_check.py",
    "docs/P3/device_state_manager_implementation.md",
    "include/runtime/event.hpp",
    "include/runtime/runtime_manager.hpp",
    "src/runtime/runtime_manager.cpp",
    "tests/runtime_core_tests.cpp",
    "CMakeLists.txt",
    "tests/CMakeLists.txt",
)
EVENT_NAMES = {
    "runtime initialized": "runtime_initialized",
    "required services ready": "required_services_ready",
    "warning event": "warning",
    "critical failure": "critical_failure",
    "issue recovered": "issue_recovered",
    "failure escalates": "failure_escalated",
    "start recovery": "recovery_started",
    "success": "recovery_succeeded",
    "failed": "recovery_failed",
}
STATES = {"booting", "ready", "running", "warning", "error", "recovering", "offline"}


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def enum_members(source, name):
    match = re.search(r"enum\s+class\s+" + name + r"\s*\{([^}]+)\}", source)
    require(match is not None, "missing enum: " + name)
    return {member.strip() for member in match.group(1).split(",")}


def extract_table(source, name):
    table = re.search(r"\b" + name + r"\s*\{\{(.*?)\}\};", masked_source(source), re.S)
    require(table is not None, "missing transition table: " + name)
    edges = re.findall(r"\{\s*DeviceState::(\w+)\s*,\s*DeviceStateEventType::(\w+)\s*,"
                       r"\s*DeviceState::(\w+)\s*\}", table.group(1))
    require(len(edges) == 9, "transition table must contain nine edges: " + name)
    require(len({(state, event) for state, event, _ in edges}) == len(edges), "ambiguous table: " + name)
    return {(state, event): target for state, event, target in edges}


def check_contract():
    model = read("include/runtime/device_state.hpp")
    require(enum_members(model, "DeviceState") == STATES, "device states differ from contract")
    events = enum_members(model, "DeviceStateEventType")
    require(events == set(EVENT_NAMES.values()), "device event triggers differ from contract")
    rows = re.findall(r"^\|([A-Z]+)\|([^|]+)\|([A-Z]+)\|\s*$",
                      read("docs/P3/STATE_TRANSITION.md"), re.M)
    require(len(rows) == 9, "expected nine documented transitions")
    documented = {}
    for source, event, target in rows:
        require(event in EVENT_NAMES, "unknown documented trigger: " + event)
        documented[(source.lower(), EVENT_NAMES[event])] = target.lower()
    require(len(documented) == 9, "duplicate documented transition")
    implementation = read("src/runtime/device_state_manager.cpp")
    actual = extract_table(implementation, "transitions")
    require(actual == documented, "implementation transition table differs from STATE_TRANSITION.md")
    require(extract_table(read("tests/device_state_manager_tests.cpp"), "allowed") == documented,
            "C++ matrix test expectations differ from STATE_TRANSITION.md")
    names = dict(re.findall(r'case DeviceState::(\w+): return "([A-Z]+)";', implementation))
    require(names == {state: state.upper() for state in STATES}, "state labels differ from contract")
    allowed = sum((state, event) in actual for state in STATES for event in events)
    require(allowed == 9 and len(STATES) * len(events) - allowed == 54, "unexpected matrix dimensions")
    paths = (
        ("boot", "booting", ("runtime_initialized", "required_services_ready"), "running"),
        ("critical failure", "running", ("critical_failure",), "error"),
        ("recovery entry", "error", ("recovery_started",), "recovering"),
        ("warning cleared", "running", ("warning", "issue_recovered"), "running"),
        ("warning escalated", "running", ("warning", "failure_escalated"), "error"),
        ("recovery success", "error", ("recovery_started", "recovery_succeeded"), "running"),
        ("recovery failure", "error", ("recovery_started", "recovery_failed"), "offline"),
    )
    for label, state, triggers, expected in paths:
        for event in triggers:
            require((state, event) in actual, "missing path edge: " + label)
            state = actual[(state, event)]
        require(state == expected, "unexpected path destination: " + label)
    return len(paths)


def check_wiring():
    cmake = read("CMakeLists.txt")
    require("src/runtime/device_state_manager.cpp" in cmake, "module missing from runtime_core")
    tests = read("tests/CMakeLists.txt")
    require(re.search(r"add_test\(NAME\s+phase3_device_state_unit\s+COMMAND\s+device_state_manager_tests\)", tests),
            "missing device state CTest registration")
    require(re.search(r"set_tests_properties\(phase3_device_state_unit\s+PROPERTIES\s+TIMEOUT\s+10\)", tests),
            "missing device test timeout")
    header = read("include/runtime/runtime_manager.hpp")
    implementation = masked_source(read("src/runtime/runtime_manager.cpp"))
    require("DeviceStateManager device_states_;" in header, "RuntimeManager does not own device manager")
    require("DeviceStateSnapshot queryDeviceState() const;" in header, "missing internal query declaration")
    require(re.search(r"RuntimeManager::queryDeviceState\(\) const\s*\{\s*return device_states_\.query\(\);", implementation),
            "missing internal query definition")
    require("void RuntimeManager::post(DeviceStateEvent event)" in implementation and
            "envelope.device_state_event = std::move(event);" in implementation and
            "post(std::move(envelope));" in implementation, "device event not queued")
    require("event.type == EventType::device_state" in implementation and
            "device_states_.handle(*event.device_state_event)" in implementation,
            "device event not routed to its owner")
    require("device_states_.handle(DeviceStateEvent{DeviceStateEventType::runtime_initialized" in implementation,
            "missing explicit runtime initialization trigger")
    require("std::optional<DeviceStateEvent> device_state_event;" in read("include/runtime/event.hpp"),
            "missing internal event envelope")


def main():
    try:
        files = sorted(path for directory in ("include", "src", "tools", "tests")
                       for path in (ROOT / directory).rglob("*") if path.suffix in (".hpp", ".cpp"))
        scenarios = sum(check_source(path) for path in files)
        targets, registrations = check_registration()
        paths = check_contract()
        check_wiring()
        for name in ARTIFACTS:
            source = read(name)
            require(source.endswith("\n"), "missing final newline: " + name)
            require(all(line == line.rstrip() for line in source.splitlines()), "trailing whitespace: " + name)
            require(not re.search(r"^(?:<{7}|={7}|>{7})(?:\s|$)", source, re.M), "conflict marker: " + name)
    except (ValueError, OSError) as error:
        print("FAIL: " + str(error), file=sys.stderr)
        return 1
    print(f"PASS: {len(files)} C++ files; local includes, delimiters, comments/literals, conflict markers")
    print(f"PASS: {scenarios} test functions invoked by main; {targets} targets, {registrations} CTest entries")
    print("PASS: seven states, nine documented table edges; matrix has 9 allowed / 54 undefined pairs")
    print(f"PASS: {paths} paths through the extracted table, including boot, error and recovery entry")
    print("PASS: internal ownership/query/event wiring; task artifacts whitespace and final newlines")
    print("LIMIT: source-only validation; C++ compilation, runtime behavior and concurrency are unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
