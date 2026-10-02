# Phase3 Codex Dependency


## Thread Dependency Graph


Thread1

Device State Manager


↓

Thread2

Event Aggregation


Thread3

IPC Extension


↓

Thread4

Testing



## Execution Order


## Step1


Must complete:


Thread1


Reason:


State Model is foundation.



## Step2


Can execute parallel:


Thread2

Thread3


Reason:


Both depend on Thread1 only.



## Step3


Execute:


Thread4


Reason:


Testing depends on final interfaces.



## Review


After all threads:


Architecture Review required.