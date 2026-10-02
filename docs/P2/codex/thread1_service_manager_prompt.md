# Phase2 Thread1

## Role

You are implementing Phase2 Service Manager extension.

You must first read:

docs/P2/README.md

docs/P2/architecture.md

docs/P2/service_manager_design.md


Do not start implementation before understanding these documents.


## Goal

Implement Service Manager core model.


Responsibilities:

- Service definition model
- Lifecycle state machine
- Service registry
- State transition management


## Input

Existing Phase1 Runtime Framework.


## Output

A Service Manager foundation that can later connect with:

- Process Controller
- Recovery Manager
- IPC Manager


## Allowed Modification

Allowed:

- service_manager module
- related headers/interfaces
- unit tests


## Forbidden

Do not:

- modify Phase0 architecture
- implement process launching
- implement restart logic
- implement dependency resolution
- modify vision/control/ota services


## Design Rules

Follow:

docs/P2/service_manager_design.md


Do not invent new architecture.


## Validation

Provide:

- changed files
- design impact
- tests executed
- remaining dependencies
