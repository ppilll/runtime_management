# Phase3 Agent Rules


## Architecture Boundary


Runtime Manager is a supervisor.

Do not implement business capability.


Forbidden:

- Camera logic
- AI inference
- MCU protocol
- Hardware driver


## Modification Boundary


Allowed:

runtime_manager


Forbidden:

vision_service business

control_service business

ota_service business


## Dependency Rule


Do not introduce:

- database
- MQTT
- cloud SDK
- web server
- AI framework


## IPC Rule


Maintain:

Unix Domain Socket


Do not replace Phase2 IPC architecture.


## Design Rule


Prefer:

- deterministic state machine
- explicit transition
- static configuration


Avoid:

- dynamic rule engine
- complex framework


## Testing Rule


Every new state transition requires test.

Every IPC interface requires test.


## Build Rule


Use existing Phase1/Phase2 build system.

Do not introduce new build framework.