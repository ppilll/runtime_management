# Phase 1 Runtime Core contract

This document fixes the implementation boundary for the Runtime Core modules. It does not change `ipc_protocol.md`.

## Ownership and event flow

- `runtime_manager` loads configuration, owns the internal event queue, dispatches events, and polls the process supervisor for exits. An IPC adapter can submit `start`, `stop`, and `heartbeat` through `RuntimeManager::post`; it can read status through `RuntimeManager::query`.
- `service_manager` is the only writer of `ServiceState`, PID, last heartbeat time, and restart count. All lifecycle commands, process exits, and health events pass through its serial `handle` method on the event loop thread.
- `process_supervisor` only starts, signals, and reaps child processes. It does not decide service state or restart policy.
- `monitor` tracks heartbeat deadlines on its worker thread and emits only `health_missed` events. It does not change service state. A heartbeat resets its consecutive miss count. Checks from the timer thread and watch updates from the event loop are synchronized.
- `logger` queues line-oriented diagnostics and writes them to stdout on its worker thread. Destruction drains the queue.

The queue is internal to the runtime process. It does not define a socket frame or a new IPC message type.

## Configuration and state

The JSON root is one service object or `{ "services": [ ... ] }`. `service_name` and `executable` are required. `arguments` defaults to an empty array, `autostart` to false, `dependency` to an empty array, `startup_timeout` and `heartbeat_timeout` to 15 seconds, and `restart_policy` to `never`. Dependencies may be a string or an array of strings. The loader rejects duplicate service names and preserves configuration order. Phase 1 parses `dependency` but does not build a dependency graph, validate cross-service references, sort services, or gate startup on dependencies.

Successful `execv` is the Phase 1 start completion signal: `CREATED`/`STOPPED` → `STARTING` → `RUNNING`. The process supervisor bounds this handshake by `startup_timeout`; an exec error or timeout enters `FAILED`. A stop command enters `STOPPING` until the child is reaped, then `STOPPED`. A clean spontaneous exit enters `STOPPED` unless the policy is `always`. An abnormal exit or three consecutive heartbeat misses enters `FAILED`. For `on-failure` or `always`, recovery is scheduled as `RECOVERING` with delays of 2, 5, 10, 30, and 60 seconds, capped at five attempts. A new heartbeat resets the miss counter but not the restart counter.

For STOP and failure cleanup, the supervisor sends SIGTERM first. If the child is still present after two seconds, the service manager asks the supervisor to send SIGKILL; the state changes to `STOPPED` only after reaping on a STOP path. A late health event from an earlier process start cannot fail its replacement.

The monitor's first missed check occurs at `heartbeat_timeout` after process start or the last heartbeat. Each further miss occurs at the five-second monitoring interval. The third consecutive miss triggers failure. Event timestamps use the runtime's monotonic clock; a socket adapter should use receipt time for heartbeat events.

The runtime uses a one-second `timerfd` on the Timer Thread to request Monitor Thread checks. The Event Loop Thread remains the only dispatcher to `ServiceManager::handle`; the IPC Handler Thread owns sockets, and the Logger Thread owns log output.

Phase 1 Runtime Core exposes the IPC integration seam but does not implement the socket transport, fake service, or any business service. Those are assigned to the IPC and Test work in `thread_plan.md`.

## Build and test

On Linux with CMake: `cmake -S . -B build`, `cmake --build build`, and `ctest --test-dir build --output-on-failure`.

