# Phase 2 Thread 5 verification and review

The shutdown defect described below was subsequently addressed on 2026-10-02.
See `docs/P3/review_fixes.md` for the configured-deadline/reap implementation and
static verification. This file preserves the original review and test evidence.

Reviewed on 2026-10-01 (Asia/Shanghai) against the working-tree Phase 2 modules,
`docs/P2/testing_strategy.md` and `docs/P2/recovery_dependency_design.md`.
The workspace already contained uncommitted Phase 2 implementation and tests.
This task changes only test code, test registration and test utilities/reporting.
Production code and its existing modifications are preserved.

## Result and validation limits

**Static checks passed. C++ executable tests were not compiled or run.**
Per the user's isolated-environment instruction, no CMake, CTest, compiler,
cross compiler, runtime binary or child fixture was executed. No tools installed.
The source tests below are coverage prepared for later Linux execution, not
evidence that the service behavior has passed. One shutdown defect is inferred
from source inspection; the separate regression is expected to expose it.

Executed checks:

- `python tests/phase2_static_check.py`: passed for 25 C++ source/header files,
  including quoted project-local includes, balanced delimiters after masking
  comments/literals, terminated comments/literals and conflict markers.
- The same script found all 33 test functions invoked by their executable's
  `main`, and verified five test executable targets with six CTest registrations,
  source existence, linkage declarations and the shutdown-regression argument.
  Both new integration entries have a 60-second timeout and no failure masking.
- `git diff --check`: passed for tracked workspace changes. Git reports the
  repository's LF-to-CRLF normalization warnings; no whitespace errors reported.
- Manual review compared the public APIs, state transitions, raw waitpid status,
  timeout/backoff boundaries, PID clearing, monitor events, dependency order and
  cleanup with the implementation. The new test and checker files were also
  inspected for whitespace and conflict markers, including untracked files.

The script checks source structure and registration text. It does not parse C++
types, preprocess conditional compilation, validate external Linux headers,
configure CMake, link binaries or validate asynchronous runtime behavior.

## Coverage

| Required scenario | Existing coverage | Added cross-module coverage in `phase2_integration_tests.cpp` |
| --- | --- | --- |
| Service startup | `runtime_core_tests.cpp`, `process_lifecycle_tests.cpp`, IPC START/status | `test_start_and_graceful_shutdown`: real child readiness, live/tracked PID, RUNNING, CREATED -> STARTING -> RUNNING, duplicate start |
| Service shutdown | SIGTERM, timeout SIGKILL, STOPPING -> STOPPED, IPC STOP | Same startup/shutdown test: graceful exit, PID/start-time clearing, monitor removal, no restart under `always`; heartbeat test: real SIGTERM-resistant child and SIGKILL; independent long-grace runtime regression |
| Process crash | Simulated raw exit events and policy matrix | `test_real_crash_backoff_and_restart_limit`: actual SIGKILL, backend reap, preserved signal status, ServiceManager recovery and live replacement |
| Heartbeat timeout | Monitor deadline/reset, stale events, IPC heartbeat silence under `never` | `test_heartbeat_timeout_escalation_and_recovery`: accepted heartbeat then silence, exact configured deadline, recovery under `on_failure`, SIGTERM/SIGKILL, no replacement before reap |
| Restart limit | Five retries and failed-launch exhaustion with fake backend | Crash test: six real child crashes, exact 2/4/8/16/32 second logical backoff boundaries, duplicate-event resistance, fifth retry count retained, sixth failure stays FAILED with no PID |
| Dependency order | Diamond/shared prerequisites, invalid graphs, failure propagation, cancellation | `test_real_dependency_start_stop_order`: real diamond child launches prerequisite-first, stop requests dependent-first, all four exits reaped and statuses STOPPED without recovery |

The normal entry `phase2_lifecycle_integration` runs four scenario groups covering
all six requirements. `phase2_runtime_shutdown_regression` runs the independently
selected `--shutdown-regression` branch so the suspected defect does not suppress
the normal suite. Existing unit, process, recovery/dependency and IPC tests remain
registered.

The new fixture re-execs `/proc/self/exe` with a readiness pipe after installing
its SIGTERM disposition. It delegates process calls to PosixProcessSupervisor,
records calls for ordering assertions and routes the backend's real waitpid
results through ServiceManager's public event API. Heartbeat events enter through
the public API; this new suite does not exercise heartbeat wire transport or the
RuntimeManager event loop except in the shutdown regression. Existing IPC tests
cover the wire, and existing runtime tests cover autostart/event-loop shutdown.
Dependency order asserts launch and signal-request order, not child scheduling or
exit completion order, as specified by the recovery/dependency contract.

For normal scenarios, logical clock values advance backoff/health/grace deadlines
without wall-clock waits. OS launch, readiness, signals and reaping remain real.
The shutdown regression uses wall time, `shutdown_timeout=6`, a child ignoring
SIGTERM and a 60-second heartbeat timeout to isolate shutdown. Readiness and reap
polls are bounded at three seconds; CTest bounds each entry at 60 seconds. The
supervisor destructor cleans up owned children on test failure. The runtime
thread joins before RuntimeManager destruction, and temporary config cleanup is
automatic. The CTest timeout has not been assessed on loaded ARM64 hardware.

## Failures and suspected root cause

### P1: Runtime shutdown returns before long configured grace periods finish

**Classification:** source-supported defect; expected regression failure,
not an observed executable-test failure.

Evidence:

- `src/config/config_manager.cpp` accepts `shutdown_timeout` from 1 to 3600 seconds.
- `src/service/service_manager.cpp`, `stop()`, schedules escalation at
  `now + service.config.shutdown_timeout`; `tick()` force-stops at that deadline.
- `src/runtime/runtime_manager.cpp`, `run()`, calls `stop_all()` and then uses an
  independent fixed four-second shutdown deadline. When it expires, `run()`
  restores signal handlers and returns without ensuring all child PIDs cleared.

Reproducer in the independent regression: autostart a ready child that ignores
SIGTERM with a six-second shutdown timeout, request runtime shutdown and join
`run()`. The source path implies return after roughly four seconds of shutdown
polling with the child still STOPPING and its PID retained, before the six-second
SIGKILL deadline. Destructor cleanup later kills/reaps the child but does not
complete ServiceManager's state update before `run()` returns. In a long-lived
caller, the child can remain alive until RuntimeManager destruction; in the CLI,
destruction can shorten the configured grace period.

Expected contract: wait through the configured grace period, force-stop the
unresponsive child, reap it, report STOPPED with PID -1, and return. The test also
requires at least six seconds from request to completion and no waitable child.
The suspected root cause is the fixed runtime shutdown budget conflicting with
per-service shutdown deadlines. No production fix or test-expectation workaround
was applied. The test is not marked WILL_FAIL, disabled or suppressed.

No other deterministic defect was established in the six requested paths by this
static review. Compiling and running on Linux remains necessary to confirm the
suspected failure and detect type/link errors, scheduling races, signal behavior
and ARM64/kernel-specific issues. The exponential delay cap of 60 seconds cannot
be reached through the public five-retry budget; existing tests cover all five
reachable delays, without exposing private implementation just to test the cap.

## Repeating the checks

In the isolated environment, from the repository root:

```text
python tests/phase2_static_check.py
git diff --check
```

On a Linux host with a suitable compiler and CMake (not executed here):

```text
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

If cross-compiling, run the built test executables on the Linux target or a
configured emulator; host CTest cannot directly execute ARM64 binaries. Keep
the shutdown regression's intended assertions when addressing the production
issue in a separate authorized implementation task.
