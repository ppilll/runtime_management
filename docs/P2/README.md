# Runtime Management Framework

# Phase 2 Development Specification


## Purpose

Phase 2 extends Phase 1 Runtime Core Framework.

The objective is implementing:

Service Lifecycle Management.


Phase 2 transforms Runtime from:

basic service communication framework

into:

device runtime supervisor.


---

# Scope


Included:


- Service lifecycle management

- Linux process lifecycle management

- Service state synchronization

- Health monitoring

- Recovery strategy

- Static dependency management

- IPC extension

- Configuration extension

- Integration testing



---

# Not Included


Phase 2 MUST NOT implement:


## Vision Business

Forbidden:

- Camera pipeline

- V4L2 logic

- DMA-BUF management

- RGA processing

- RKNN inference

- MPP processing


## Control Business

Forbidden:

- UART protocol

- RS485 protocol

- MCU control


## OTA Business

Forbidden:

- Firmware update logic

- Package management



---

# Architecture Constraint


Phase 2 must preserve:


Phase 0 architecture:


Runtime Manager

Service Manager

Monitor

Recovery Manager

IPC Manager

Config Manager

Logger



Phase 1 interfaces must not be redesigned.


---

# Development Principle


Prefer:


- simple design

- explicit interfaces

- Linux native mechanisms

- embedded friendly implementation



Avoid:


- large frameworks

- distributed architecture

- unnecessary abstraction

- service orchestration platform design


---

# Target Platform


Linux ARM64

RK3588 Device

