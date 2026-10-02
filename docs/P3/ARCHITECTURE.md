# Phase3 Architecture


## Overview


Current architecture:


Hardware

↓

Linux Kernel

↓

runtime_manager

↓

Service Layer


vision_service

control_service

ota_service



Phase3 adds:


runtime_manager

|

+----------------+

|

Device State Manager

|

+----------------+

|

Service Manager

Monitor

Heartbeat

IPC



## New Components


## Device State Manager


Responsibilities:


- Maintain device state
- Process state events
- Execute transitions
- Notify state changes


Not responsible:


- hardware operation
- service business


## Event Layer


Purpose:


Decouple:

Service Manager

Monitor

Heartbeat


from:

Device State Manager


## Health Layer


Collect:

- service health
- process health
- resource health
- heartbeat health


Generate:

Device State



## Design Principle


Device State is a system-level abstraction.

Service State is a component-level abstraction.


They must not be mixed.