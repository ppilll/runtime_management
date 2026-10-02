# Phase2 Thread3

## Role

Implement Recovery and Dependency Management.


Read:

docs/P2/recovery_dependency_design.md


## Goal

Implement:

1. crash recovery

2. heartbeat timeout recovery

3. restart policy

4. dependency ordering


## Input

Existing:

Service Manager

Process Controller


## Required Behavior


Crash:

FAILED

        |

RECOVERING

        |

RESTART


Restart:

maximum 5 times


Backoff:

2s-60s


## Dependency

Support:

static dependency graph.


Required:

startup ordering

shutdown reverse ordering


## Forbidden

Do not:

- create Kubernetes-like scheduler
- introduce external framework
- modify business services


## Validation

Test:

- crash restart
- heartbeat timeout
- restart limit
- dependency order
