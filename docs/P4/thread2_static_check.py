"""T2 source-only audit. Does not compile or execute C++ tests.

The unchanged P3 aggregate checker still requires the removed SM retry owner;
that incompatibility is reported separately in implementation_thread2.md.
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from phase2_static_check import check_source, check_registration, masked_source, require
from phase3_validation_static_check import check_explicit_target_coverage, check_scenario_coverage

ARTIFACTS = (
    "include/runtime/recovery.hpp", "include/runtime/recovery_manager.hpp",
    "src/runtime/recovery_manager.cpp", "include/runtime/service_manager.hpp",
    "src/service/service_manager.cpp", "include/runtime/config_manager.hpp",
    "src/config/config_manager.cpp", "tests/recovery_manager_tests.cpp",
    "tests/recovery_dependency_tests.cpp", "tests/runtime_core_tests.cpp",
    "tests/phase2_integration_tests.cpp", "docs/P4/thread2_static_check.py",
    "docs/P4/implementation_thread2.md",
)
PROTECTED = (
    "include/runtime/process_supervisor.hpp", "src/service/process_supervisor.cpp",
    "include/runtime/runtime_manager.hpp", "src/runtime/runtime_manager.cpp",
    "include/runtime/service_aggregation.hpp", "src/runtime/service_aggregation.cpp",
    "include/runtime/monitor.hpp", "src/monitor/monitor.cpp", "src/ipc/ipc_manager.cpp",
    "src/ipc/frame.hpp", "src/ipc/frame.cpp", "include/runtime/device_state.hpp",
    "include/runtime/device_state_manager.hpp", "src/runtime/device_state_manager.cpp",
    "tests/phase3_validation_static_check.py", "tests/phase2_static_check.py",
)


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def main():
    files = [p for folder in ("include", "src", "tools", "tests")
             for p in (ROOT / folder).rglob("*") if p.suffix in (".hpp", ".cpp")]
    scenarios = sum(check_source(path) for path in files)
    targets, registrations = check_registration()
    for name in ARTIFACTS:
        source = read(name)
        require(source.endswith("\n"), "missing newline: " + name)
        require(all(line == line.rstrip() for line in source.splitlines()), "trailing whitespace: " + name)
    for name in PROTECTED:
        baseline = subprocess.run(["git", "show", "HEAD:" + name], cwd=ROOT,
                                  capture_output=True, check=True).stdout.decode("utf-8-sig")
        require(read(name) == baseline.replace("\r\n", "\n"), "protected file changed: " + name)
    sm = masked_source(read("src/service/service_manager.cpp"))
    header = masked_source(read("include/runtime/service_manager.hpp"))
    rm = masked_source(read("src/runtime/recovery_manager.cpp"))
    config = masked_source(read("src/config/config_manager.cpp"))
    require(not re.search(r"\b(?:restart_at|restart_delay|maximum_restarts|restart_policy)\b", sm + header),
            "SM retained retry/policy owner")
    require("++service.status.restart_count" not in sm, "SM increments budget")
    require(rm.count("++slot.attempts_reserved_total") == 1, "reservation owner is not unique")
    for symbol in ("classifyExit", "reserveLocked", "terminal", "tick", "launch_gate_", "finalizing"):
        require(symbol in rm, "missing RM policy/finish boundary: " + symbol)
    require("now >= slot.active->request.deadline" in rm and "now + delay" in rm, "missing deadline/due")
    require("2u << slot.attempts_reserved_total" in rm and "maximum_reservations = 5" in rm,
            "wrong frozen automatic budget")
    require("!configured() &&" in rm, "external terminal bypasses configured policy")
    for symbol in ("prepareRecovery", "launchRecoveryAttempt", "finishRecoveryFailure", "releaseRecovery",
                   "projectRestartCount", "launched_generation", "termination_requested"):
        require(symbol in sm, "missing SM primitive/instance cleanup: " + symbol)
    for symbol in ("86400", "3600", "ConfigManager::validate", "ConfigManager::recoveryTimeout"):
        require(symbol in config, "missing shared config validation: " + symbol)
    require(not re.search(r"\b(?:thread|async|fork|execve|waitpid)\s*[({]", rm), "RM introduced worker/backend")
    check_explicit_target_coverage()
    check_scenario_coverage()
    print(f"PASS (static): {len(files)} C++ sources; {scenarios} test functions wired; {targets} targets/{registrations} CTest entries")
    print(f"PASS (static): {len(PROTECTED)} protected files unchanged; T2 artifacts, owner/primitive/timeout boundaries")
    print("PASS (static): unchanged P3 441-entry explicit-target matrix and declared recovery scenarios")
    print("LIMIT: source structure only; C++ types/linking and runtime behavior remain unverified")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, subprocess.CalledProcessError) as error:
        print("FAIL (static): " + str(error), file=sys.stderr)
        sys.exit(1)
