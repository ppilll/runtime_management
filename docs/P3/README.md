# Phase3 Device State Management

## Project

RK3588 Embedded Device Runtime Management Framework


## Phase

Phase3

Device State Management Layer


## Objective

Phase3 extends runtime_manager from service lifecycle supervision
to device-level health supervision.

The system must answer:

"Is the device currently healthy?"


## Background

Phase0:

Architecture Freeze


Phase1:

Runtime Core Framework


Phase2:

Service Lifecycle Management


Phase3:

Device State Management


## Scope


Included:

- Device State Model
- State Machine
- Service State Aggregation
- Internal Event Model
- Device Health Query
- Monitor Integration


## Non Goal


Forbidden:

- Camera implementation
- V4L2 management
- RKNN inference
- MCU protocol
- Business logic
- Cloud monitoring
- MQTT
- Web Dashboard
- Database history analysis
- AI prediction


## Architecture Principle


runtime_manager only manages:

- lifecycle
- health
- recovery coordination
- IPC


runtime_manager does not implement:

- vision pipeline
- control algorithm
- hardware business


## Development Rule


Before implementation:

Read:

/docs/P3/AGENTS.md

and related architecture documents.


All implementation decisions must follow documents under:

/docs/P3/