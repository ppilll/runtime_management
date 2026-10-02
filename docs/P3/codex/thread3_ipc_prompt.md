# Phase3 Thread3 Codex Task

## Task Name

Device State IPC Extension


---

# Dependency


Thread1 Device State Model required.


Can run parallel with Thread2.



---

# Mandatory Reading


Read:


/docs/P3/README.md

/docs/P3/AGENTS.md

/docs/P3/IPC_EXTENSION.md

/docs/P3/DEVICE_STATE_MODEL.md



---

# Objective


Extend existing Unix Domain Socket IPC.


Allow external applications to query device health.



---

# Scope


Implement:


GET_DEVICE_STATE


GET_HEALTH


SUBSCRIBE_EVENT



---

# Existing Interface


Must preserve:


START


STOP


QUERY_STATUS


HEARTBEAT



---

# Interface Requirement


GET_DEVICE_STATE


Return:


- state
- timestamp
- reason



---

GET_HEALTH


Return:


- device state
- service summary
- health information



---

SUBSCRIBE_EVENT


Support:


DEVICE_STATE_CHANGED



---

# Modification Boundary


Allowed:


ipc_manager


IPC protocol definition


IPC test



Forbidden:


changing service lifecycle


changing state machine logic



---

# Design Requirement


Maintain:


Unix Domain Socket


Do not introduce:


HTTP


TCP


MQTT



---

# Testing Requirement


Verify:


1.

Query current state


2.

Invalid request handling


3.

State change notification


4.

Compatibility with Phase2 IPC



---

# Deliverables


Provide:


- modified files
- protocol changes
- IPC test result