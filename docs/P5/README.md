# Phase 5 — Resource Monitoring & Health Facts

状态：PHASE5 DESIGN FREEZE CANDIDATE。设计文档已生成；P5 C++ 未实现，未运行 C++ 测试。
事实基线：ppilll/runtime_management，master@4843061fce2a329832105d34aaea380a35560c0e。
基线核查：2026-10-02；文档完成：2026-10-03，Asia/Shanghai。读取来源为已连接 GitHub API，未使用网页仓库页面。

## Phase Definition

| 项目 | 冻结内容 |
| --- | --- |
| Phase ID / Name | P5 / Resource Monitoring & Health Facts |
| Goal | 从 Linux procfs 持续采集系统 CPU、Memory 和受管直接子进程资源；经 Monitor 形成可激活、降级、清除的设备资源健康事实 |
| Why | 现有 report_resources 只接受外部百分比；缺少真实采集、无效样本表达、迟滞和受管进程资源快照 |
| Input | 当前 P0–P4 文档、实现与 tests；/proc/stat、/proc/meminfo、/proc/PID/stat；静态 JSON；SM 只读实例身份 |
| Output | typed current snapshot、CPU WARNING、Memory WARNING/CRITICAL、同 source clear；既有 FIFO→聚合→设备状态链 |
| Modified Modules | 新增轻量 resource collector/model；Monitor、RuntimeManager、ConfigManager、main 启动接线、tests/CMake；SM 最小只读身份快照接口 |
| Interface Changes | typed collection/observation/query、完整 RuntimeConfig loader、native Runtime 构造入口、批量身份快照；保留旧资源上报方法与旧构造入口 |
| Implementation Tasks | T1 collector/model → T2 process/config → T3 policy/runtime → T4 review/validation/limited fix |
| Testing | [TEST_PLAN.md](TEST_PLAN.md)，单位、集成、静态、Host Linux、RK3588 分开 |
| Acceptance Criteria | [ACCEPTANCE_CRITERIA.md](ACCEPTANCE_CRITERIA.md)，24 项原需求及新增兼容/身份/失败门 |
| Risk | 共享 Monitor worker 的 I/O 延迟、既有 SM 同步启动锁、进程身份读取竞态、旧测试迁移、资源未验证 |
| Next Phase Dependency | 下一阶段只能消费已定义快照/资源事实；不得假定已完成 thermal/disk、资源驱动恢复或新 IPC |

## 设计导航

| 交付内容 | 文档 |
| --- | --- |
| Current Repository Assessment / Existing Monitor Gap | [REPOSITORY_ASSESSMENT.md](REPOSITORY_ASSESSMENT.md) |
| Collector / ownership / architecture | [ARCHITECTURE.md](ARCHITECTURE.md) |
| 最小 snapshot / typed interface | [RESOURCE_MODEL.md](RESOURCE_MODEL.md) |
| CPU / Memory / parser / test seam | [PROCFS_COLLECTION.md](PROCFS_COLLECTION.md) |
| Process / PID / generation / heartbeat | [PROCESS_MONITORING.md](PROCESS_MONITORING.md) |
| Sampling / shutdown / failure / performance / persistence | [SAMPLING_AND_FAILURE.md](SAMPLING_AND_FAILURE.md) |
| Threshold / hysteresis / CPU & Memory severity | [THRESHOLD_POLICY.md](THRESHOLD_POLICY.md) |
| Event / Device State / Recovery | [DEVICE_STATE_INTEGRATION.md](DEVICE_STATE_INTEGRATION.md) |
| Config | [CONFIGURATION.md](CONFIGURATION.md) |
| IPC / Temperature / Disk | [SCOPE_AND_IPC.md](SCOPE_AND_IPC.md) |
| A/B 决策及理由 | [DECISIONS.md](DECISIONS.md) |
| Module / Interface / file change list | [CHANGE_LIST.md](CHANGE_LIST.md) |
| Testing / Risk / Acceptance | [TEST_PLAN.md](TEST_PLAN.md)、[RISK_ANALYSIS.md](RISK_ANALYSIS.md)、[ACCEPTANCE_CRITERIA.md](ACCEPTANCE_CRITERIA.md) |
| 仓库源证据 | [SOURCE_MANIFEST.md](SOURCE_MANIFEST.md) |
| Freeze / Blocking / Deferred | [DESIGN_FREEZE_CANDIDATE.md](DESIGN_FREEZE_CANDIDATE.md) |
| 执行顺序 / 每线程任务 / 短提示词 | [codex_package/README.md](codex_package/README.md) |
| Agent 特殊规则 | [AGENTS.md](AGENTS.md) |
| 本设计包实际校验 | [PACKAGE_VALIDATION.md](PACKAGE_VALIDATION.md) |

## 范围

核心：system aggregate CPU、MemAvailable-based memory、受管直接子进程 CPU/RSS/观测状态、2s 静态采样、迟滞、可清除资源事实、memory-only 快照、测试 seam。

延期：Temperature、Disk、新 GET_RESOURCE_USAGE、process thresholds、资源触发恢复。
禁止：GPU/NPU/RKNN/AI performance、云/MQTT、Prometheus server/SDK、DB/历史趋势、Web/HTTP/gRPC、动态配置、复杂规则引擎、每服务独立线程、业务代码。

本包是可复制的文档 overlay；不含新 C++，不自动启动后续实现线程。仓库当前未有 docs/P5、根 AGENTS.md 或 docs/P4/codex_package；后者在 P4 历史报告被引用但不在本次树中，不能把它当已读取文件。
