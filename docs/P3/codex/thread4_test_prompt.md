# Phase3 Thread4 Codex Task

## Task Name

Phase3 Validation and Review


---

# Execution


This thread runs after Thread1 Thread2 Thread3.



---

# Mandatory Reading


Read all:


/docs/P3/



---

# Objective


Validate Phase3 Device State Management.



---

# Test Scope


## Test1 Normal Startup


Verify:


BOOTING

↓

READY

↓

RUNNING



---

## Test2 Service Failure


Scenario:


critical service crash



Expected:


RUNNING

↓

ERROR



---

## Test3 Recovery Flow


Verify:


ERROR

↓

RECOVERING

↓

RUNNING



---

## Test4 Aggregation


Test:


multiple services


Examples:


vision failure


control failure


ota failure



---

## Test5 Heartbeat


Scenario:


service heartbeat timeout



Expected:


Device ERROR



---

# Review Items


Check:


State transition completeness


Event consistency


IPC compatibility


Thread safety


Phase2 regression



---

# Deliverables


Provide:


Test report


Failed cases


Architecture concerns


Recommended fixes