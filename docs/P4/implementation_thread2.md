# Thread2 — Recovery Policy & Service Lifecycle Integration

日期：2026-10-02（Asia/Shanghai）。基线 HEAD：`2ccbc733a1386c46917e6aee2fc7e0680e4321e9`。

## 基线、授权与范围

用户指定的 `docs/P4/codex/thread2_policy_service_prompt.md` 不存在；本次执行实际文件
`docs/P4/codex_package/thread2_policy_service_prompt.md`。已读取 P4 AGENTS、README、CHANGE_LIST、
DECISIONS、RECOVERY_MODEL、RECOVERY_POLICY、RECOVERY_GENERATION、SERVICE_INTEGRATION、TEST_PLAN、
T1 报告和相关实现。开始时已有 T1 未提交改动及 P4 文档，本次保留它们，没有 Git 提交或远端操作。

按用户要求仅静态检测，没有调用编译器、交叉编译器、CMake、CTest、C++ 程序或 Linux 进程/socket 测试。
本次只完成 T2；**Runtime/IPC/aggregation 尚未切换，是不可发布的中间实现**。

## Implemented

| 文件 | T2 实施 |
| --- | --- |
| `include/runtime/recovery.hpp` | Executor writer primitives、captured reply、ExitDisposition；Operation 尾部 deadline/finalize_only；active 尾部 due/prepared/finalizing |
| `include/runtime/recovery_manager.hpp`、`src/runtime/recovery_manager.cpp` | 静态配置构造、policy/classifyExit、五次累计 reservation、2/4/8/16/32 due、绝对 deadline、同步 return checkpoint、手动一次事务、唯一终结和 launch gate |
| `include/runtime/service_manager.hpp`、`src/service/service_manager.cpp` | 删除 legacy retry；Executor 实现、生命周期 snapshot、captured cause/operation/failure_type、launched_generation、保留 grace/PID/reap/dependency graph |
| `include/runtime/config_manager.hpp`、`src/config/config_manager.cpp` | 唯一新增 JSON 字段 recovery_timeout；JSON/SM/RM 共用 validate/recoveryTimeout |
| `tests/recovery_manager_tests.cpp` | 保留 T1 12 个身份测试，增加配置模式策略、期限、累计成功、手动、阻塞、invariant、shutdown gate 场景 |
| `tests/recovery_dependency_tests.cpp` | 显式 SM+RM writer fixture；原 delay/count/launch/reap/依赖顺序断言保留，终结断言改查 RM receipt；增加实际 SM 的超时清理与手动 startup cap 场景 |
| `tests/runtime_core_tests.cpp` | 自动恢复 fixture 组合 RM；独立 SM 无自动重试负向断言；配置单服务/services-root/default/非法值矩阵 |
| `tests/phase2_integration_tests.cpp` | lifecycle fixture 组合 RM，真实 backend/reap/ready pipe 及 PID/grace 断言保留；独立六秒 Runtime shutdown regression 未改 |
| `docs/P4/thread2_static_check.py` | 可重复的 T2 源码结构、owner、保护路径、P3 状态矩阵检查 |

没有修改 T1 的 Event envelope/CMake 注册，也没有修改 Runtime、Monitor、aggregation、IPC、DSM、
ProcessSupervisor ABI/backend、frame、logger、业务服务或旧静态脚本。测试 adapter 在三个允许的
translation unit 内重复保留，避免新增共享生产 adapter 或超出允许路径的新测试头文件。

### 策略与执行不变量

- 生产必须使用 `RecoveryManager(vector<ServiceConfig>, RecoveryExecutor&, Now, LaunchGate)`；definitions
  按 SM startup_order 传入。names-only 构造保留为 T1 被动身份测试 harness，无 tick/自动执行能力，
  不可用于生产。配置模式下旧三参数 submit 仍强制采用 RM 静态 policy/timeout，不能绕过。
- RM 唯一读取 restart_policy 决策：never 首 fault SUPPRESSED_POLICY，不发终结；clean spontaneous
  exit 仅 always 转 failure；异常/未知退出产生 failure，由 policy 决定是否 admit。
  显式 stop/failed/recovering cleanup exit 由 SM 优先处理，不因 raw status 触发新策略。
- 仅 reserveLocked 增加 lifetime 计数。初始启动不计，取消不退，成功和手动操作不 reset。
  第五次成功允许 SUCCESS，之后新 fault 零 launch 即 RETRY_EXHAUSTED；第五次失败同 episode 终结一次。
  零预算 admission 使用 finalize_only，不先把 SM 转为 RECOVERING。
- due、deadline 均用 writer 时间。重复事实不改变它们。old PID/依赖阻塞不重复 reservation。
  每次 tick 最多一个 replacement launch，按 due 排序，同 due 保留 topology/name 顺序。
- manual admission 取消旧 automatic，捕获 stop token；reap 后不推进该 stop token，随后一次 launch。
  manual prerequisite closure 使用统一 active gate 和剩余整秒 startup cap；不自动恢复 dependents。
  manual launch failure 先产出一个内部 FAILED，再以该捕获 fault 创建独立 automatic episode。
- 后端使用整秒 startup_timeout：剩余 duration 向下取整，launch_config 副本取 min；少于 1s 不 launch。
  不修改静态 definition。readiness、launch、finalization 返回后检查实际 clock checkpoint；>=deadline
  TIMEOUT 优先。LaunchGate 在 replacement 前/返回后及每个 manual prerequisite launch 前/后检查。
- configured RM 拒绝外部 FAILED/TIMEOUT/CANCELLED 注入；它们只能由策略/取消路径规范化产生。
  SUCCESS 仍经过 T1 完整 captured identity/PID/exec/RUNNING/deadline 谓词。
- terminal 先撤销 due/operation 许可并置 finalizing，锁外调用 SM，使用 reply.captured.generation
  更新 latest fault 后通过唯一 finishLocked 关闭 active、保存并输出 receipt。finalization 回调有专门 cause，
  不提交新 request。不可执行的 executor reply 产生一次 EXECUTION_FAILURE，不制造 SUCCESS。
- SM 保存未 reap child 的 STARTING launched_generation，fault/stop 不重贴 instance token；reap 清 PID
  时才清 launched_generation。带 instance_generation 的 exit/health_missed 会验证它。
- timeout 可以从 RECOVERING/RUNNING/manual STOPPING 或 STOPPED 进入 FAILED；后两类边只在
  recovery_finalization cause 下授权。未 reap PID 保留，已承诺 grace 不缩短、不重启。
  termination_requested 保存已 force-signal 但未 reap 的清理事实，防止 terminal 再安排一轮 grace。
- backend 的 runtime_error/system_error 仍为启动失败事实；已有固定 `process startup timeout`
  消息只用于区分 failure cause，不用于 identity/freshness。逻辑错误、内存错误和锁外 observer throw
  保留异常传播，交 Runtime 错误清理；没有改 ProcessSupervisor 异常 ABI。
- recovery_timeout 整秒 1..86400；原三种 timeout 1..3600。先验证有界操作数再计算默认
  `63 + 5 * (shutdown + startup + 5) + 1`，默认174、最大合法36089。deadline/due 加法显式检查 timepoint 范围。
  未知 JSON 字段继续原解析行为，max_restart_attempts/restart_delay 等不是新增配置。

## Statically Verified

| 实际命令 | Exit status / 结果 |
| --- | --- |
| `python -B docs/P4/thread2_static_check.py` | 0；37 个 C++ 文件、92 个 main 已接入测试函数、9 executable/10 CTest；16 个保护文件与 HEAD 相同；P3 441-entry/24 allowed/417 rejected 矩阵保留 |
| `python -B tests/phase2_static_check.py` | 0；include、括号、字面量、冲突标记、注册结构 |
| `python -B tests/phase3_state_static_check.py` | 0；原状态表、声明矩阵、结构 |
| `python -B tests/phase3_aggregation_static_check.py` | 0；原 aggregation 表和接线结构，不能证明已迁移 T3 |
| `python -B tests/phase3_ipc_static_check.py` | 0；原 frame/types/注册和文档结构 |
| `git diff --check` | 0；未跟踪文件另由 T2 专项检查换行/空白 |
| `python -B tests/phase3_validation_static_check.py` | 1；旧 SM owner 断言不兼容，见下文；此前四个基础 suite 均通过 |

这些只证明源码结构，不是 C++ 类型分析或运行验证。原有 executable/CTest 均保留，
独立 shutdown regression 仍为60s CTest timeout，六秒及 PID/reap 断言未修改。
旧 P3 综合检查的 ownership remediation 仍要求 SM 的 `service.config.restart_policy != RestartPolicy::never`
和 `restart_count >= maximum_restarts`，与本次授权的迁移冲突；保留脚本并记录失败，未删检查来掩盖。
`tests/phase4_static_check.py` 尚不存在，属于 T4 全迁移审计，没有假称执行。

新增测试源码覆盖 P4-02/05/13/14/15/20/21/23/29/31 策略部分，以及手动失败、单 turn launch gate。
它们没有运行；不将断言存在、静态词法通过或 Python 检查输出解释成 C++ 行为通过。
本环境未发现 cppcheck/clang-tidy；未安装工具，也没有 C++ 类型静态分析通过记录。

## Runtime Verified

无。

## Not Verified

C++ 类型/链接、所有新增/迁移 unit 的执行、Linux exec/signals/reap、真实 observer/queue 时序、
socket 通知、ARM64/RK3588 行为均未验证。当前 Runtime 仍没有构造/调用 RM，IPC 仍保留旧 manual poll，
Runtime 仍保留旧终结推导；SM 自动循环已经移除，所以生产自动恢复尚不可用。
`event_aggregation_tests.cpp` 的 SM 自主 exhaustion fixture 属于 T3/T4 迁移范围，尚未改动，
不能宣称全套旧 C++ 测试现在可通过。

## T3 接线交接

1. 在完整注册/graph 验证后，以 SM startup_order 的 ServiceConfig 列表创建配置模式 RM；SM 作为 executor。
   使用 Runtime 实际 steady Clock::now；LaunchGate 必须仅读取 shutdown/signal 门，不触发 callback 或 query SM。
   gate 返回 false 时 RM cancelAll，Runtime 随即进入现有 stop_all/最大 grace+5s/reap shutdown cleanup。
2. StateChangeSink 只将完整 ServiceStateChange 追加 writer work，不能回调中直接 submit/tick。
   沿用 FIFO，在 tick 前排空已捕获 failure/stop 工作；producer at 只作事实 metadata，admission 用当前 writer now。
3. process_exited 的 PID 和 instance_generation 从 reap owner 映射捕获；Runtime 调 RM.classifyExit，
   再调用 SM.handleProcessExit(event, disposition)。没有分类的 standalone SM 保持 clean STOPPED/abnormal FAILED。
4. 无 operation 的 FAILED 使用 change.generation/failure_type 建 request；有 operation 的 attempt failure
   使用其 captured context 调 observeFailure(..., writer_now)。**不得在 rejected observeFailure 后降级为新 submit**。
   tick 已直接消费的 synchronous failed reply 对应回调随后到达时是重复旧工作，应拒绝。
5. recovery_finalization cause 不 admission；manual recovery_preparation 的 STOPPING/STOPPED 不 cancel 本 manual。
   explicit/dependency stop 应在接受 writer 命令时先 cancel 受影响闭包，仍处理 captured callback 清理健康事实。
6. 每个 writer turn 先执行既有 SM cleanup tick、排空事实，再 RM.tick(actual_now)，再次排空 captured callbacks，
   消费 takeResults sealed receipt。不要仅 health_check tick；不要把 receipt 再 post 到外部 active-result gate。
7. 接线需在 callback candidate 与 canonical result 之间保留 aggregation binding，RUNNING callback 只为 candidate；
   只有 canonical SUCCESS 清正确 service fault，FAILED/TIMEOUT 使用 final captured latest_fault_generation。
   manual/CANCELLED 仅内部诊断，不发布自动 recovery trio。
8. 新 admission 正常保留 active 时发送一次 RECOVERY_START；零预算/已立即终结 admission 从 receipt 发唯一 terminal，
   不制造 START。duplicate admission 不重复 START。移除 Runtime service_recoveries_/recovery_exhausted producer 和 IPC poll。
9. 保留 restartService routing facade，在 Runtime 构造时把 sink 接到同一个 internal restart_request 队列入口。
   RM snapshot 不推进生命周期。P3 旧 ownership 检查与 aggregation fixture 需由 T3/T4 按新 owner 精确迁移，
   同时保留六秒 regression、PID/reap、441-entry 状态矩阵和所有旧 executable/CTest。
