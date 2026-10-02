# Phase 3 Thread 1: Device State Manager Foundation

This is the original implementation record. Subsequent review remediation and
current integration contracts are documented in [review_fixes.md](review_fixes.md).

Implemented on 2026-10-02 (Asia/Shanghai). Scope is the state foundation and
RuntimeManager's internal integration. Thread 2 aggregation, Thread 3 IPC and
Monitor implementation are deferred to their assigned tasks.

## Modules and interfaces

- `include/runtime/device_state.hpp`: seven device states, nine explicit
  transition triggers, `DeviceStateEvent`, `DeviceStateSnapshot` and
  `DeviceTransitionResult`. Device state is independent of `ServiceState`.
- `include/runtime/device_state_manager.hpp` and
  `src/runtime/device_state_manager.cpp`: `DeviceStateManager`, the single owner
  of its state, with the complete fixed table from `STATE_TRANSITION.md`.
- `DeviceStateManager::handle(event)` accepts only documented state/event pairs.
  A missing source or reason returns `invalid_metadata`; other undefined pairs
  return `invalid_transition`. Rejected events preserve the whole snapshot and
  produce no notification. There is no public state setter or global state.
- `DeviceStateManager::query()` returns a copy containing current state, previous
  state, source, reason and timestamp. Construction starts at BOOTING with
  previous BOOTING, source `runtime_manager` and reason `runtime starting`.
- Optional `StateChangeSink` receives each accepted snapshot after releasing the
  mutex. It may query the manager. Exceptions propagate after the transition has
  committed; observers must not throw. The runtime observer writes a state-change
  log. Callbacks are not invoked for construction or rejected triggers.
- `RuntimeManager::queryDeviceState() const` is the internal health query;
  `RuntimeManager::post(DeviceStateEvent)` wraps a trigger in the existing FIFO
  `EventQueue`. The appended optional payload preserves existing aggregate
  initializers and the service event field order. The runtime loop routes device
  events exclusively to their owner; other events retain service routing.
- Once runtime threads/timers initialize, `run()` explicitly submits
  `runtime_initialized` to the manager on the runtime thread, before processing
  queued events. This moves BOOTING to READY. Producers may queue events before
  `run()`; they are processed after this initialization boundary.

Timestamps use `std::chrono::steady_clock::time_point`, identical to existing
`runtime::Clock`. They represent producer event time, not UTC/wall-clock time.
Accepted events preserve that time, including equal or earlier timestamps;
FIFO processing order controls transitions. A timestamp is not a freshness
check. No serialization/persistence format is introduced here.

Mutation and snapshot reads use the same mutex. RuntimeManager serializes all
device event handling on its event-loop thread. Standalone users may read
concurrently, but must serialize writers for ordered callback delivery. The
manager protects state mutation even with concurrent writers; callbacks after
unlock can arrive out of order in that use case. Reentrant event submission
should use the runtime queue, rather than recursive `handle()` calls.

## Changed files

New files:

- `include/runtime/device_state.hpp`
- `include/runtime/device_state_manager.hpp`
- `src/runtime/device_state_manager.cpp`
- `tests/device_state_manager_tests.cpp`
- `tests/phase3_state_static_check.py`
- `docs/P3/device_state_manager_implementation.md`

Modified files:

- `include/runtime/event.hpp`
- `include/runtime/runtime_manager.hpp`
- `src/runtime/runtime_manager.cpp`
- `CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/runtime_core_tests.cpp`

No service business logic, IPC implementation/protocol, Monitor implementation,
service aggregation or recovery policy is added. Build registration uses the
existing CMake targets; no new build framework or external dependencies.

## Verification

Per the user's isolated-environment instruction, validation is source-only.
No compiler, cross compiler, CMake, CTest or runtime executable is invoked.

Commands:

```text
python -B tests/phase3_state_static_check.py
python -B tests/phase2_static_check.py
git diff --check
```

The Phase 3 checker reuses the Phase 2 source/include/delimiter and test
registration checks. It checks all seven state labels and compares the exact
nine implementation table entries and C++ test expectations to the documented
table. It traverses the extracted table for normal boot, critical failure,
recovery entry, warning clearance/escalation and recovery success/failure.
This checks table declarations, not execution of the C++ transition function.
Whitespace/conflict checks also include the new untracked files.

Results: both static scripts passed. They checked 29 C++ source/header files,
41 test functions invoked by `main`, six test targets and seven CTest entries.
The Phase 3 table comparison and all seven extracted-table paths passed, with
nine allowed and 54 undefined state/event pairs. Internal interface/event wiring
and all task artifact whitespace checks passed. `git diff --check` passed;
Git emitted LF-to-CRLF normalization notices, with no whitespace errors.

Prepared C++ tests cover:

- BOOTING -> READY -> RUNNING, exact event metadata and copied-query isolation.
- RUNNING -> ERROR -> RECOVERING.
- All nine transitions and all 54 undefined pairs in the 7 x 9 matrix, including
  duplicate events and terminal OFFLINE behavior; notification counts and data.
- Missing source/reason, unknown event values and unknown state labels.
- Observer queries after commit (including lock-release behavior).
- Runtime ownership, the explicit initialization trigger, FIFO internal device
  events, metadata retention and unchanged service lifecycle handling.

The standalone test is registered as `phase3_device_state_unit` with a 10-second
timeout; runtime integration scenarios are in `runtime_core_unit`.
Executable tests are prepared but unrun. Static validation cannot establish C++
type/link correctness, OS behavior, asynchronous behavior or concurrency safety.

## Architecture concerns and next-thread contract

1. `STATE_TRANSITION.md` defines READY on runtime initialization; the state model
   describes READY as required services already started. This implementation
   follows the explicit transition table. Thread 2 must emit
   `required_services_ready` after evaluating actual required service health to
   enter RUNNING. Until connected, runtime initialization alone leaves READY.
2. Service started/stopped/failed facts do not map directly to device transitions
   in this task. Thread 2 uses existing `ServiceManager::StateChangeSink`, evaluates
   aggregate health/criticality, and submits an explicit `DeviceStateEvent`.
   Recovery success likewise requires aggregate health evaluation before sending
   `recovery_succeeded` or `issue_recovered`; an individual service recovery must
   not force RUNNING. No criticality assumptions are made by the foundation.
3. The table defines no startup failures from BOOTING/READY, no shutdown-to-OFFLINE
   edge, no failures during RECOVERING except `recovery_failed`, and no exit from
   OFFLINE. Undefined triggers are rejected rather than inventing transitions.
   Shutdown retains the last device snapshot. These paths need an explicit
   documented policy before later integration expands the state machine.
4. `RECOVERING` is absent from the model's priority list. Aggregation must resolve
   this with documented recovery semantics; the foundation has no priority engine.
5. Device and service snapshot queries are individually consistent; they are not
   an atomic cross-module snapshot. Ordered event production, stale service
   generation handling and multiple-failure priority remain Thread 2 concerns.
