# Thread4 有限修复与回归记录

日期：2026-10-02（Asia/Shanghai）。冻结基线 `2ccbc733a1386c46917e6aee2fc7e0680e4321e9`。
用户授权执行 T4 并明确只做静态检测；以下“失败/证据”严格区分静态结构失败、人工路径缺陷
和未运行的 C++ 回归，不伪造运行日志。最终验收见 [validation_review.md](validation_review.md)。

## F01：P3 静态检查仍要求已移除的旧 owner

实际执行 `python -B tests/phase3_validation_static_check.py`，exit 1：

```text
FAIL: missing runtime adapter: service_cause_->type == EventType::health_missed
FAIL: Phase3 aggregation; Thread4: lifecycle remediation missing: service.config.restart_policy != RestartPolicy::never
```

原检查绑定了 P3 Runtime-local cause pointer、SM policy 和 SM maximum_restarts。冻结 P4 要求
用 captured callback、RM 单 owner 替代，因此检查器必须迁移真实契约而不能保留旧 producer。

最小修改：phase3_aggregation_static_check 检查 lifecycle_work_ 与 SM/RM causal drain；
phase3_validation_static_check 继续检查 generation/grace/health/资源/旧测试，但将 budget/terminal
断言移到 RM reservation cap、finalization captured reply、canonical takeResults 适配。
未删整段检查，未改 phase2/state/IPC checker，未改 63/441 状态矩阵。

验证：最终 P3/P4 静态入口 exit 0。C++ 测试没有运行。

## F02：IPC mock fixture 仍断言 pending stop/query/start

人工证据：原 test_ipc_extension_boundaries 预期两次 type6 只 post 一个 STOP，query/PID 清除后
IPC post START。最终 src/ipc/ipc_manager.cpp 已移除 pending_restarts/poll；type6 每次 post
restart_request，由 Runtime/RM 合并。这是测试的旧契约冲突，不是已观察到的 socket 测试失败。

最小修改：保留请求 pairing、escaping/健康、状态/PID 查询、断连/reconnect、后续 server turn
检查，改断言为逐请求 restart_request 与后续 STOP FIFO、无 query-triggered START/cancel。
真正的 oldPID/reap/never manual restart、ACK 与 SERVICE_STOP 的原 main 场景保留；RM duplicate
coalesce 由 RM unit 断言覆盖，避免在 transport fixture 伪装生命周期 owner。

验证：P3/P4 的结构、注册和 main 接线检查 exit 0；真实 socket 场景 Runtime Verified 为空。

## F03：OFFLINE 取消其他 active 后 SM 可能永久显示 RECOVERING

人工路径证据（修复前）：

1. 两个服务均启动失败并进入 RM BACKOFF，SM FAILED→RECOVERING。
2. HIGH 服务短 deadline 到期；Runtime 提交 canonical TIMEOUT，设备 OFFLINE。
3. Runtime 调 RM stopAutomatic；RM cancel 关闭另一个 active，并调用 SM releaseRecovery。
4. releaseRecovery 只 reset recovery context；SM state 仍 RECOVERING。SM.tick 不再有 retry，
   RM active/deadline 已消失，设备 OFFLINE 又禁止 auto，所以状态没有任何内部路径继续推进。

问题符合 SERVICE_INTEGRATION 中 cancellation 应通过正常 STOPPING/STOPPED 的冻结契约；
只撤销 binding 不足以完成生命周期。不能保留无限显示 RECOVERING 作为“已取消”。

最小 production 修复：仅 src/runtime/runtime_manager.cpp 的 OFFLINE cancellation receipt
循环追加 services_.stopService(result.service_name, Clock::now())。沿用原 stop closure，重复 stop
幂等；已有 termination_requested 时不重开/缩短 grace；未 reap PID 保留；回调只追加 writer work。
RM 仍唯一决定 cancel/预算/terminal，SM 仍唯一写生命周期，DSM 的 OFFLINE 不被 stop 清除。
不会 stop 无 active 的普通服务；以后显式 START/manual 的原权限不改变，不 reset 预算。

新增回归源码：RC test_offline_cancels_other_task_and_leaves_no_recovering_service，断言 optional
pending 在 OFFLINE 后 STOPPED/PID−1/count1/gen3，显式新 START 可执行但不 auto 或解锁设备，
只出现一个 OFFLINE。P4 checker 另检查该实际 cancellation branch 含 lifecycle stop。

验证：最终 P3/P4 静态入口 exit 0；diff 空白检查 exit 0。没有运行该 C++ 回归，因此报告为
Implemented + Statically Verified；Runtime Verified 为空。不得把此人工执行轨迹当实际运行日志。

## 保留的冻结边界

未改后端、frame/header/types、DSM 状态表、logger、tools/fake_service 或业务/采集模块；未新增
线程/依赖/公开 RPC/重置策略。原六秒 shutdown 函数逐字保留并独立注册，无断言弱化。
新增测试仅供后续 Linux 执行，命令与未验证/剩余覆盖边界见 validation_review.md。
