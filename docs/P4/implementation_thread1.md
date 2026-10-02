# Thread1 — Recovery Contract & Generation Core 实施交接

日期：2026-10-02（Asia/Shanghai）。本次执行 T1，不代表整个 Phase4 已实现。

## 基线与范围

用户给出的 `docs/P4/codex/thread1_contract_core_prompt.md` 在仓库中不存在；实际任务文件为
`docs/P4/codex_package/thread1_contract_core_prompt.md`，本次按它执行。
HEAD 为 `2ccbc733a1386c46917e6aee2fc7e0680e4321e9`，与设计事实基线相同。
开始时只有用户提供的 `docs/P4/` 未跟踪目录，没有已有 tracked 源码改动。

已读取 README、AGENTS、ARCHITECTURE、RECOVERY_MODEL、RECOVERY_GENERATION、
RECOVERY_POLICY、CHANGE_LIST、TEST_PLAN、codex_package/README、T1/T2 提示词，
并核对 P3 EVENT_MODEL/review_fixes 及 P4 SERVICE_INTEGRATION/RECOVERY_EVENT_FLOW/DECISIONS。
遵循用户此次明确的隔离环境约束：仅静态检测，不要求或尝试编译、CMake、CTest。

## Implemented

| 文件 | 实施内容 |
| --- | --- |
| `include/runtime/recovery.hpp` | Request/Result/Context、failure/origin/outcome/terminal/admission 类型，捕获 Operation、只读 Executor snapshot、active/snapshot |
| `include/runtime/recovery_manager.hpp` | 单 writer 身份核心接口；查询返回同步副本；没有执行线程或生产生命周期回调 |
| `src/runtime/recovery_manager.cpp` | per-service episode admission、单 active、重复事实合并、captured launch binding、attempt failure invalidation、真实 Result gate、取消/关闭/一次性结果回执 |
| `include/runtime/event.hpp` | RuntimeEvent 尾部 optional context；Event 尾部 instance/context/result envelope；EventType 尾部 restart_request/recovery_result |
| `tests/recovery_manager_tests.cpp` | 12 个假执行器/显式 writer 时间的测试函数，均由 main 调用 |
| `CMakeLists.txt`、`tests/CMakeLists.txt` | core 源文件和 phase4_recovery_manager_unit 注册；原有注册和超时保持 |
| `docs/P4/implementation_thread1.md` | 本次报告及 T2 接口交接 |

没有改动 SM、ProcessSupervisor、Monitor、config、Runtime、aggregation、DSM、IPC/frame、
logger、业务服务、旧测试或旧静态脚本。`src/runtime/event.cpp` 不需要改动。
新 RM 仅编入库，没有 production 实例或路由，基线 P2/P3 owner 尚未迁移，也没有启动第二套 retry。

### 身份和结果不变量

- 每注册 service 只保留一个 active、一个 last_result、episode counter 和累计 reservation 投影。
  `completed_` 仅作为待排空的规范化结果工作 FIFO，不作为历史记录。
- episode 由 RM admission 发放、非零且单调；成功/取消不归零。overflow 明确抛错，不复用旧 token。
  SM 原有 lifecycle generation 实现未修改；core 从不发放或写入 lifecycle/PID/设备状态。
- initial fault 保持不变；expected/latest fault 从捕获事实推进。fault=10、launch=11、
  bound failure=12、下一 launch=13 属于同 episode，但 execution=11 的结果永久失效。
- 独立新 fault 创建新 episode，先产出旧 episode 的 CANCELLED(SUPERSEDED)。manual
  supersede automatic 使用 SUPERSEDED_MANUAL；同一 active manual 重复请求合并。
- Result 的 inspection 和实际 observe/finish 都调用 `acceptsLocked`：service/episode/origin/
  initial/latest fault/execution/累计投影/故障类型/PID/当前 expected token 必须一致。
  SUCCESS 还要求已捕获 launch、exec 已成功、SM projection 为 RUNNING、launch token 和 PID 一致，
  且实际 writer checkpoint 小于绝对 deadline。缺失、旧、未来或已经关闭的 binding 不可接受。
- 非 success 仍经过身份 gate；TIMEOUT 仅在 checkpoint >= deadline 可接受，deadline 时不授权新执行。
  外部 CANCELLED 被拒绝，只能由 cancel/cancelAll/supersede 产生。重复 terminal 无额外输出。
- producer_time、result.completed_at、reason/source 字符串不作为 freshness/身份依据。
  canonical completed_at 由 writer checkpoint 重写。Result 不含 source 认证字段。
- 初始/最新 failure_type 是 Result 尾部的内部重复事实指纹，防止原 crash 与后续 startup failure
  混淆；不新增 request ID、lifecycle counter 或 wire 字段。
- finish 先准备结果存储，在同一无 callback 的临界区撤销 active，再交付 sealed receipt。
  程序级异常不吞掉、不包装为 SUCCESS。

## Statically Verified

以下命令/检查 exit status 均为 0；只证明对应源码结构和注册约束。

1. `python -B tests/phase3_validation_static_check.py`
   - 四个原有 suite、P3 remediation/matrix 检查全部通过。
   - 37 个 C++ 文件的本地 include、括号、注释/字面量、冲突标记检查。
   - 84 个 test 函数由 main 调用；9 个 executable target、10 项 CTest 注册。
     其中原有 8 target/9 CTest 和独立六秒 shutdown regression 均保留。
   - 原有 441 项设备目标声明矩阵及 24 allowed / 417 rejected 未改变。
2. T1 专项只读 Python 审计（PowerShell here-string，经 `python -B -` 执行）：
   - 对七个源码/注册 artifact 检查末尾换行、尾随空白、冲突标记及本地 include/括号。
   - 与 `git show HEAD:<path>` 比较 Event/RuntimeEvent 原字段前缀及枚举值位置；
     RuntimeEventType 仍为原有八种；旧 executable/CTest/timeout 注册逐项保留。
   - 核对两个公开 Result 接口确实共用真实谓词，以及 identity、launch/PID、deadline、
     overflow、binding invalidation、shutdown 与 finish 的源码条件。
   - 20 个受保护的生产文件与 HEAD 内容一致（仅规范化 CRLF）；tracked 改动均在 T1 允许范围。
   - core 不含 production 生命周期写入、策略读取、线程/fork/exec/waitpid，或 timestamp/source 认证。
   - 12 个新测试函数定义完整并接入 main。
3. `git diff --check`：通过；Git 的 LF→CRLF 提示不是检查失败。
   未跟踪源码另外由上述专项检查覆盖，不能仅依赖 git diff --check。

当前环境未发现 `cppcheck`/`clang-tidy`；以上是 Python 源码结构检测与人工逐路径审查，
不是 C++ 类型分析器通过记录。未删除或放宽任何旧静态检查。
`tests/phase4_static_check.py` 尚不存在，它属于 T4 最终迁移审计，本次未假称执行它。

### 新测试源码与矩阵映射

| 测试函数 | 声明的行为覆盖 |
| --- | --- |
| test_admission_metadata_and_snapshot | 未知 service、空/非法 metadata、缺失/旧/未来 fault、producer 不得分配 episode、snapshot 副本、admission 不执行生命周期 |
| test_duplicate_submit_and_result | P4-06/07：重复请求不变 deadline/episode/投影；bridge 后重复合并；SUCCESS 一次；成功后 episode 单调 |
| test_result_gate_all_identity_fields | P4-04/18/32 core：每个关联字段、当前 token/launch token/PID/状态、无 launch SUCCESS、fault 与 execution 区分 |
| test_bound_attempt_failure_invalidates_old_execution | P4-04/18：同 episode 新 fault 撤销旧 execution；再次 launch/PID 复用仍拒绝旧结果 |
| test_independent_fault_supersedes_episode | P4-04：独立新故障 r2；旧 r1 cancel 一次，旧结果/binding 无效 |
| test_failed_launch_without_pid_and_latest_fault_duplicate | 无 PID/无成功 launch 的捕获失败、initial/latest 区分、最新故障类型幂等、同 token 不另建任务 |
| test_cancel_shutdown_and_service_isolation | STOP token 阻止旧成功、取消幂等、per-service 隔离、shutdown 禁止新 admission/execution |
| test_manual_supersede_coalesce_and_preparation | manual supersede/coalesce；STOPPING→STOPPED 同 stop token；使用捕获 stop token launch；manual 不修改自动预算 |
| test_deadline_result_idempotency_and_producer_time | P4-07/28 core：FAILED/TIMEOUT 一次；timestamp 倒序/未来不改变身份；deadline−1ms/等于/之后 gate；不虚构 execution |
| test_captured_binding_and_launch_handshake | 原 Operation 捕获、拒绝完成时重贴 token/future episode、STARTING 不是 SUCCESS、确认 exec 后绑定幂等 |
| test_overflow_and_registry | P4-28 core：实际 admission 使用的 nextGeneration helper 边界、deadline overflow 无部分 admission、非法 registry |
| test_event_envelope_tail_and_fifo | 尾部数据 FIFO 保留；倒序 producer time 不重排；typed Result 走实际 gate，closed episode 不复活 |

这些是已检查的测试源码，不是测试运行结果。P4-32 仅完成 core typed Result gate；
现有 Runtime 的 named/device-wide RuntimeEvent ingress、source spoof 与健康发布门必须由 T3 完成。

## Runtime Verified

无。没有调用 C++ 程序、Linux 服务/进程、socket、CMake、CTest、编译器或交叉编译器。

## Not Verified

C++ 类型/链接、上述 12 个 unit 的实际行为、并发/回调时序、真实 fork/exec/reap/grace、
设备健康聚合/通知、Linux host 与 ARM64/RK3588 运行结果均未验证。
T1 没有实现策略选择、五次 reservation、2/4/8/16/32 backoff、due 调度、tick、
配置 recovery_timeout、terminal SM finalization、生产实例 generation capture 或 Runtime/IPC 接线。
上述均是指定的 T2/T3/T4 范围，不能把本报告当作 Phase4 production 完成验收。

## T2 交接：executor 和下一阶段接口语义

1. `RecoveryExecutor::snapshot` 是只读 SM projection，需给出当前 lifecycle generation、
   仍未 reap 子进程的 launched_generation、PID、state。必须无副作用/无 callback/RM reentry，
   executor 生命周期覆盖 RM。core 的 mutex 用于 snapshot 同步，不授权多个 lifecycle writer。
2. T1 `submit(request, writer_now, timeout)` 是已验证输入的 admission core，不读取 ServiceConfig。
   T2 在 RM 内补 policy/classifyExit、SUPPRESSED_POLICY、timeout 静态推导与 programmatic 校验，
   再调用/整合 admission。当前 attempts_reserved_total 保留为 0；T2 实现唯一累计 reservation
   和 SM count projection，禁止用 caller 随意填 Result 的 count 来代替预算。
3. `beginExecution` 只返回 captured Operation，不调用 executor。T2 先检查 due/deadline、
   old PID/prerequisites/单 turn launch 限制，再交 SM primitive，使用 captured expected token。
   automatic BACKOFF 不是可以立即 launch 的 production 许可；T1 不计算 due。
4. manual 的 stop/reap 阶段用原 Operation 关联，通过 `observePreparation` 记录捕获 stop token。
   后续 launch primitive 的 expected_current_gen 使用这个 preparation 回调值，仍回送原 Operation。
   例如 begin token=10、stop token=11、launch token=12；不得完成时 query 最新 token 为旧执行补标签。
5. exec 成功用 SM 返回的 captured STARTING/RUNNING generation、PID 调 `bindExecution`，
   再以实际返回 checkpoint 检查 Result。RUNNING snapshot 不能替代 exec 成功的捕获确认。
   同步失败且 PID 已 rollback 时可使用原 Operation.context 调 `observeFailure`，不虚构 launch
   或 query 新 token 标为 SUCCESS。若已绑定 launch，则失败回送带该 execution 的 active context。
6. automatic bound failure 仅更新 expected/latest、撤销旧 operation/execution、回 BACKOFF；
   T2 再决定 reserve/retry/timeout/exhausted。manual 新 launch 失败应先终结 manual FAILED，
   再从独立 failure 建 automatic episode；不能把 manual 当 automatic retry 延续。
7. T2 必须增加 terminal finalization 的内部准备/提交步骤：先撤销 launch/due，保留 context，
   调 SM finishRecoveryFailure，捕获 final FAILED generation 更新 latest fault，最后 finish。
   本次单步 `observe` 不执行该 primitive；不能为了接线把 finalization callback 当新 failure retry。
   成功/非成功的策略授权需在 RM 内收敛，不能只依赖 T1 身份匹配认定 RETRY_EXHAUSTED。
8. `takeResults` 是仅供内部 writer 消费的 sealed canonical receipt，关闭 active 后仍可交给
   adapter 一次；不能再次 post 到外部 `observe`，也不能以 source 字符串赋予 canonical 权限。
   每 writer turn 排空；CANCELLED/manual 结果只内部诊断，不发布自动 recovery trio。
9. T2 迁移 SM policy/primitives/config 与组合 fixture；T3 再一次切换 Runtime/Monitor/aggregation/IPC，
   删除旧 producer/poll/owner。禁止在当前 T1 production 旁挂自动 RM 以形成两个 owner。

T2 可继续在上述允许共享文件中扩展 policy/tick/finalization 接口。没有主动新建后续 chat、
提交 Git 或写入远端；此次交接保存在本文件供后续任务读取。
