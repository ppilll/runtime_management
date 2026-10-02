# Phase 2 Thread 3: Recovery and dependency implementation

## Scope

Implemented the task in `codex/thread3_recovery_prompt.md` on top of the existing
Service Manager and Process Supervisor changes. Business services and the process
backend were not changed by Thread 3. No external framework was added.

`ServiceManager` owns recovery decisions, retry reservations, dependency ordering
and all lifecycle transitions. `RuntimeManager` validates the full graph during
construction and queues autostart roots in topological order. The implementation
contract is recorded in `recovery_dependency_design.md`.

## Behavior

- Abnormal/unknown process exit, failed launch and expired heartbeat follow
  FAILED -> RECOVERING -> STARTING -> RUNNING when policy and budget allow.
- Heartbeat timeout uses the last accepted heartbeat or successful launch time.
  The first monitor timeout notification triggers recovery (default: 15 seconds).
  Old timestamps and events identifying a different positive PID are ignored.
- The five retry reservations use 2, 4, 8, 16 and 32 seconds. The exponential
  formula is capped at 60 seconds. Reserving a retry consumes the budget even
  if an explicit stop later cancels it; explicit start does not reset the count.
- Recovery waits for the old child to be reaped, and the shutdown deadline still
  escalates SIGTERM to SIGKILL. The sixth failure stays FAILED without a timer.
- A static graph supports forward references and duplicate edges, rejects missing
  dependencies, self-dependencies and cycles, and breaks topological ties by name.
- Starting a service starts its prerequisite closure; prerequisites awaiting
  recovery or stopping prevent dependent launch. Unrelated services are untouched.
- Stop, failure and clean exit propagate to transitive dependents and cancel their
  pending recoveries. Graceful and forced stop requests use reverse topological
  order. Child exit completion remains asynchronous (STOPPING -> STOPPED on reap).
- Prerequisite recovery does not automatically revive stopped dependents. They
  need another explicit start; no dynamic recovery graph or scheduler is present.
- The existing `restartService` callback-routing API is retained. This task adds
  automatic fault recovery; IPC restart command wiring belongs to Thread 4.

## Validation performed

The user requested static validation only in the isolated environment. No CMake,
compiler, cross compiler, CTest, real service process or C++ test executable was run.

Passed:

1. `git diff --check` for tracked workspace changes.
2. Python source-structure checks for the modified Service Manager header/source,
   Runtime Manager source and both relevant C++ test sources: balanced delimiters,
   terminated comments/literals, project-local includes and conflict markers.
3. Service Manager declaration/definition correspondence (25 definitions including
   the constructor), and test-function registration in each executable's `main`.
4. Test CMake source paths and the new recovery test target/CTest registration,
   inspected as text without executing CMake.
5. Manual review of transitions, retry boundaries, stale events, process reaping,
   prerequisite readiness, propagation, cancellation and callback lock release.

These checks do not establish C++ type correctness, successful linking or runtime
behavior. The following executable test sources were added/updated for later Linux
validation; their assertions were reviewed but have not been executed here.

## Regression sources

`tests/recovery_dependency_tests.cpp` adds seven scenario groups using a fake
Process Supervisor and explicit clock values (no sleeps or real process launches):

- Crash transitions, exact backoff boundaries, duplicate events and retry limit.
- Heartbeat boundary/reset, default 15-second deadline, reaping and stale events.
- Restart policy matrix and repeated launch failure budget exhaustion.
- Diamond topology, forward/duplicate edges, startup and reverse shutdown signals.
- Transitive failure propagation, dependent retry cancellation and clean exit.
- Explicit reverse stop, blocked launch and explicit cancellation of recovery.
- Invalid missing/self/cyclic graphs rejected before launch or lifecycle changes.

`tests/runtime_core_tests.cpp` now expects the Phase 2 backoff and heartbeat behavior
and registers the prerequisite needed by the service-model test. The existing
registration, process lifecycle and callback-routing test coverage is preserved.
