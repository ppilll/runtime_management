# State Transition Design

The table below is the original no-target contract. Aggregate events with an
explicit `health_target` additionally use the 15 checked triples documented in
[AGGREGATION_RULE.md](AGGREGATION_RULE.md#explicit-aggregate-transition-extension).
The combined target-aware contract has 24 triples. No OFFLINE exit is permitted.


## Normal Flow


BOOTING

↓

READY

↓

RUNNING



## Transition Table


|From|Event|To|
|-|-|-|
|BOOTING|runtime initialized|READY|
|READY|required services ready|RUNNING|
|RUNNING|warning event|WARNING|
|RUNNING|critical failure|ERROR|
|WARNING|issue recovered|RUNNING|
|WARNING|failure escalates|ERROR|
|ERROR|start recovery|RECOVERING|
|RECOVERING|success|RUNNING|
|RECOVERING|failed|OFFLINE|



## Transition Rules


State change must have:

- event source
- reason
- timestamp


No implicit transition.


Example:


Wrong:

service crash automatically changes state


Correct:

SERVICE_FAILED event

↓

State Manager evaluates

↓

Device State changes
