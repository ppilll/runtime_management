# Phase 3 Thread 2: Event System and Service Aggregation

This is the original implementation record. Generation validation, resource
severity and terminal retry results were subsequently added; see
[review_fixes.md](review_fixes.md) for current contracts and static results.

Implemented on 2026-10-02 (Asia/Shanghai), on top of the existing Thread 1 work.
Verification follows the user's isolated-environment instruction: static checks
only. No compiler, cross compiler, CMake, CTest or executable tests are run.

## Changed files

Added:

- `include/runtime/service_aggregation.hpp`
- `src/runtime/service_aggregation.cpp`
- `tests/event_aggregation_tests.cpp`
- `tests/phase3_aggregation_static_check.py`
- `docs/P3/event_aggregation_implementation.md`

Modified for Thread 2:

- `include/runtime/event.hpp`, `src/runtime/event.cpp`: typed internal facts and
  lightweight FIFO dispatcher, retaining existing command/envelope fields.
- `include/runtime/runtime_manager.hpp`, `src/runtime/runtime_manager.cpp`:
  queued producer API, lifecycle/heartbeat adapter and dispatcher drain points.
- `include/runtime/device_state.hpp`, `include/runtime/device_state_manager.hpp`,
  `src/runtime/device_state_manager.cpp`: optional aggregate target and checked
  aggregate transition triples. The original nine no-target edges are retained.
- `CMakeLists.txt`, `tests/CMakeLists.txt`: existing build/test registration only.
- `docs/P3/EVENT_MODEL.md`, `docs/P3/AGGREGATION_RULE.md`: event ordering and explicit
  startup/partial-recovery policy.

Earlier Thread 1 changes and untracked files were present before this task and
were preserved. Service implementation, service lifecycle/restart code, IPC
protocol, Monitor implementation and hardware monitoring are unchanged.

## Event design

Eight internal fact types support all seven requested events plus RECOVERY_START.
Facts contain service name, producer source, reason and steady-clock time.
RESOURCE_WARNING additionally carries `active`, with false clearing only its
producer's resource warning. No external dependencies, broker or database are
introduced.

`RuntimeManager::post(RuntimeEvent)` is the thread-safe producer entrypoint. It
appends an envelope to the existing mutex-protected queue. The runtime thread
publishes internal facts to EventDispatcher and drains them before handling the
next queued command. Subscriber registration order defines callback order.
Nested publications append to the dispatcher FIFO; nested drain does not recurse.
Producer timestamps are metadata and are never used to sort or reorder events.

ServiceManager's existing callback runs after its registry lock is released.
The adapter converts RUNNING/FAILED/STOPPED/RECOVERING into service/recovery
facts. Returning to RUNNING after failure emits SERVICE_STARTED followed by
RECOVERY_SUCCESS. Accepted heartbeat misses emit HEARTBEAT_TIMEOUT; rejected
stale misses never reach aggregation. Process failures include exit status when
available; the exact exec/start exception remains in the ServiceManager log.
The adapter drains after both command handling and service ticks, avoiding the
race where a queued shutdown/recovery command overtakes an earlier derived fact.

## Aggregation and configuration

ServiceAggregation keeps its own ordered service health records and resource
warning sources. It does not query a newer ServiceManager snapshot while replaying
an older fact. Failure and heartbeat flags remain latched until that service
starts or succeeds in recovery. Stopping a failed process does not clear them.

Defaults: control HIGH/required, vision MEDIUM/optional, OTA LOW/optional. Thus
control failure gives ERROR; vision or OTA failure gives WARNING. Any accepted
heartbeat timeout gives ERROR. Unknown services default to LOW; their autostart
flag gates readiness. A fault-free inactive registry stays READY.

Static configuration is supplied through the optional second constructor
argument, without expanding the Phase 2 JSON or IPC schema:

```cpp
runtime::AggregationOptions options;
options.vision_required = true;
options.policies["aux_service"] = {runtime::ServiceCriticality::high, true};
runtime::RuntimeManager manager(config_path, options);
```

The existing executable supplies no second argument and therefore uses defaults.
`vision_required` always promotes vision to HIGH/required, including when a
per-service override is supplied.

Critical failures dominate optional failures and resource warnings. Device
RECOVERING requires active recovery of all critical issues, or explicit
device-wide recovery coordination. A new failure cancels device-wide recovery.
Recovery success recalculates all remaining issues. Device-wide success does
not assert that individual services recovered. A failed critical/heartbeat
recovery or device-wide recovery enters terminal OFFLINE; optional recovery
failure remains WARNING. Normal runtime shutdown retains the last health
snapshot, matching Thread 1.

The original state table cannot express READY startup failure or a direct
RECOVERING/ERROR -> WARNING after partial recovery. Fifteen explicit aggregate
triples, documented in AGGREGATION_RULE.md, supplement it only when the event
includes `health_target`. The manager validates the complete state/event/target
triple, and remains the only writer of its snapshot. This avoids temporary
RUNNING notifications and retains all original undefined-pair rejection tests.

## Static verification and scenario coverage

Commands:

```text
python -B tests/phase3_aggregation_static_check.py
python -B tests/phase3_state_static_check.py
python -B tests/phase2_static_check.py
git diff --check
```

Results: all three static scripts and `git diff --check` passed. They checked
32 C++ files, 54 test functions invoked by main, seven executable test targets
and eight CTest entries. Thread 1's original nine edges and 54 undefined pairs
still match its contract. Thread 2's 15 extra triples match the documented
policy, and all nine extracted-table scenario paths passed. Artifact whitespace,
includes, delimiters, event labels and adapter checks passed. Git emitted only
LF-to-CRLF normalization notices, with no whitespace errors.

The aggregation checker extracts C++ transition declarations, compares all extra triples to the
documented policy, traverses expected scenario paths, checks event enum/labels,
default policies, priority branch order, runtime/dispatcher wiring, registration,
local includes, delimiters and whitespace (including new untracked artifacts).
It does not execute or type-check the C++ algorithm.

Prepared C++ test scenarios (registered, not executed):

| Task case | Expected behavior | Static verification |
| --- | --- | --- |
| 1: vision failure | RUNNING -> WARNING | Declared edge and C++ case/wiring checked |
| 2: control failure | RUNNING -> ERROR | Declared edge and C++ case/wiring checked |
| 3: multiple failures | HIGH dominates; six failure order permutations | Escalation path and C++ case/wiring checked |
| 4: recovery | Recalculate all failures; partial recovery -> WARNING | Direct aggregate recovery edge and C++ case/wiring checked |

Additional prepared cases cover OTA, required vision and static overrides,
startup failure, stopped services, heartbeat flags surviving stop, resource
warning sources/clearing, critical recovery failure and terminal OFFLINE,
multiple simultaneous critical recoveries, device-wide recovery recalculation,
invalid/duplicate events, checked target rejection, FIFO/nested publications,
and RuntimeManager's launch-failure adapter followed by resource warning/shutdown.

Static checks establish source structure and declared contracts only. C++ type
and link correctness, actual scenario behavior, Linux process/timer behavior,
and concurrency remain unverified in this environment.

## Possible races and limitations

1. Dispatcher/aggregator mutations assume a single runtime-loop writer. External
   producers must use RuntimeManager::post; direct concurrent mutation is invalid.
   No callback holds the ServiceManager registry lock or device snapshot mutex.
2. Concurrent producers are ordered by queue-lock acquisition, which depends on
   scheduling. Delivery is deterministic once that order is established; event
   timestamps do not establish a global causal order across independent producers.
3. ServiceManager validates heartbeat/process facts against current lifecycle,
   PID and timing before the adapter publishes them. Future external recovery
   producers must similarly publish current facts. RuntimeEvent does not carry a
   restart-generation token; an externally posted stale recovery success could
   incorrectly clear a newer failure. Generation-aware recovery is future work.
4. Service queries and device queries are separately consistent, not one atomic
   cross-module snapshot. A reader can see updated service state before its
   derived device fact is drained. DeviceStateManager queries themselves return
   a consistent copy under its mutex.
5. Subscriber callbacks must not throw or directly mutate state reentrantly.
   Exceptions propagate after any prior state commit, so the remaining subscribers
   of that fact will not run. Future observers should post facts through the queue.
6. The queues have no capacity limit or backpressure, consistent with the existing
   event queue. Sustained producer flooding can delay shutdown and grow memory.
   Resource warnings require an explicit clearing fact from the same source.
7. Phase 2 restart execution is retained. This task exposes recovery facts and
   applies health policy; it does not implement a new recovery strategy/coordinator.
   OFFLINE remains terminal for this runtime's lifetime.
