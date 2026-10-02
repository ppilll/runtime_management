# Internal Event Model


## Purpose


Provide asynchronous communication between runtime modules.


## Event Source


Sources:


- Service Manager
- Monitor
- Heartbeat
- Recovery Manager



## Events


## SERVICE_STARTED


Meaning:

Service entered RUNNING.


---


## SERVICE_FAILED


Meaning:

Service failed.


Data:

- service name
- error reason


---


## HEARTBEAT_TIMEOUT


Meaning:

Service heartbeat lost.


---


## RESOURCE_WARNING


Meaning:

System resource pressure.


---


## RECOVERY_START


Recovery begins.


---


## RECOVERY_SUCCESS


Recovery completed.


---


## RECOVERY_FAILED


Recovery failed.



## Event Bus Decision


Use internal lightweight event dispatcher.


Do not introduce external message system.


Forbidden:


MQTT

Kafka

Redis

## Thread 2 dispatcher contract

`RuntimeEventType` includes SERVICE_STARTED, SERVICE_FAILED, SERVICE_STOPPED,
HEARTBEAT_TIMEOUT, RESOURCE_WARNING, RECOVERY_START, RECOVERY_SUCCESS and
RECOVERY_FAILED. Each fact contains service name (when service-scoped), source,
reason, steady-clock producer time and an active flag for resource warnings.
Unknown services/types and empty source/reason are rejected by aggregation.

External producers call `RuntimeManager::post(RuntimeEvent)`, which wraps facts
in the mutex-protected EventQueue. Enqueue order, not timestamp sorting, defines
delivery order; producer timestamps are preserved as metadata. Subscriptions
are installed at runtime construction. No external thread may access the
single-writer dispatcher or aggregator directly.

The runtime loop handles one queued command/fact, then drains all causally
generated ServiceManager facts before the next command. It also drains facts
after the existing service timer tick. Dispatcher subscribers run in registration
order. Nested publication appends to its internal FIFO and cannot interrupt
delivery of the current event. Callbacks must not throw; an exception propagates
and stops normal dispatch rather than silently continuing with incomplete health.

ServiceManager's existing post-lock state-change callback supplies lifecycle
facts; its internal lifecycle/restart behavior is unchanged. Monitor health misses
become HEARTBEAT_TIMEOUT only when ServiceManager validates and accepts the miss
as a failure. Stale/rejected misses cannot change aggregate health. Failure facts
include the process exit status when available; exec/start failure details remain
in the existing ServiceManager log. No IPC messages or hardware monitors change.

## Review remediation contract (2026-10-02)

Named service facts must carry a nonzero `RuntimeEvent::generation` captured
from the lifecycle owner. `ServiceStatus` and `ServiceStateChange` expose that
token. New launches, new failures and explicit stops advance it; RUNNING and
RECOVERING retain their initiating token, and STOPPING -> STOPPED shares one stop
token. The callback carries its captured token rather than querying newer status.
Recovery request producers must capture the fault token when starting work and
return it unchanged. Missing, stale and future recovery-result tokens are
rejected; timestamps remain FIFO metadata and are not freshness checks.
Duplicate facts for the same generation are idempotent. An explicit new stop
invalidates prior recovery results and is reported as unavailable from STOPPING.
These are internal API additions, not changes to Phase2 JSON or IPC frames.

Lifecycle FAILED facts now additionally carry `recovery_exhausted` when an enabled
automatic restart policy has already used its five retries. RuntimeManager emits
the failure fact followed by one matching RECOVERY_FAILED for this case only.
First failures and `restart_policy=never` are not terminal recovery results.
Phase2 owns its existing automatic retry budget; a future Phase4 coordinator
must not duplicate these terminal results, and must bind its own work to tokens.

RESOURCE_WARNING retains its type number/name but carries `ResourceSeverity`
(`warning` or `critical`, default warning). The source key is the resource
identity: producers use separate stable sources for separate resource signals.
An active update replaces that source's severity; `active=false` clears only
that source. Resource facts are device-scoped and do not use service generations.
See MONITOR_INTEGRATION.md for thresholds and measured-data submission.
