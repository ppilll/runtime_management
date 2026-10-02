# Phase 2 Thread 4: IPC and configuration implementation

## Scope and integration

Implemented `codex/thread4_ipc_prompt.md` on top of the existing uncommitted
Service Manager, recovery/dependency and process backend changes. Product changes
are limited to IPC and Config Manager. Runtime Manager, Service Manager, Monitor
and Process Supervisor are unchanged by Thread 4. No build configuration or
external dependency was added.

The wire contract, configuration defaults and validation rules are in
`ipc_config_extension.md`. Phase 1 protocol behavior remains available, with types
6 (RESTART_SERVICE) and 7 (GET_SERVICE_LIST) added to the existing frame enum.

The current Runtime API exposes only `post` and single-service `query`. IPC's
entrypoint therefore reads static definitions from the same startup configuration
file and gives IpcManager that snapshot. Keep this file stable during startup;
Runtime and IPC each load it once. Callers of the existing four-argument
constructor retain their behavior; its default definition snapshot is empty.
Callers wanting GET_SERVICE_LIST must supply the complete static definitions as
the fifth argument. This is not a mutable registry or dynamic registration API.

Manual restart sequences queued STOP, observed STOPPED/no PID, and queued START.
The IPC thread tracks only accepted requests and never writes lifecycle state,
signals a process or implements retry policy. Its existing 200 ms epoll timeout
also drives pending-request polling. The runtime owns actual stopping, reaping,
dependency startup and failure recovery. No runtime restart callback or event
type was invented to bypass the task's module boundary.

The new loader populates the Phase 2 fields already present in ServiceConfig.
The existing process backend consumes environment and working_directory, and
Service Manager consumes shutdown_timeout. Per-service escalation honors this
timeout; Runtime's separate existing four-second global shutdown drain remains
outside this task's modification boundary.

## Regression source

`tests/ipc_integration_tests.cpp` extends the existing registered test executable:

- Old configuration defaults, array roots, declaration order, environment
  overrides/empty values/embedded equals, working directory and timeout boundaries.
- Invalid field types, malformed environment entries, embedded NUL, nonpositive,
  oversized, fractional and overflowing shutdown timeouts.
- List request validation, channel restrictions, request IDs, configuration order,
  JSON escaping, empty lists, missing-snapshot/oversized-list errors and subsequent
  connection use.
- Fresh/stale/missing heartbeats and created/running/stopped/failed health views.
- Manual restart under policy never, old PID reaping and replacement PID, service
  stop notification, repeated pending requests and restart of stopped/failed services.
- Mocked runtime snapshots check PID clearance, requester disconnect survival and
  cancellation by a subsequent explicit STOP, without relying on process exit timing.
- Existing Phase 1 framing, status, heartbeat, stop and forced-stop scenarios remain.

The source remains registered in the existing test target; it was not compiled
or executed in the isolated environment.

## Static validation

The user requested static validation only. No CMake, compiler, cross compiler,
CTest, Unix socket test or service executable was run.

Passed static checks cover diff whitespace, C++ delimiter/literal/comment structure,
project-local include resolution, merge markers, enum value compatibility,
declaration/definition correspondence, test entrypoint registration and JSON
examples. Manual review covers asynchronous STOP/reap/START sequencing,
duplicate/cancellation behavior, payload bounds, health derivation and defaults.

Static checks do not establish C++ type correctness, successful linking or runtime
behavior. The regression source is provided for later validation on Linux.
