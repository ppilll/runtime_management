# Service Aggregation Rule


## Purpose


Convert multiple Service State into Device State.


## Service Criticality


Each service has:


HIGH

MEDIUM

LOW



## Rule


## HIGH


FAILED:


Device ERROR



Example:

control_service


---


## MEDIUM


FAILED:


Device WARNING


Example:

vision_service


Exception:

If device configuration requires vision:

ERROR


---


## LOW


FAILED:


Device WARNING


Example:

ota_service



## Priority


Critical failure dominates.


Order:


HIGH failure

>

MEDIUM failure

>

LOW failure



## Recovery


When failed service recovers:


Recalculate device state.

Do not immediately force RUNNING.

## Thread 2 deterministic integration policy

Service facts are serialized on the runtime event loop. The aggregator keeps
each service's running, failed, stopped, heartbeat-lost and recovering flags.
No recovery success clears another service's flags. Duplicate facts do not
produce duplicate device transitions.

Default policies: control HIGH/required, vision MEDIUM/optional, OTA LOW/optional.
Other registered services default to LOW, with autostart gating readiness.
`AggregationOptions` is static device configuration passed to RuntimeManager;
`vision_required=true` promotes vision to HIGH/required. Per-service policy
overrides are also supported. These options do not change the Phase 2 JSON or IPC
schema. A deployment must supply them at construction; the executable retains
the default policies.

An accepted heartbeat timeout is a critical issue even for an optional service,
consistent with DEVICE_STATE_MODEL.md. A stopped service is unavailable and has
its configured failure severity. A stop does not clear a latched failure or
timeout. Merely CREATED services gate required readiness but are not failures.
When no service is running, the fault-free aggregate stays READY.

Priority: terminal recovery failure -> OFFLINE; critical resource pressure or
critical unavailable services -> ERROR; optional unavailable services or warning
resource pressure -> WARNING; required
readiness with at least one running service -> RUNNING; otherwise READY.
RECOVERING replaces ERROR only when all critical issues are actively recovering
or an explicit device-wide recovery coordinator has started. A new failure
cancels that device-wide recovery flag. Named recovery failure goes OFFLINE for
a HIGH service or heartbeat loss; an optional non-heartbeat recovery failure
retains WARNING. Device-wide RECOVERY_FAILED always goes OFFLINE. OFFLINE has no
automatic exit. Device-wide success only recalculates recorded service health.

RESOURCE_WARNING is latched per producer source. The same event with
`active=false` clears that source only. Service recovery cannot clear resource
pressure. No hardware monitoring is added.

The review remediation adds generation validation as specified in EVENT_MODEL.md.
Named results may only clear the matching fault token. A service restart cannot
clear resource pressure. Critical resource pressure remains ERROR even during
service or device-wide recovery; only a measurement downgrade/clear from the same
resource source can resolve it. A new critical resource fact cancels the old
device-wide recovery flag. RecoveryFailed after retry exhaustion keeps optional
non-heartbeat failures at WARNING; HIGH or heartbeat-lost services go OFFLINE.

### Explicit aggregate transition extension

The nine original STATE_TRANSITION.md edges remain unchanged for events without
`health_target`. Aggregate updates carry this optional field and must match
either an original edge or one of the following extra state/event/target triples.
This is needed to report startup failure and partial recovery without emitting a
temporary RUNNING snapshot. Undefined targets, BOOTING exits other than runtime
initialization, and OFFLINE exits remain rejected.

| From | Explicit trigger | Aggregate target |
| --- | --- | --- |
| READY | warning | WARNING |
| READY | critical_failure | ERROR |
| RUNNING | issue_recovered | READY |
| WARNING | issue_recovered | READY |
| ERROR | issue_recovered | READY |
| ERROR | issue_recovered | RUNNING |
| ERROR | issue_recovered | WARNING |
| RECOVERING | recovery_succeeded | READY |
| RECOVERING | recovery_succeeded | WARNING |
| RECOVERING | recovery_succeeded | ERROR |
| RECOVERING | critical_failure | ERROR |
| ERROR | recovery_failed | OFFLINE |
| READY | recovery_failed | OFFLINE |
| RUNNING | recovery_failed | OFFLINE |
| WARNING | recovery_failed | OFFLINE |

Ordinary runtime shutdown suppresses health adaptation and retains the last
device snapshot, matching the Thread 1 shutdown contract. It does not imply a
recovery failure or invent a shutdown state transition.
