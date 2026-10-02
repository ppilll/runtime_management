# Device State Model


## Purpose


Define device-level runtime status.


## States


## BOOTING


Meaning:

Runtime initialization stage.


Entry:

runtime_manager starts.


Exit:

initialization complete.


---


## READY


Meaning:

Runtime initialization complete; waiting for required service readiness.

Workload readiness is represented by RUNNING.


Condition:


- runtime event loop initialized
- required services may still be CREATED or awaiting startup


---


## RUNNING


Meaning:

Device operating normally.


Condition:


- required services running and no accepted critical faults
- no accepted heartbeat timeout (initial heartbeat grace is allowed)
- resource normal


---


## WARNING


Meaning:

Device still works but has degradation.


Examples:


- optional service failure
- resource pressure


---


## ERROR


Meaning:

Critical function unavailable.


Examples:


- critical service failure
- heartbeat timeout


---


## RECOVERING


Meaning:

Recovery procedure executing.


---


## OFFLINE


Meaning:

Device unavailable.


Examples:


- recovery failed
- unrecoverable error



## State Priority


Highest:


OFFLINE

ERROR

WARNING

RUNNING

READY

BOOTING
