# IPC and Configuration Extension


## Objective


Extend Phase1 communication.


---

# 1. IPC Existing Commands


Keep:



START
STOP
QUERY_STATUS
HEARTBEAT
EVENT


---

# 2. New Commands


## RESTART_SERVICE


Purpose:


Manual recovery request.


Example:



runtime_manager
  |

restart
  |

service


---


## GET_SERVICE_LIST


Purpose:


Query runtime managed services.


Return:


- service name

- state

- pid

- health status


---

# 3. Not Implemented


Do not add:


REGISTER_SERVICE


UNREGISTER_SERVICE


Reason:


Configuration is static in Phase2.


---

# 4. Configuration Extension


Add:


## environment


Purpose:


Runtime environment variables.


---

## working_directory


Purpose:


Process working path.


---

## shutdown_timeout


Purpose:


Graceful termination.


---

# Not Added


## pid_file


Reason:


Runtime owns PID.


---

## health_check


Reason:


Heartbeat already provides basic health model.

---

# Thread 4 wire and configuration contract

The Unix stream sockets, 10-byte little-endian header, UTF-8 JSON payload,
64 KiB payload limit, existing type numbers and request/response pairing remain
unchanged. New commands are control-channel only; errors retain the request ID
and use ERROR (255).

| Command | Type | Request | Success response |
| --- | --- | --- | --- |
| RESTART_SERVICE | 6 | `{"service_name":"fake_service"}` | `{"result":"OK"}` |
| GET_SERVICE_LIST | 7 | `{}` | `{"services":[...]}` |

RESTART_SERVICE acknowledges acceptance, rather than completed recovery. IPC
submits STOP through the existing runtime queue, sends the existing SERVICE_STOP
notification to identified service connections, and polls status without blocking
socket handling. START is queued only after the runtime reports STOPPED with no
positive PID, so the old process is reaped first. Pending duplicates are coalesced;
disconnecting the requester does not cancel accepted work. A later STOP cancels
pending restarts of the target and its configured transitive dependents. Manual
restart also works with policy `never`, and does not reserve an automatic retry or
reset its counter. Dependency startup and lifecycle changes remain in Service
Manager; restarting a prerequisite does not automatically revive its dependents.

GET_SERVICE_LIST uses a static definition snapshot and reads each service's
current runtime status in configuration order. A response example is:

```json
{"services":[{"service_name":"fake_service","state":"RUNNING","pid":1234,"health_status":"HEALTHY"}]}
```

The list has no `service_name` request field. Each item contains service_name,
state, pid (normally -1 when absent), and health_status:

- HEALTHY: RUNNING with a positive PID and a heartbeat within heartbeat_timeout.
- UNHEALTHY: FAILED, or RUNNING with an expired heartbeat/initial heartbeat deadline.
- UNKNOWN: other states, no positive PID, or no heartbeat yet within the initial
  grace period. A stale heartbeat from before the current start is ignored.

Health is a read-only projection, not a new health-check configuration or a state
transition. It uses the monotonic clock and the runtime's accepted heartbeat.
Rows are individually synchronized snapshots, not one atomic snapshot of all
services. A missing configured runtime entry or a list exceeding 64 KiB returns
ERROR code 1003; the list is never silently truncated or split into another format.
Existing invalid-service (1001) and invalid-payload/type (1002) errors remain.

Optional configuration fields are supported in both a single service object and
the existing `{"services":[...]}` root:

```json
{
  "service_name": "fake_service",
  "executable": "/usr/bin/fake_service",
  "environment": ["MODE=production", "EMPTY=", "TOKEN=a=b"],
  "working_directory": "/var/lib/fake_service",
  "shutdown_timeout": 5
}
```

- environment defaults to `[]`. Each item must be a string containing a nonempty
  key followed by `=` and a value, without NUL. Empty values and additional `=`
  characters in values are allowed. Declaration order and duplicate keys are
  preserved; the existing process backend inherits the parent environment and
  applies the last override for each key.
- working_directory defaults to `""` (inherit Runtime's current directory).
  Nonempty strings are passed to the process backend's chdir; relative paths are
  relative to Runtime's current directory. NUL and non-string values are rejected.
  Filesystem availability is checked during process launch, not configuration load.
- shutdown_timeout defaults to 2 seconds and requires an integer from 1 through
  3600. The Service Manager uses it for SIGTERM-to-SIGKILL escalation.

No pid_file, health_check, dynamic registration or unregistration is added.
