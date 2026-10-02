# Thread4 — Validation / Architecture Review

日期：2026-10-02（Asia/Shanghai）。HEAD / 冻结基线：`2ccbc733a1386c46917e6aee2fc7e0680e4321e9`。
验证对象为当前未提交工作树（含 T1–T3 及本次 T4），不是 HEAD 的原始 P3 代码。
用户指定的 `docs/P4/codex/thread4_validation_review_prompt.md` 实际位于
`docs/P4/codex_package/thread4_validation_review_prompt.md`，按该文件执行。

本次用户明确要求隔离环境只进行静态检测，不要求编译测试。这覆盖提示词中的 Linux
build/CTest 要求：本次没有尝试编译器、交叉编译器、CMake、CTest、C++ 程序、服务或 socket。
完成测试源码、结构检查、人工架构审查和有限修复；不宣布 Phase4 运行验收完成。

## 四类验证状态

| 类别 | 本次结论 |
| --- | --- |
| Implemented | T1–T3 恢复实现已接线；T4 增补断言/真实 Linux 集成测试源码，迁移 IPC fixture 和 P3 结构契约，新增 phase4_static_check.py、验收映射与有限修复 |
| Statically Verified | Python 源码/契约检查、注册与 main 调用、冻结文件比较、generation/策略/launch/terminal 人工路径审查及 diff 空白检查通过，实际命令见下表 |
| Runtime Verified | 无；全部 C++ unit/integration，包括原独立六秒 shutdown regression，均未执行 |
| Not Verified | C++ 类型/链接；所有运行断言、实际 child/signal/reap、socket 流量、并发/时序/压力延迟、ARM64/RK3588 行为 |

“测试源码已实现”和“静态通过”都不等于 C++ 测试运行 PASS。这里的 Python 检查是结构和契约
检测，不是 cppcheck/clang-tidy 或 C++ 编译诊断；没有安装或调用这些工具。

## 实际静态执行记录

环境：Windows / PowerShell，Python 标准库；时区按用户 Asia/Shanghai。
最终命令、实际 Python/平台信息、stdout/stderr 与 exit status 已保存到
[static_validation.log](static_validation.log)，该日志只包含静态执行结果。

| 命令 | Exit status | 证据范围 |
| --- | --- | --- |
| `python -B tests/phase3_validation_static_check.py`（迁移前） | 1 | aggregation 的旧 service_cause_ adapter；综合入口的旧 SM restart_policy/maximum_restarts 检查失败，非 C++ 运行失败 |
| `python -B tests/phase3_validation_static_check.py`（最终） | 0 | 四个旧 suite、review fixes、63 无目标/441 有目标声明空间及原场景/注册保留 |
| `python -B tests/phase4_static_check.py`（最终） | 0 | 38 C++ 文件、112 个 main 接入的 test_* 函数、10 executable/11 CTest；17 个冻结文件与基线相同；P4 33 行映射及 ownership/bridge/cancel/IPC 边界 |
| `git diff --check`（最终） | 0 | tracked diff 空白/冲突检查；未跟踪源码、Python 与本次报告另由 phase4_static_check 覆盖 |

P4 结构检查将 33 行矩阵映射到 43 个实际含断言的测试函数；包括验证 EA.expect 内部的
状态比较断言，而不只是检查函数名称存在。阶段开发期间新 checker 曾将该 wrapper 误判为
“无 require”，已修正为先验证 wrapper 的实际 oracle，再检查调用；没有删除该场景。

保留原 8 个 executable / 9 项 CTest，新增 T1/T3 的两项恢复目标合计 10 / 11；本次不新增测试目标。
原六秒回归函数与基线逐字比较（只规范化 CRLF）一致，60s 独立注册和参数保持；没有
WILL_FAIL / DISABLED。设备状态表和测试文件均与基线相同：24 allowed / 417 rejected triples。
没有提交、PR、远端写操作、依赖安装或创建其他 chat/subagent。

## T4 修改与缺陷收敛

- IPC mock 保留原健康、escaping、PID 状态查询、断连和请求顺序断言；type6 现在逐请求投递
  restart_request，由 RM coalesce。query/PID 清除/断连不得使 IPC 自行生成 START 或 cancel。
  原真实 type6 ACK / SERVICE_STOP / old PID reap 场景未删除。
- RM unit 增加相同 due 的 topology/name 次序、不同 due 优先、每 turn 一次 launch、OFFLINE
  取消其他 pending、manual after OFFLINE 不 reset、manual never failure、各阶段 shutdown。
- Linux coordination 增加有 release guard 的 writer observer gate：A 成功后独立 B 故障时，
  队列投递迟到/重复/future/missing 结果和 32 个重复 exit，不改 B 快照或再 reserve；保留 FIFO。
  diamond/duplicate edge/unrelated 隔离使用子进程文件 readiness gate，不能依赖固定 sleep 猜测。
- 增加真实 optional ordinary/heartbeat terminal 差异、critical resource 部分恢复、恢复中的
  signal/observer throw 清理，以及 OFFLINE 后其他服务无悬空 RECOVERING 的用例。
- device_ipc 增加真实 Runtime/exec failure/deadline/RM finalization → DSM → socket subscription
  ERROR/RECOVERING/OFFLINE 链，检查通知源、请求 pairing、无重复 terminal、shutdown 最后快照。
- SM 独立负向用例断言三种 policy 均不自主 reservation/retry，未连接 restart facade 不能假称成功。
- 人工审查发现 OFFLINE 取消只撤销 RM/SM binding、未完成 SM 生命周期：其他服务可无限显示
  RECOVERING。Runtime 的该取消回执路径现在调用已有 stopService；不改变 retry 策略和 grace/reap。
  这是本次唯一 production 修改，详细证据与验证边界见 [review_fixes.md](review_fixes.md)。

## 单 owner 与全部执行入口人工审查

| 入口/数据 | 最终路径与判断 |
| --- | --- |
| restart_policy 的全部 production 出现 | config_manager.hpp 声明/default；config_manager.cpp JSON 解析/枚举校验；recovery_manager.cpp admission 与 classifyExit 决策。SM/Runtime/Monitor/IPC 没有策略读取 |
| recovery_timeout | config 的统一合法范围/默认计算；RM admission 使用绝对 deadline；SM 只接收剩余 startup cap / Operation deadline 执行，不生成第二份恢复期限 |
| 自动计数/due | RM reserveLocked 唯一递增，五次累计 2/4/8/16/32；SM 只 max 投影，不 reset/退款；无 restart_at/legacy delay loop |
| 所有 ServiceManager 内 process launch | 单一 processes_.start(launch_config) 位于 start；普通 start_dependencies 为显式 START 闭包；recoveryReady 为 manual prerequisites；launchRecoveryAttempt 为 RM 授权 replacement。三者通过 active/PID/dependency/cap gate |
| 后端 launch wrapper | PosixProcessSupervisor::launchProcess 只是既有 start 转发；backend 不决定 retry；fork/exec/error rollback/reap/pidfd 与冻结基线完全相同 |
| manual 入口 | IPC type6、SM restartService facade 均投递 EventType::restart_request；Runtime writer submit manual；没有 IPC pending query poll/start owner；ACK 仅入队接受 |
| terminal 的全部 production 入口 | RM terminal / finishLocked 唯一封闭规范化结果；SM finishRecoveryFailure 只执行并回送 captured final token；Runtime takeResults 适配而不推导第二份终结；配置模式拒绝外部 FAILED/TIMEOUT/CANCELLED |
| fault → execution bridge | ServiceStateChange 捕获 operation/context，fault generation 与 launched_generation 分开；RM 绑定 reply.captured，aggregation 的 SERVICE_STARTED 只记 candidate；SUCCESS 匹配完整 context/token/PID/deadline 才清故障 |
| callback / tick | SM callback 只追加 lifecycle_work_；writer drain → reap → SM.tick → drain → RM.tick → drain；RM 无线程，每 turn 至多一个恢复 launch；deadline terminal 先返回给 writer 提交健康再考虑其他 due |
| instance / cleanup | Monitor watch 和 owner reap 都回送 launched_generation；fault/STOP 不重贴旧 PID 的 instance；reap 清 PID/token；旧 PID 数值相同也不能清 replacement |
| public bypass | post(RuntimeEvent) 仅允许资源事实；post(RecoveryResult) 走 active gate；device-wide recovery/health_target bypass 拒绝。standalone aggregator 的 P3 device-wide 能力不能从 Runtime public ingress 调用 |
| cancel/shutdown/OFFLINE | STOP 先取消闭包；shutdown cancelAll 在 stop_all 之前、冻结快照；OFFLINE stopAutomatic 取消 pending，再用 SM stop 原语完成取消生命周期；后续 manual/START 不解 OFFLINE/预算 |

结论：静态路径中只有 RM 进行自动恢复策略决策。未发现第二个 retry owner、无限 automatic
retry、依赖自动 revive 或新增 recovery 线程。deadline 在 writer 可推进时终结 active；不承诺
内核阻塞下硬实时完成。OFFLINE 取消悬空 RECOVERING 已按最小路径修复，运行结果仍未验证。

## P4-01..32 测试源码与断言证据

下表所有行：Implemented（测试源码）+ Statically Verified（结构/人工审查）；Runtime Verified
均为空。U/I 表示源码要运行的层，不表示已运行。缩写：RM=recovery_manager_tests.cpp；
RD=recovery_dependency_tests.cpp；RC=recovery_coordination_tests.cpp；EA=event_aggregation_tests.cpp；
Core=runtime_core_tests.cpp；P2=phase2_integration_tests.cpp；IPC=ipc_integration_tests.cpp；DIPC=device_ipc_tests.cpp。
函数名均省略 `test_` 前缀；可重复映射保存在 `tests/phase4_static_check.py:COVERAGE`。

| ID | 断言证据（文件 / 函数） | 可观察边界 / 待运行层 |
| --- | --- | --- |
| P4-01 | RC real_crash_candidate_success_and_reap | 精确 ERROR→RECOVERING→RUNNING、fault2→launch3、canonical source、old child ECHILD、replacement reap；I |
| P4-02 | RM policy_backoff_exhaustion_and_duplicate_budget；RD crash_backoff_and_limit；RC real_retry_exhaustion_terminal_once | 五个 due−1ms/等于 due、五 reservation、总六 launch、一次 exhausted、无第七；U+I |
| P4-03 | RD heartbeat_deadline_and_reaping；RC real_heartbeat_cleanup_before_replacement | 首 timeout、grace/KILL/reap 前无 replacement、清 latch；U+I |
| P4-04 | RM bound_attempt_failure_invalidates_old_execution / independent_fault_supersedes_episode；RC queue_late_success_duplicate_faults_and_terminal_gate | 同 r 新 attempt 和独立 r2；late/future/missing 不改新 fault、snapshot/通知；U+I gate |
| P4-05 | RM policy_matrix_and_absolute_deadline / deadline_result_idempotency_and_producer_time；RC real_timeout_before_due_without_launch | −1ms/等于/晚于 return checkpoint；due=deadline 零 launch；保留 PID；U+I（精确时间边界 U） |
| P4-06 | RM policy_backoff_exhaustion_and_duplicate_budget；RC queue_late_success_duplicate_faults_and_terminal_gate | 重复 admission 不改 count/due/deadline；队列重复旧 exit 不另建任务；U+I |
| P4-07 | RM duplicate_submit_and_result / deadline_result_idempotency_and_producer_time；RC queue_late_success_duplicate_faults_and_terminal_gate；DIPC real_recovery_terminal_reaches_device_subscription | SUCCESS/FAILED/TIMEOUT gate 幂等、迟到重复不改 B；socket 无第二 terminal；U+I |
| P4-08 | RD dependency_order_and_shutdown / failure_propagation_and_recovery_cancellation；RC real_dependency_stop_closure_requires_explicit_start | diamond、duplicate edge、逆/正序、unrelated、只恢复故障节点、explicit START；U+I |
| P4-09 | RM manual_never_failure_and_shutdown_in_each_phase；RC shutdown_backoff_and_manual_stop_fifo；P2 独立 shutdown | BACKOFF/EXECUTING/manual CANCELLED，不退款/再 launch，合法清理且保存最后快照；U+I |
| P4-10 | RC real_retry_exhaustion_terminal_once；DIPC real_recovery_terminal_reaches_device_subscription | 真实 failed exec 链到 canonical terminal/OFFLINE；START 不解锁；I |
| P4-11 | EA lifecycle_retry_exhaustion_reaches_device_state；RC real_optional_terminal_and_partial_resource_recovery | 普通 optional terminal WARNING；optional heartbeat terminal OFFLINE；U+I |
| P4-12 | EA partial_recovery_case4 / recovery_rechecks_other_critical_failures / resource_critical_downgrade_and_clear；RC real_optional_terminal_and_partial_resource_recovery | SUCCESS 重算其他 optional/critical active/资源；resource I 中无中间 RUNNING；U+I（多服务组合 U） |
| P4-13 | RM policy_matrix_and_absolute_deadline；RD policy_matrix_and_launch_failure / service_manager_has_no_automatic_retry_owner | 三 policy × clean/abnormal/unknown/start/timeout；显式 STOP 不 auto，SM 无 owner；U |
| P4-14 | RM policy_lifetime_success_cancel_and_manual_budget；RD crash_backoff_and_limit；P2 real_crash_backoff_and_restart_limit | 五次 success 不 reset；第六 fault 零新 retry，一次 exhausted；U+I backend |
| P4-15 | RM policy_lifetime_success_cancel_and_manual_budget；IPC ipc_extension_boundaries；RC real_manual_restart_and_stale_instance_facts | reservation 不退、manual 不占预算、duplicate coalesce、断连不 cancel；U+I |
| P4-16 | RM manual_supersede_coalesce_and_preparation；RD explicit_stop_and_blocked_launch / failure_propagation_and_recovery_cancellation；IPC ipc_extension_boundaries | supersede/cancel、STOP 撤销；IPC 不 poll START，依赖 active gate；U+I |
| P4-17 | RM policy_blocked_invariant_manual_failure_and_launch_gate / manual_never_failure_and_shutdown_in_each_phase；RC manual_launch_failure_creates_one_automatic_episode | 一 manual FAILED，enabled 单独 auto，never 无自动 START/terminal；U+I |
| P4-18 | EA stale_recovery_generation_rejected；RC real_crash_candidate_success_and_reap | candidate 不清 fault，只有 captured bridge SUCCESS；U+I |
| P4-19 | Core monitor_instance_reuse_and_cleanup_generation；RC real_manual_restart_and_stale_instance_facts | fake PID 相同、旧 exit/health 拒绝；真实 old child 独占 reap；U+I |
| P4-19b | RD timeout_finalization_preserves_grace_pid_and_instance；Core monitor_instance_reuse_and_cleanup_generation | fault/STOP token 与 launched token 分开、合法 cleanup 清 PID；U |
| P4-20 | RM policy_blocked_invariant_manual_failure_and_launch_gate；RD timeout_finalization_preserves_grace_pid_and_instance | dependency/PID 阻塞不 reserve/launch，deadline 终结而 cleanup 保留 grace/PID；U |
| P4-21 | RM policy_matrix_and_absolute_deadline / policy_blocked_invariant_manual_failure_and_launch_gate；RD manual_restart_reap_and_remaining_startup_cap；RC signal_and_observer_failure_during_recovery_cleanup | 剩余 cap、return timeout 优先、signal gate、异常传播/reap/恢复 handler；U+I |
| P4-22 | RM multi_due_order_one_launch_and_offline_cancellation；RC queue_late_success_duplicate_faults_and_terminal_gate | 每 turn 一 launch、due 优先/稳定 tie；32 envelope 队列负载；U+I（持续负载延迟未测） |
| P4-23 | Core recovery_timeout_config_validation | single/services root 1/86400 与非法类型/范围、174/36089、programmatic overflow；U |
| P4-24 | RD shutdown_deadline_uses_all_service_grace_periods；P2 runtime_shutdown_respects_long_grace_period | 最大 2/6s deadline、六秒/PID/ECHILD 原断言完整保留且独立注册；U+I |
| P4-25 | IPC ipc_extension_boundaries + 原 main 实际 command 场景；DIPC real_recovery_terminal_reaches_device_subscription | type6 单入口、ACK/STOP/request pairing 与旧命令；frame/types 与基线完全一致；I |
| P4-26 | DIPC running_health_requires_confirmed_heartbeats / producer_overflow_disconnect_and_resubscribe / unread_client_output_bound / optional_device_callback_and_sink_lifetime | grace UNKNOWN、非空全 healthy、只读 query、overflow/reconnect/weak sink；I |
| P4-27 | RD invalid_graphs_have_no_side_effects；Runtime 构造中 startup_order | missing/self/cycle 在 service launch/lifecycle 和 run workers 前拒绝；U；见下方 Logger 既有线程限制 |
| P4-28 | RM overflow_and_registry / deadline_result_idempotency_and_producer_time / event_envelope_tail_and_fifo；SM transition guard | RM helper max−1/max、clock overflow；同/倒序 metadata；SM uint64 guard 静态审查，接近最大 SM 注入未运行 |
| P4-29 | RD timeout_finalization_preserves_grace_pid_and_instance / timeout_after_force_does_not_restart_cleanup_grace | final captured token，不创建 auto loop；force 后不重启 grace；U |
| P4-30 | RM multi_due_order_one_launch_and_offline_cancellation；Core terminal_checkpoint_precedes_another_due_launch；RC offline_cancels_other_task_and_leaves_no_recovering_service | OFFLINE 取消其他 pending、停止悬空 lifecycle、budget 不变、START 不解锁；U+I |
| P4-31 | RM policy_blocked_invariant_manual_failure_and_launch_gate / policy_backoff_exhaustion_and_duplicate_budget | invalid executor 一次 EXECUTION_FAILURE 无 fake SUCCESS；普通 launch failure 仍 retry；U |
| P4-32 | RC untrusted_recovery_ingress_cannot_clear_resource；RM result_gate_all_identity_fields；RC queue_late_success_duplicate_faults_and_terminal_gate | source/device-wide/typed bypass 拒绝；资源入口保留；U+I |

## 20 项完成验收映射

以下为逐项静态判断与运行门，不把源码覆盖转换成验收 PASS。

| # | 验收 | 当前证据 / 状态 |
| --- | --- | --- |
| 1 | RM 职责与实现 | ADR01 + recovery_manager.hpp/.cpp；静态满足 |
| 2 | single policy owner | 上方全部 readers/launch review + P4-13；静态满足，运行未验证 |
| 3 | 无两套 retry loop | legacy SM/Runtime/IPC 源码检查 + P4-16/22；静态满足 |
| 4 | Failure→Request | Runtime apply_change + P4-01/03/13；源码已接线，运行未验证 |
| 5 | Execution→Result | RM tick/terminal + P4-01/02/31；源码已接线，运行未验证 |
| 6 | Result generation | captured reply / active gate + P4-04/18；静态满足 |
| 7 | 旧 result 不清新 fault | token/context/PID 完整谓词 + P4-04/19/32；静态满足，gate 场景未运行 |
| 8 | exhaustion terminal | 唯一 reserve cap + terminal + P4-02/14；静态满足，运行未验证 |
| 9 | RECOVERY_FAILED 真实链 | RC/DIPC 测试源码与 Runtime adapter；真实运行证据缺失 |
| 10 | 无逻辑永久 RECOVERING | deadline 与 OFFLINE cancellation fix + P4-05/20/21/30；静态收敛，内核阻塞仍限制 |
| 11 | SUCCESS 重算全部健康 | aggregator evaluate/candidate gate + P4-12；静态满足，组合运行未验证 |
| 12 | dependency 边界 | static graph/closure + P4-08/27；静态满足，Linux 顺序未验证 |
| 13 | 无无限 auto retry | lifetime reservation + P4-02/14/29；静态满足 |
| 14 | 无明显 storm | coalesce、一次 launch/turn、diamond + P4-06/08/22；结构满足，压力未测 |
| 15 | P1–P3 最小兼容 | CHANGE_LIST/旧目标/原六秒函数/状态表保持；内部 token 入口按设计收紧；类型/运行未验证 |
| 16 | Unix socket 保持 | frame/type 文件基线一致 + P4-25/26；静态满足，流量未验证 |
| 17 | 无业务实现 | final production diff 仅允许模块，backend/fixtures 冻结，无业务路径；静态满足 |
| 18 | 无 Phase5 采集 | Monitor diff 仅 watch instance capture；未增加 CPU/procfs/GPU/NPU 采集；静态满足 |
| 19 | 关键恢复路径实测 | P4-01..12 assertion 源码/注册已覆盖；实际运行验收未完成 |
| 20 | 文档/代码/任务一致 | 本报告 + review_fixes；历史设计/T1–T3 报告保持各自时点，由本报告给最终状态 |

## 未验证、剩余边界与后续复现

没有未处理的静态入口失败。下列内容仍没有运行证据，不属于本次用户要求执行的编译/运行工作：

1. 所有 C++ 测试均未执行；也没有 C++ 类型/链接诊断。静态结构不能保证新增 fixture 可构建或通过。
2. 精确 deadline/同步 launch 返回边界通过 fake clock 源码断言；真实 Runtime 测试覆盖 due 前 timeout，
   没有用内核调度宣称可复现精确 ±1ms。EXECUTING shutdown 返回 gate 有 fake executor 断言，
   未声称能在隔离环境实测中断内核 exec/rollback。
3. 队列测试是有界 32-envelope gate，未测持续负载最大通知延迟/硬实时 SLA；需 Linux 压力测量。
4. SM 接近 uint64 最大的真实 mutation 注入尚缺可运行证据；保留 overflow guard，RM helper/clock 边界有测试源码。
5. P4-27 的 service graph 在 lifecycle/monitor/timer/recovery worker 启动前验证；既有 Logger 在成员
   构造时已启动日志线程。如果“worker 启动前”指任何线程，字面条件尚不满足。没有在 T4 改写冻结
   Logger/构造架构；此细项保留限制，不能把 service 无 launch/无 lifecycle side effect 说成零线程。
6. 同步 backend / kernel D-state 可阻塞 writer 或 reap；不可伪清 PID，不承诺硬实时。现有全局队列容量、
   非原子整包 snapshot、socket 旧连接跨代 heartbeat 身份限制、默认 mcu LOW 等风险继续见 RISK_ANALYSIS。
7. T2/T3 的阶段性静态脚本带当时“保护文件不变”约束，不是最终验收入口；T4 授权迁移了 P3 两个
   checker。后续使用 tests/phase3_validation_static_check.py 与 tests/phase4_static_check.py；不覆写历史日志。

后续获得 Linux 环境时可复现（本次未执行）：

```text
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 -B tests/phase3_validation_static_check.py
python3 -B tests/phase4_static_check.py
git diff --check
```

全部 CTest 保持串行默认；process-global signal handlers 场景不能随意并行。需要复查 gate 竞态时仅限定：

```text
ctest --test-dir build -R '^phase4_recovery_coordination$' --repeat until-fail:3 --output-on-failure
ctest --test-dir build -R '^phase2_runtime_shutdown_regression$' --output-on-failure
```

ARM64 使用部署已有 toolchain，另建 build-arm64；仓库没有可引用路径，本报告不虚构参数。
host 构建、目标编译和 RK3588 实机分别记录。Phase4 整体 Runtime Verified / 完成验收仍待后续运行。
