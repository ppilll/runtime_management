# Monitor Integration


## Input


Monitor provides:


CPU

Memory

Process

Heartbeat



## CPU


High usage:


WARNING



## Memory


Threshold:


80%

WARNING


95%

ERROR



## Process


Critical process crash:


ERROR



## Heartbeat


Timeout:


ERROR



## Rule


Monitor reports facts.

State Manager decides state.


Monitor must not directly modify Device State.

## Review remediation interface

`ResourceThresholds` is immutable configuration passed when constructing Monitor
or as RuntimeManager's fourth argument. Defaults are CPU warning 80%, memory
warning 80% and memory critical 95%; equality triggers the threshold. Values must
be finite, positive and at most 100%, with memory warning below critical.

`Monitor::report_resources(cpu_percent, memory_percent, at)` accepts externally
measured percentages. `RuntimeManager::reportResourceUsage(...)` forwards to this
interface. It publishes queued CPU and memory facts under `cpu_monitor` and
`memory_monitor`, with source, reason and producer timestamp. Both inputs must be
finite and within 0..100; validation occurs before publication. Below warning
publishes a clearing fact, memory between warning and critical publishes warning,
and memory at/above critical publishes critical severity. Repeated facts are
idempotent at the aggregate state level.

This interface does not collect hardware/procfs measurements. The executable
requires a deployment's measurement producer to submit resource usage. Heartbeat
watching remains the existing Monitor implementation. Resource facts are handled
by the runtime event loop and the state owner; the reporting caller never writes
DeviceState directly. Callers must serialize one resource producer's submissions
for meaningful measurement order, just as other FIFO producers.
