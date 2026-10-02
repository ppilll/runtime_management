# Recovery and Dependency Design


## Objective


Implement basic fault recovery.


---

# 1. Crash Recovery


Detection:


Process exit.


Flow:



RUNNING
 |
process exit
 |
FAILED
 |
RECOVERING
 |
restart
 |
RUNNING


---

# 2. Heartbeat Recovery


Heartbeat:


5 seconds


Timeout:


15 seconds



Flow:



RUNNING
 |
heartbeat timeout
 |
FAILED
 |
RECOVERING


---

# 3. Restart Policy


Maximum:


5 times


Backoff:



2s
4s
8s
16s
32s
60s(max)


After limit:


State:



FAILED


No automatic restart.


---

# 4. Dependency Management


Only support static dependency graph.


Example:



runtime_manager
    |

vision_service


---

# Startup


Use:


Topological order



Example:



runtime_manager
    |

vision_service


---

# Shutdown


Reverse order:



vision_service
    |

runtime_manager


---

# Failure Propagation


If dependency fails:


Dependent service enters:


STOPPED


Phase2 does not implement:


- dynamic recovery graph

- dependency migration

- scheduler

## Implementation contract (Thread 3)

- Recovery and dependency orchestration extend `ServiceManager`; only it changes
  lifecycle states. Process signals/reaping remain in `ProcessSupervisor`.
- Automatic recovery reserves at most five attempts per registered service,
  including failed launches. Attempts use `min(2 * 2^attempt, 60)` seconds:
  with the five-attempt budget the delays are 2, 4, 8, 16, 32 seconds. The
  60-second ceiling applies to the backoff formula; it does not add a sixth retry.
  Explicit stop cancels a pending attempt; explicit start retains the used budget.
- `never` disables recovery, `on-failure` recovers abnormal/unknown exit,
  launch failure and heartbeat timeout, and `always` also recovers clean exit.
  Replacement launch waits for both the backoff deadline and old-child reaping.
- Monitor's first timeout notification at the configured deadline triggers failure
  (15 seconds by default), rather than waiting for three further notifications.
  Heartbeats normally arrive every 5 seconds. Old timestamps and mismatched
  positive PIDs are ignored; the timeout is measured from the latest heartbeat
  or successful launch.
- All definitions are registered before graph validation. Missing references,
  self-dependencies and cycles are rejected before lifecycle operations. Duplicate
  edges are harmless. Topological order uses service-name order to break ties.
- Starting a service starts its prerequisite closure in topological order, even
  when a prerequisite has `autostart=false`. Unrelated services remain untouched.
  A prerequisite awaiting recovery or stopping cannot be started prematurely;
  a dependent whose prerequisites are unavailable remains STOPPED.
- Stopping a prerequisite, or its abnormal/clean exit, stops all transitive
  dependents in reverse topological order and cancels their recovery timers.
  Shutdown sends stop requests in reverse topological order. Signals are
  asynchronous: live children remain STOPPING until their exit is reaped.
  This ordering governs stop requests, not child exit completion.
- Recovery restarts only the failed service when its prerequisites are RUNNING.
  Stopped dependents require a new explicit start; no dynamic recovery graph is
  introduced. Runtime validates the graph before starting worker threads.
- Thread 3 validation in the isolated development environment is static only.
  Regression test sources are provided for later execution on a Linux toolchain.
