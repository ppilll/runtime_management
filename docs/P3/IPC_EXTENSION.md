# Phase3 IPC Extension


## Existing IPC


Phase2:


START

STOP

QUERY_STATUS

HEARTBEAT



## New Interface


## GET_DEVICE_STATE


Purpose:

Return current device state.


Return:


- state
- timestamp
- reason



---


## GET_HEALTH


Purpose:

Return health summary.


Include:


- device state
- service summary
- health reason



---


## SUBSCRIBE_EVENT


Purpose:


Receive device state changes.


Events:


DEVICE_STATE_CHANGED



## Transport


Keep:


Unix Domain Socket


Do not replace IPC mechanism.

## Thread3 wire contract

The existing Unix stream sockets, 10-byte little-endian header and 64 KiB
UTF-8 JSON payload limit are unchanged. Types 1 through 7 and ERROR (255)
retain their Phase1/Phase2 meanings. New commands use the control socket only.
Success responses use the request's type and request_id; errors use type 255
and retain request_id. Empty query payloads must be the JSON object `{}`.

| Command | Type | Request |
| --- | --- | --- |
| GET_DEVICE_STATE | 8 | `{}` |
| GET_HEALTH | 9 | `{}` |
| SUBSCRIBE_EVENT | 10 | `{"event":"DEVICE_STATE_CHANGED"}` |

GET_DEVICE_STATE response example:

```json
{"state":"WARNING","timestamp":123456,"reason":"resource pressure"}
```

`timestamp` is the committed DeviceStateSnapshot timestamp, expressed as integer
milliseconds since the Linux monotonic clock epoch. It is neither Unix time nor
the time of the query. It is directly comparable within the same boot; clients
must not compare it across device reboots. Equal timestamps are allowed.

GET_HEALTH response example:

```json
{"device_state":{"state":"WARNING","timestamp":123456,"reason":"resource pressure"},"service_summary":{"services":[{"service_name":"control_service","state":"RUNNING","pid":1234,"health_status":"HEALTHY"}],"total":1,"healthy":1,"unhealthy":0,"unknown":0},"health":{"status":"DEGRADED","reason":"resource pressure"}}
```

Services appear in static configuration order. Service health follows the
existing GET_SERVICE_LIST projection: FAILED or an expired heartbeat is
UNHEALTHY; RUNNING with a positive PID and a recent accepted heartbeat is
HEALTHY; other states or an initial heartbeat still in its grace period are
UNKNOWN. Counts describe exactly the returned rows. Device health is a read-only
label: WARNING is DEGRADED, ERROR/OFFLINE are UNHEALTHY, and
BOOTING/READY/RECOVERING are UNKNOWN. The health reason is the device snapshot's
reason. Device and individual service snapshots are synchronized separately;
this response is not an atomic snapshot across all services.

For RUNNING, the overall health label now considers the returned service rows:
any UNHEALTHY row yields UNHEALTHY; otherwise any UNKNOWN row or an empty service
list yields UNKNOWN; only a nonempty all-HEALTHY list yields HEALTHY. This is a
conservative confirmation rule for all configured services, including inactive
optional services. RUNNING remains lifecycle readiness within heartbeat grace,
while GET_HEALTH reports confirmation. The query does not mutate DeviceState.
Fields, type numbers and the existing status labels remain unchanged.

SUBSCRIBE_EVENT response example:

```json
{"result":"OK","event":"DEVICE_STATE_CHANGED"}
```

Subscription starts when the IPC thread registers the connection under the
producer queue lock. The ACK precedes all notifications for that subscription.
It does not replay prior changes or send an initial state. Query the current
state separately when needed. Repeated subscriptions on the same connection
are idempotent. There is no unsubscribe command; closing the connection removes
the subscription. Service-channel peers and unsubscribed control peers never
receive DEVICE_STATE_CHANGED.

Notifications use the existing EVENT type (5), with request_id 0:

```json
{"event":"DEVICE_STATE_CHANGED","previous_state":"RUNNING","source":"monitor","state":"WARNING","timestamp":123456,"reason":"resource pressure"}
```

The runtime's single writer forwards every committed state change through the
existing state-change callback. IPC copies these notifications into a mutex
protected queue and sends them on the epoll thread, preserving producer order
even when multiple changes occur between socket polls. Invalid transitions
produce no notification. Existing SERVICE_STOP notifications are unchanged.
Subscribed clients must demultiplex EVENT frames from paired query responses.

The producer queue is limited to 1024 events and 262184 payload bytes; each
client's unsent output is limited to 262184 bytes (four maximum-size frames).
An event exceeding 64 KiB, invalid snapshot UTF-8, or producer queue overflow
disconnects subscribers affected by the missing event. Slow clients exceeding
their output limit are also disconnected. Clients should reconnect, subscribe
again and query the snapshot after disconnection. There is no replay guarantee
across disconnects. Socket polling adds up to approximately 200 ms under normal
load; subscribers do not block the runtime writer on socket I/O.

Invalid JSON, unexpected/duplicate fields, unsupported events, incorrect field
types and wrong-channel requests return 1002. A missing device query provider,
unavailable snapshot, or oversized query response returns 1003 without
truncation. Existing invalid-service code 1001 is unchanged. Older IpcManager
constructors remain supported; their new commands return 1003 until a device
query callback is supplied. Frames exceeding the existing payload limit still
cause disconnection under the Phase2 framing rules.
