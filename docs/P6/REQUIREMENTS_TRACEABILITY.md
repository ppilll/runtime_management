# Original Task Traceability

Required Design 1–27 与 Final Output 1–24/26 是两套编号，分别完整保留。原 Test1–22 对应 P6-01–22，原 28 Acceptance 对应 ACCEPTANCE_CRITERIA 表；新增 P6-23–38 不替换原场景。

| Required Design | 主题 | 完整设计 |
| --- | --- | --- |
| 1 | Communication Ownership | [OWNERSHIP.md](OWNERSHIP.md) |
| 2 | Do Not Duplicate UART Protocol | [OWNERSHIP.md](OWNERSHIP.md) / [AGENTS.md](AGENTS.md) |
| 3 | Process Health vs Link Health | [MCU_HEALTH_MODEL.md](MCU_HEALTH_MODEL.md) |
| 4 | Normalized Contract | [MCU_HEALTH_MODEL.md](MCU_HEALTH_MODEL.md) / [IPC_CONTRACT.md](IPC_CONTRACT.md) |
| 5 | MCU Reboot Detection | [MCU_HEALTH_MODEL.md](MCU_HEALTH_MODEL.md) |
| 6 | Link Loss Semantics | [MCU_HEALTH_MODEL.md](MCU_HEALTH_MODEL.md) / [RECOVERY_BOUNDARY.md](RECOVERY_BOUNDARY.md) |
| 7 | Local vs Runtime Recovery | [RECOVERY_BOUNDARY.md](RECOVERY_BOUNDARY.md) |
| 8 | Runtime Health Event | [RUNTIME_EVENT_INTEGRATION.md](RUNTIME_EVENT_INTEGRATION.md) / [DECISIONS.md](DECISIONS.md) |
| 9 | ServiceAggregation | [DEVICE_STATE_INTEGRATION.md](DEVICE_STATE_INTEGRATION.md) |
| 10 | Criticality / Required | [CONFIGURATION.md](CONFIGURATION.md) / [DEVICE_STATE_INTEGRATION.md](DEVICE_STATE_INTEGRATION.md) |
| 11 | Startup Readiness | [SERVICE_INTEGRATION.md](SERVICE_INTEGRATION.md) / [DEVICE_STATE_INTEGRATION.md](DEVICE_STATE_INTEGRATION.md) |
| 12 | IPC Contract | [IPC_CONTRACT.md](IPC_CONTRACT.md) |
| 13 | Producer Identity | [IPC_CONTRACT.md](IPC_CONTRACT.md) / [RUNTIME_EVENT_INTEGRATION.md](RUNTIME_EVENT_INTEGRATION.md) |
| 14 | Disconnect A–H | [RECOVERY_BOUNDARY.md](RECOVERY_BOUNDARY.md) |
| 15 | UART / RS485 Config | [CONFIGURATION.md](CONFIGURATION.md) / [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md) |
| 16 | VisionArm Reuse | [OWNERSHIP.md](OWNERSHIP.md) / [CHANGE_LIST.md](CHANGE_LIST.md) |
| 17 | Metrics Boundary | [MCU_HEALTH_MODEL.md](MCU_HEALTH_MODEL.md) |
| 18 | Device State Mapping | [DEVICE_STATE_INTEGRATION.md](DEVICE_STATE_INTEGRATION.md) |
| 19 | OFFLINE Semantics | [DEVICE_STATE_INTEGRATION.md](DEVICE_STATE_INTEGRATION.md) |
| 20 | Host Linux Strategy | [TEST_PLAN.md](TEST_PLAN.md) |
| 21 | Fake MCU | [TEST_PLAN.md](TEST_PLAN.md) |
| 22 | RK3588 Hardware | [HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md) |
| 23 | Fault Injection | [TEST_PLAN.md](TEST_PLAN.md) |
| 24 | Concurrency / Single Writer | [ARCHITECTURE.md](ARCHITECTURE.md) / [RUNTIME_EVENT_INTEGRATION.md](RUNTIME_EVENT_INTEGRATION.md) |
| 25 | Shutdown | [SERVICE_INTEGRATION.md](SERVICE_INTEGRATION.md) |
| 26 | Configuration | [CONFIGURATION.md](CONFIGURATION.md) |
| 27 | Scope Exclusions | [AGENTS.md](AGENTS.md) / [DESIGN_FREEZE_CANDIDATE.md](DESIGN_FREEZE_CANDIDATE.md) |

Final Output 全文见交付根目录 Phase6_完整设计与执行包.md；24 的附录 A 含全部 docs/P6 正文，26 的附录 B 含九个 codex_package 文件与四个独立提示词。未补造附件没有的 Final Output 25。
