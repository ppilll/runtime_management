# Phase4 Recovery Strategy & Coordination
日期：2026-10-02（Asia/Shanghai）  
状态：设计冻结候选；T1–T4 当前实现/静态审查状态见 [validation_review.md](validation_review.md)。  
事实基线：[ppilll/runtime_management@2ccbc733a1386c46917e6aee2fc7e0680e4321e9](https://github.com/ppilll/runtime_management/tree/2ccbc733a1386c46917e6aee2fc7e0680e4321e9)。

## 设计交付范围（历史时点）
本节与下方原设计验证用语描述设计包生成时点；T1–T4 后续源码与审查已经产出，当前运行验证仍为空。
最终测试矩阵、20 项验收、静态命令结果及未验证边界以 [validation_review.md](validation_review.md)
和 [review_fixes.md](review_fixes.md) 为准，不将原设计记录当作当前实现状态。

建立确定性恢复协调层：Failure Fact → Request → Execution → Result → 整体健康聚合。保留 RK3588 Linux ARM64、C++17、CMake、Multi Process + Internal Thread、Unix Domain Socket 及 P1–P3 生命周期/设备状态架构。当前包没有创建或修改任何 C++ 源码，没有提交或写入远端仓库。

## 阅读顺序
1. [REPOSITORY_ASSESSMENT.md](REPOSITORY_ASSESSMENT.md)：事实、历史修复和证据。
2. [ARCHITECTURE.md](ARCHITECTURE.md) 与 [DECISIONS.md](DECISIONS.md)：职责和方案取舍。
3. [RECOVERY_MODEL.md](RECOVERY_MODEL.md)、[RECOVERY_GENERATION.md](RECOVERY_GENERATION.md)、[RECOVERY_POLICY.md](RECOVERY_POLICY.md)：精确契约。
4. [SERVICE_INTEGRATION.md](SERVICE_INTEGRATION.md)、[RECOVERY_EVENT_FLOW.md](RECOVERY_EVENT_FLOW.md)、[DEVICE_STATE_INTEGRATION.md](DEVICE_STATE_INTEGRATION.md)：接线。
5. [CHANGE_LIST.md](CHANGE_LIST.md)、[TEST_PLAN.md](TEST_PLAN.md)、[RISK_ANALYSIS.md](RISK_ANALYSIS.md)：实施边界与验收。
6. [AGENTS.md](AGENTS.md)、[codex_package/README.md](codex_package/README.md)：后续开发入口。
7. [DESIGN_FREEZE_CANDIDATE.md](DESIGN_FREEZE_CANDIDATE.md)：冻结判断。
8. [validation_review.md](validation_review.md)、[review_fixes.md](review_fixes.md)：T4 最终静态审查与有限修复；本次不执行编译/运行。

## 核心结论
RecoveryManager 是唯一自动重试/退避/恢复期限/终结结果所有者；ServiceManager 保留生命周期执行、PID、生命周期 generation、停止升级、静态依赖图。RuntimeManager 只接线与调度。DeviceStateManager 只提交显式设备转换。IPC 不再自行轮询 stop/start。

默认不增加线程，不增加 IPC 命令，不增加设备状态和 RuntimeEventType。不自动重启 Runtime 或 reboot。依赖恢复保留 P2：停止受影响闭包，恢复故障节点；被停止的依赖方由显式 START 恢复。

## 验证用语
- Implemented：本包 Markdown 文档及任务提示词已产出；Phase4 C++ 尚未实现。
- Statically Verified：本次仅核对仓库源码路径/证据、包内引用与需求覆盖；并非运行 C++ 静态分析器。
- Runtime Verified：本次无；没有执行 Linux 构建、CTest、进程或 socket 测试。
- Not Verified：C++ 类型/链接、实际恢复链、Linux 并发/时序及 RK3588 行为。
P3 的静态检查记录是历史证据，不当作本次或运行通过证据。

## 实施前提
将本包的 docs/P4 整体放入目标仓库，核对当前 HEAD 相对基线的相关差异，按 codex_package 顺序执行。docs/P4/AGENTS.md 的自动目录作用域只涵盖 docs/P4；每个开发提示词显式要求先阅读它，将规则作为当前任务约束，不冒称它自动约束 include/src。
