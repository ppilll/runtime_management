# Phase3 Thread2 Codex Task

## Task Name

Event System and Service Aggregation


---

# Dependency


Thread1 must be completed before starting.


Required existing:


Device State Manager


State Model



---

# Mandatory Reading


Read:


/docs/P3/README.md

/docs/P3/AGENTS.md

/docs/P3/EVENT_MODEL.md

/docs/P3/AGGREGATION_RULE.md

/docs/P3/DEVICE_STATE_MODEL.md



---

# Objective


Implement internal event handling and service state aggregation.



---

# Scope


Implement:


1.

Internal runtime event mechanism


2.

Service health aggregation


3.

Connection between Service Manager and Device State Manager



---

# Event Support


Must support:


SERVICE_STARTED


SERVICE_FAILED


SERVICE_STOPPED


HEARTBEAT_TIMEOUT


RESOURCE_WARNING


RECOVERY_SUCCESS


RECOVERY_FAILED



---

# Aggregation Rules


Follow:


/docs/P3/AGGREGATION_RULE.md



Required behavior:


control_service failure:


Device ERROR



vision_service failure:


Device WARNING



ota_service failure:


Device WARNING



---

# Modification Boundary


Allowed:


event subsystem


aggregation module


runtime_manager adapters



Forbidden:


service business implementation


IPC protocol


hardware monitoring



---

# Design Requirements


Event layer should:


- reduce module coupling
- provide deterministic ordering
- support future recovery module



Do not introduce:


MQTT


external message broker


database



---

# Testing Requirement


Test:


## Case1


vision_service failed


Expected:


RUNNING

↓

WARNING



## Case2


control_service failed


Expected:


RUNNING

↓

ERROR



## Case3


multiple service failures


Verify priority.



## Case4


Recovery event


Verify state recalculation.



---

# Deliverables


Provide:


- changed files
- event design summary
- aggregation test result
- possible race conditions