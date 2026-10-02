# Phase2 Thread4

## Role

Extend IPC and Configuration integration.


Read:

docs/P2/ipc_config_extension.md


## Goal


Add:


IPC:

- RESTART_SERVICE
- GET_SERVICE_LIST


Config:


Add:

- environment
- working_directory
- shutdown_timeout


## Input

Existing Phase1 IPC Manager.


## Allowed Modification

Allowed:

- IPC Manager
- Config Manager


## Forbidden

Do not:

- add dynamic service registration
- redesign IPC architecture
- change protocol style


## Validation

Test:

- query service list
- restart command
- config loading
