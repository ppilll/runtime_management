# Thread3 Device State IPC implementation

This is the original implementation record. Conservative RUNNING health labels
and additional overflow regressions are recorded in [review_fixes.md](review_fixes.md).

## Result

Added GET_DEVICE_STATE (8), GET_HEALTH (9), and SUBSCRIBE_EVENT (10) on the
existing Unix control socket. The complete request/response/event contract is
in [IPC_EXTENSION.md](IPC_EXTENSION.md). Phase1/Phase2 type numbers, frame format,
service commands, heartbeat direction and SERVICE_STOP event are preserved.

Queries read the existing DeviceStateSnapshot and static service definitions;
they do not submit lifecycle events or recompute the device state machine.
GET_HEALTH uses the Phase2 service health projection and adds summary counts.
Both query responses preserve the snapshot's monotonic timestamp in milliseconds.

State subscriptions use a weak-reference, mutex protected producer queue. The
runtime forwards each committed snapshot through its existing callback, and
the IPC thread alone writes sockets. Subscription sequence boundaries prevent
pre-subscription replay. Rapid transitions are queued individually, including
transitions with equal timestamps. Queue and per-client output bounds disconnect
clients on gaps or slow consumption; they do not silently coalesce events.

## Modified files

- `src/ipc/frame.hpp`: additional wire type numbers.
- `src/ipc/ipc_manager.hpp`: optional device query and lifetime-safe event sink.
- `src/ipc/ipc_manager.cpp`: request validation, responses, notification queue,
  subscriptions and output bounds.
- `src/ipc/main.cpp`: bind RuntimeManager's snapshot query and callback to IPC.
- `include/runtime/runtime_manager.hpp`, `src/runtime/runtime_manager.cpp`:
  one optional constructor callback forwarded by the existing committed-state
  observer. This is the minimal integration beyond the IPC directory required
  because Thread1 exposed querying but no observer registration. No service
  lifecycle, aggregation rule or state transition logic was modified by Thread3.
- `tests/device_ipc_tests.cpp`, `tests/CMakeLists.txt`: IPC scenarios registered
  with the existing test framework; no new build framework.
- `tests/phase3_ipc_static_check.py`: compiler-free source/protocol checker.
- `docs/P3/IPC_EXTENSION.md`, this report: wire protocol and delivery details.

Existing Thread1/Thread2 working-tree changes were retained.

## IPC validation

The user requested static checking only in this isolated environment. No CMake,
compiler, cross compiler, CTest or C++ test executable was run.

The added C++ scenarios cover queries and escaping, health counts, invalid fields
and duplicate keys, unsupported events, wrong-channel requests, oversized and
unavailable snapshots, fragmented/coalesced frames, legacy commands/heartbeat,
multiple subscribers, ordered rapid transitions, duplicate subscriptions,
invalid transitions, disconnects, oversized events, optional provider defaults
and sink lifetime after manager destruction. These are authored test cases,
not runtime test results.

Static validation commands:

```text
python tests/phase3_ipc_static_check.py
python tests/phase2_static_check.py
python tests/phase3_state_static_check.py
python tests/phase3_aggregation_static_check.py
git diff --check
```

All five commands passed in this environment:

| Check | Static result |
| --- | --- |
| Phase3 IPC | 33 C++ files; 57 invoked test functions; 8 targets / 9 CTest entries; legacy frame/types preserved; 3 new commands / 4 valid JSON examples; callback and bounded-queue wiring verified |
| Phase2 | Includes, delimiters, literals, conflicts and test registration passed |
| Phase3 state | Seven states; nine original table edges; 9 allowed / 54 undefined pairs; seven extracted-table paths passed |
| Phase3 aggregation | Eight internal events; 24 transition triples; nine extracted-table paths; adapter and dispatcher structure passed |
| git diff --check | No whitespace errors in tracked changes; the IPC checker also checks all new Thread3 artifacts |

These results establish source structure and protocol consistency. C++ type/link
correctness, actual socket behavior and concurrency remain unverified.
