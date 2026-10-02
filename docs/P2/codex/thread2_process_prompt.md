# Phase2 Thread2

## Role

Implement Linux Process Lifecycle Management.


Read:

docs/P2/README.md

docs/P2/process_lifecycle_design.md


## Goal

Implement process backend.


Features:

- process start
- process stop
- pid tracking
- process alive detection


## Input

Thread1 Service Manager.


Assume:

Service lifecycle model already exists.


## Allowed Modification

Allowed:

- process_controller
- monitor related code
- service integration


## Forbidden

Do not:

- redesign Service Manager
- implement Recovery policy
- implement dependency manager
- use systemd


## Technical Requirement

Use Linux native process management.

Target:

ARM64 Linux.


## Validation

Test:

- start fake service
- stop fake service
- process exit detection
- pid validation
