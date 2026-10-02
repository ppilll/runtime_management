"""Thread3 source-only checks; Python standard library, no compiler/CMake.

Checks protocol consistency, source structure, callback/queue wiring and test
registration. Does not execute C++, sockets, concurrency or state transitions.
"""
from pathlib import Path
import ast
import json
import re
import sys

from phase2_static_check import check_registration, check_source, masked_source, require


ROOT = Path(__file__).resolve().parent.parent
ARTIFACTS = (
    "src/ipc/frame.hpp", "src/ipc/ipc_manager.hpp", "src/ipc/ipc_manager.cpp",
    "src/ipc/main.cpp", "include/runtime/runtime_manager.hpp",
    "src/runtime/runtime_manager.cpp", "tests/CMakeLists.txt",
    "tests/device_ipc_tests.cpp", "tests/phase3_ipc_static_check.py",
    "docs/P3/IPC_EXTENSION.md", "docs/P3/ipc_extension_implementation.md",
)


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def check_protocol():
    header = read("src/ipc/frame.hpp")
    enum = re.search(r"enum class Type\s*:\s*std::uint16_t\s*\{([^}]+)\}", header)
    require(enum is not None, "missing message type enum")
    members = dict((name, int(value)) for name, value in
                   re.findall(r"(\w+)\s*=\s*(\d+)", enum.group(1)))
    expected = dict(start=1, stop=2, query_status=3, heartbeat=4, event=5,
                    restart_service=6, get_service_list=7, get_device_state=8,
                    get_health=9, subscribe_event=10, error=255)
    require(members == expected, "wire types differ from Phase1/2/3 contract")
    require("header_size = 10" in header and "max_payload = 64 * 1024" in header,
            "existing frame sizes changed")
    documentation = read("docs/P3/IPC_EXTENSION.md")
    rows = re.findall(r"^\| (GET_DEVICE_STATE|GET_HEALTH|SUBSCRIBE_EVENT) \| (\d+) \| `([^`]+)` \|$",
                      documentation, re.M)
    require(len(rows) == 3, "missing documented wire requests")
    for name, number, payload in rows:
        require(members[name.lower()] == int(number), "documented message number mismatch")
        require(json.loads(payload) == (dict(event="DEVICE_STATE_CHANGED") if name == "SUBSCRIBE_EVENT" else {}),
                "invalid documented request schema")
    examples = [json.loads(text) for text in re.findall(r"```json\s*\n(.*?)\n```", documentation, re.S)]
    require(len(examples) == 4, "expected four response/notification examples")
    state, health, ack, event = examples
    require(set(state) == {"state", "timestamp", "reason"} and isinstance(state["timestamp"], int),
            "device response example violates contract")
    require(health["device_state"] == state and health["health"]["reason"] == state["reason"],
            "health reason/snapshot example inconsistency")
    summary = health["service_summary"]
    require(summary["total"] == len(summary["services"]), "health example row count mismatch")
    for label in ("healthy", "unhealthy", "unknown"):
        require(summary[label] == sum(row["health_status"] == label.upper() for row in summary["services"]),
                "health example counts mismatch")
    require(ack == dict(result="OK", event="DEVICE_STATE_CHANGED") and
            event["event"] == "DEVICE_STATE_CHANGED" and event["timestamp"] == state["timestamp"],
            "notification/ACK example mismatch")
    return len(examples)


def check_wiring():
    header = read("src/ipc/ipc_manager.hpp")
    ipc = masked_source(read("src/ipc/ipc_manager.cpp"))
    require("QueryDevice query_device = {}" in header, "legacy constructor defaults lost")
    require("std::weak_ptr<DeviceEvents>(device_events_)" in ipc and "weak.lock()" in ipc,
            "sink has no lifetime-safe weak queue reference")
    require("std::lock_guard<std::mutex> lock(events->mutex)" in ipc and
            "changes.swap(device_events_->pending)" in ipc, "producer/consumer queue wiring missing")
    require("client.subscribed_after = device_events_->sequence" in ipc and
            "change.sequence <= client.subscribed_after" in ipc, "subscription boundary missing")
    require("events->pending.size() > 1024" in ipc and "events->bytes > max_queued_output" in ipc and
            "client.output.size() + encoded.size() > max_queued_output" in ipc,
            "queue/output bounds missing")
    require("client.subscribed_after < lost_through" in ipc and
            "if (client.output_overflow) { remove_client(fd); continue; }" in ipc,
            "dropped events are not surfaced by disconnect")
    require("static_cast<std::uint16_t>(ipc::Type::event), 0, change.payload" in ipc,
            "notification type/request ID mismatch")
    main = masked_source(read("src/ipc/main.cpp"))
    require("device_changes = ipc.device_state_sink()" in main and "core.queryDeviceState()" in main and
            "if (device_changes) device_changes(state)" in main, "runtime IPC binding missing")
    runtime = masked_source(read("src/runtime/runtime_manager.cpp"))
    require("device_changes = std::move(device_changes)" in runtime and
            "if (device_changes) device_changes(state)" in runtime, "committed callback is not forwarded")
    tests = read("tests/CMakeLists.txt")
    require("add_test(NAME phase3_device_ipc COMMAND device_ipc_tests)" in tests and
            "target_link_libraries(device_ipc_tests PRIVATE phase1_ipc)" in tests and
            "set_tests_properties(phase3_device_ipc PROPERTIES TIMEOUT 30)" in tests,
            "IPC test registration/linkage/timeout missing")
    scenarios = read("tests/device_ipc_tests.cpp")
    for name in ("test_device_queries_and_invalid_requests", "test_subscriptions_and_burst_order",
                 "test_optional_device_callback_and_sink_lifetime"):
        require(name in scenarios, "missing IPC test scenario: " + name)


def main():
    try:
        files = sorted(path for directory in ("include", "src", "tools", "tests")
                       for path in (ROOT / directory).rglob("*") if path.suffix in (".hpp", ".cpp"))
        scenarios = sum(check_source(path) for path in files)
        targets, registrations = check_registration()
        examples = check_protocol()
        check_wiring()
        for name in ARTIFACTS:
            source = read(name)
            require(source.endswith("\n"), "missing final newline: " + name)
            require(all(line == line.rstrip() for line in source.splitlines()), "trailing whitespace: " + name)
            require(not re.search(r"^(?:<{7}|={7}|>{7})(?:\s|$)", source, re.M), "conflict marker: " + name)
        ast.parse(read("tests/phase3_ipc_static_check.py"))
    except (ValueError, OSError, SyntaxError) as error:
        print("FAIL: " + str(error), file=sys.stderr)
        return 1
    print(f"PASS: {len(files)} C++ files; includes, delimiters, literals and conflict markers")
    print(f"PASS: {scenarios} test functions invoked by main; {targets} targets, {registrations} CTest entries")
    print(f"PASS: unchanged legacy types/frame sizes; three new requests; {examples} valid JSON examples")
    print("PASS: runtime query/sink wiring, subscription sequence boundary, bounded queues and gap disconnect")
    print("PASS: task artifact whitespace/final newlines and Python syntax")
    print("LIMIT: source only; C++ types/linking, socket behavior and concurrency are unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
