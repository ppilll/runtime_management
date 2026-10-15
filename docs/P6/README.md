# Phase 6 — MCU Link Supervision & Integration

状态：PHASE6 DESIGN FREEZE CANDIDATE；本轮仅交付架构与实施任务，不生成 C++，不启动实施 chat，不向仓库写入。
完成日期：2026-10-03（Asia/Shanghai）。以已连接 GitHub 固定提交阅读，证据见 [SOURCE_MANIFEST.md](SOURCE_MANIFEST.md)。

## Phase Definition

| 字段 | 内容 |
| --- | --- |
| Phase ID | P6 |
| Phase Name | MCU Link Supervision & Integration |
| Goal | 让 Runtime 分别观察通信进程健康和 MCU 链路健康，以最小组件事实参与设备聚合，保留现有通信恢复与进程恢复 owner |
| Why | UART 已成熟，但没有受管实例健康上报；现有启动等待失败会退出，链路掉线可能被错误升级为进程重启 |
| Input | P0–P5 完整阶段文档、当前 Runtime 核心与测试、VisionArm UART/工具、MCU portable/App 协议；静态产品配置 |
| Output | normalized health、authenticated HEALTH_REPORT、component aggregation/readiness、有限缓存与测试任务；MCU wire 不变 |
| Modified Modules | Runtime model/event/aggregation/IPC/config/接线；VisionArm owner projection、报告客户端和 managed-mode 生命周期接线；测试 |
| Interface Changes | 一个 component_health_changed 内部事实；UDS type11 bind/report；type9 可选组件查询；复用 ServiceHealthPolicy；UART 窄观测 seam |
| Implementation Tasks | T1 Runtime contract/model → T2 VisionArm adapter 与 T3 Runtime transport/config 可条件并行 → T4 review/test |
| Testing | [TEST_PLAN.md](TEST_PLAN.md)：22 原场景 + 16 补充门，unit、Linux IPC/PTY、RK3588 分开 |
| Acceptance Criteria | [ACCEPTANCE_CRITERIA.md](ACCEPTANCE_CRITERIA.md)：28 原验收条件全部映射 |
| Risk | 短暂状态漏报、旧连接伪身份、boot_id 碰撞、同进程故障耦合、历史 static guards、板端依赖与 RS485 缺证据 |
| Next Phase Dependency | 后续消费 committed component snapshot；不得假定 actuator safe、每次 reboot 均可检测、硬件 RS485 已通过或链路故障可重启进程 |

## 文档导航

- [REPOSITORY_ASSESSMENT.md](REPOSITORY_ASSESSMENT.md)：三仓库事实、历史与当前差异。
- [ARCHITECTURE.md](ARCHITECTURE.md)、[OWNERSHIP.md](OWNERSHIP.md)、[DECISIONS.md](DECISIONS.md)：路径、边界与 A/B。
- [MCU_HEALTH_MODEL.md](MCU_HEALTH_MODEL.md)、[SERVICE_INTEGRATION.md](SERVICE_INTEGRATION.md)：五状态、projection、reboot、managed 启动。
- [RUNTIME_EVENT_INTEGRATION.md](RUNTIME_EVENT_INTEGRATION.md)、[DEVICE_STATE_INTEGRATION.md](DEVICE_STATE_INTEGRATION.md)、[RECOVERY_BOUNDARY.md](RECOVERY_BOUNDARY.md)：single writer、全量聚合、恢复。
- [IPC_CONTRACT.md](IPC_CONTRACT.md)、[CONFIGURATION.md](CONFIGURATION.md)：最小新 wire、身份/lease、兼容。
- [CHANGE_LIST.md](CHANGE_LIST.md)：逐仓库/模块/接口/文件计划及旧检查迁移。
- [TEST_PLAN.md](TEST_PLAN.md)、[HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md)、[RISK_ANALYSIS.md](RISK_ANALYSIS.md)：待验证门。
- [AGENTS.md](AGENTS.md)：特殊规则；[codex_package/README.md](codex_package/README.md)：四个不同执行提示词和顺序。
- [DESIGN_FREEZE_CANDIDATE.md](DESIGN_FREEZE_CANDIDATE.md)、[PACKAGE_VALIDATION.md](PACKAGE_VALIDATION.md)：冻结范围与本包实际检查。

本包可复制为 runtime_management 的根 AGENTS.md 和 docs/P6 overlay。T2 需要将同版 docs/P6 复制至 VisionArm 的 docs/P6；它仅是任务合同副本，不复制协议或 Runtime 源码。两份设计通过本包版本与基线校验，冻结后不在并行任务中各自改合同。
主阅读稿包含 Final Output 1–24、26，以及 docs/P6/codex_package 的全部正文。附件编号未给 25；本包保留原编号。
