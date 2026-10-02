# Phase 2 Architecture Design


## 1. Objective


Implement Runtime Service Supervisor capability.


Architecture:



runtime_manager

        |

Service Manager

        |

+-----------------------+

|                       |

Process Controller   State Manager

|                       |

Recovery Manager   Dependency Manager



2. Responsibility Boundary
Runtime Manager
Responsible:
- system coordination
- service lifecycle orchestration
- IPC entry
Not responsible:
- business execution
Service Manager
Responsible:
- service registration from configuration
- lifecycle state machine
- start/stop control
- status tracking
- restart request routing
Not responsible:
- process implementation
- business health judgement
Monitor
Responsible:
- heartbeat monitoring
- process status monitoring
Recovery Manager
Responsible:
- restart decision
- retry count
- backoff calculation
Not responsible:
- repairing business state
3. Phase2 Lifecycle Model
States:
CREATED

STARTING

RUNNING

STOPPING

STOPPED

FAILED

RECOVERING


Allowed transitions:
CREATED

 -> STARTING


STARTING

 -> RUNNING

 -> FAILED


RUNNING

 -> STOPPING

 -> FAILED


FAILED

 -> RECOVERING


RECOVERING

 -> STARTING


STOPPING

 -> STOPPED


4. Design Rule
All Phase2 modules must extend existing framework.
Do not replace Phase1 architecture.

---

# docs/P2/service_manager_design.md

```md
# Service Manager Design


## Objective


Implement service lifecycle control.


---

# 1. Service Model


Service definition contains:



service_name
executable
arguments
environment
working_directory
autostart
dependency
startup_timeout
shutdown_timeout
heartbeat_timeout
restart_policy


---

# 2. Lifecycle Controller


Responsibilities:


- state transition validation

- lifecycle event generation

- state synchronization



Example:



START_REQUEST
  |

CREATED
  |

STARTING
  |

RUNNING


---

# 3. Service Registry


Maintain:


- service name

- configuration

- current state

- runtime information


Runtime information:


- pid

- start time

- restart count


---

# 4. Service Manager API


Required:



startService()
stopService()
restartService()
queryServiceStatus()
listServices()


---

# 5. Design Limit


Do not implement:


- dynamic service discovery

- service migration

- cluster management

- container scheduling