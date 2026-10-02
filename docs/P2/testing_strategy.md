# Phase2 Testing Strategy


## Objective


Verify Service Lifecycle Management.


---

# Unit Test


Coverage:


## State Machine


Test:


- valid transition

- invalid transition



## Restart Policy


Test:


- retry count

- backoff calculation



## Dependency


Test:


- graph ordering



---


# Integration Test


## Test 1

Service Start


Verify:


- process created

- state RUNNING



---

## Test 2

Service Stop


Verify:


- SIGTERM

- timeout handling



---

## Test 3

Crash Recovery


Action:


kill service process


Expected:


automatic restart



---

## Test 4

Heartbeat Timeout


Action:


stop heartbeat


Expected:


recovery triggered



---

## Test 5

Restart Limit


Action:


continuous crash


Expected:


FAILED state



---

## Test 6

Dependency Order


Verify:


startup:


dependency first


shutdown:


dependent first

