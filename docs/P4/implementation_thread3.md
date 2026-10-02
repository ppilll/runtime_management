# Thread3 — Runtime / Device / Dependency / IPC Coordination

日期：2026-10-02（Asia/Shanghai）。基线 HEAD：`2ccbc733a1386c46917e6aee2fc7e0680e4321e9`。

用户指定的 `docs/P4/codex/thread3_runtime_coordination_prompt.md` 不存在；实际任务文件是
`docs/P4/codex_package/thread3_runtime_coordination_prompt.md`，本次按该文件执行。
已读取 P4 AGENTS、README、ARCHITECTURE、CHANGE_LIST、RECOVERY_GENERATION、RECOVERY_EVENT_FLOW、
SERVICE_INTEGRATION、DEVICE_STATE_INTEGRATION、TEST_PLAN、T1/T2 报告及 P3 EVENT_MODEL、
AGGREGATION_RULE、IPC_EXTENSION、review_fixes。保留开始时已有 T1/T2 未提交修改，没有提交 Git 或写入远端。

按用户本次指令只做静态检测；没有调用编译器、交叉编译器、CMake、CTest、C++ 可执行文件、
Linux 子进程或 socket 测试。下述集成场景均为测试源码，不能解释为运行通过。

## Implemented

| 路径 | 本次实施 |
| --- | --- |
| `include/runtime/runtime_manager.hpp`、`src/runtime/runtime_manager.cpp` | 拓扑配置模式 RM 构造、重启 facade、writer 本地 lifecycle FIFO、因果排空、captured fact/result gate、reap 直接应用、每轮 SM/RM checkpoint、取消闭包、关机先 cancelAll |
| `include/runtime/service_aggregation.hpp`、`src/runtime/service_aggregation.cpp` | active episode/initial fault/origin、candidate execution generation；候选不清故障；正确结果才清本服务；取消与手动完成的 writer 接口 |
| `include/runtime/monitor.hpp`、`src/monitor/monitor.cpp` | watch 保存 launch instance generation，health_missed 回送捕获身份；未增加资源采集或线程 |
| `src/ipc/ipc_manager.cpp` | type6 只投递一次 internal restart_request，保留 ACK/SERVICE_STOP；删除 pending_restarts、cancel_restarts、STOPPED/PID 轮询和 IPC 发 START |
| `include/runtime/recovery.hpp`、`include/runtime/recovery_manager.hpp`、`src/runtime/recovery_manager.cpp` | canonical admission START receipt、OFFLINE 撤销/automatic admission gate、terminal writer checkpoint、耗尽撤销旧 due |
| `include/runtime/service_manager.hpp`、`src/service/service_manager.cpp` | Monitor 捕获 token；手动 prerequisite 每 turn 至多启动一个，返回排空事实；普通 START 以实际 active binding 作 admission gate；deprecated 字段注释 |
| `include/runtime/event.hpp` | 接线完成后的内部 envelope 注释，字段/枚举顺序未改 |
| `tests/runtime_core_tests.cpp` | 实例/PID 复用、合法 fault cleanup、终结先于另一 due launch、OFFLINE 后显式 START；device-wide recovery bypass 的旧 fixture 改为拒绝断言 |
| `tests/event_aggregation_tests.cpp` | 手工事实迁移为完整 context/candidate/result；SM+RM exhaustion fixture；旧/future/missing/错误 episode/execution、重复结果及绑定 attempt failure 断言 |
| `tests/recovery_coordination_tests.cpp`、`tests/CMakeLists.txt` | 新真实 Linux Runtime 链路源码及 phase4_recovery_coordination 注册，180s 预算；所有原有目标/注册保持 |
| `docs/P4/thread3_static_check.py` | 可重复的接线、owner、关联门、状态表/协议、保护路径、六秒 regression 和源码结构审计 |

未修改 DSM 状态表、ProcessSupervisor/backend、IPC frame/header/type/error/schema、IPC 头文件、
logger、fake_service、业务服务、旧静态脚本、T1/T2 报告或 P0–P3 文档。
没有新增恢复线程或 Runtime/device 自动重启动作，没有自动复活被停止的依赖方。

### Writer、事实和结果路径

1. StateChangeSink 只复制完整 ServiceStateChange 到 writer-local FIFO。
   Dispatcher subscriber 只做健康聚合，没有从 subscriber 递归调用 SM/RM 生命周期。
2. 每轮处理一个 queued envelope，排空 captured callbacks/dispatcher、RM START/Result receipts；
   再直接应用已 reap 的 PID+launched_generation exit，SM cleanup tick、排空、RM tick、再排空。
   producer timestamp 不排序、不判 identity；admission/deadline 使用实际 steady writer checkpoint。
3. 无 operation 的已接受 failure 创建 request；带 operation 的 failure 只 observeFailure。
   同步失败 reply 已由 RM 消费，其延迟 callback 不再退化为独立 submit。
   recovery_finalization 只更新故障和绑定，不创建自动 episode。
4. 自动 RECOVERY_START 来自 RM 一次性 admission receipt，包括 manual 失败转新的 automatic episode。
   retry preparation 只刷新同一 episode binding，不重复 START。零预算 admission 没有 START。
5. recovery RUNNING callback 仅记录 execution candidate，保留 failed/stopped/heartbeat latch。
   RM 先完成身份、PID、state、deadline gate，关闭 active 并输出 sealed receipt，随后 Runtime 发布唯一规范化结果。
   terminal 使用 receipt 的 final captured latest_fault_generation；没有完成时 query 新 token 给旧结果补标签。
6. CANCELLED/manual results 不发布 automatic recovery trio。Manual SUCCESS 经 writer-local completion
   清除其正确 candidate 并重算全部健康；manual FAILED 后的新自动任务拥有独立 episode/预算。
7. public post(RuntimeEvent) 只允许 resource_warning；具名 lifecycle/recovery 和 device-wide recovery
   不能靠 source 字符串绕过。post(RecoveryResult) 调实际 RM observe gate。显式 DeviceStateEvent
   禁止 recovery 三类 trigger/health_target 绕过 terminal owner。

### 取消、依赖和关机

- STOP writer 接受时先取消目标及静态 transitive dependent closure，再交 SM stop。
  failure/clean exit/manual preparation 取消受影响 dependent；SM 保留反向拓扑停止和清理。
- 故障节点成功不会启动 dependent。Explicit START 保留原 prerequisite closure；active binding 拒绝 bypass。
  手动 prerequisite 每次 readiness 至多启动一个，RM 返回 writer 排空后才允许后续启动。
- RM tick 产生 deadline terminal 后返回 writer checkpoint，再决定其它服务是否可启动。
  避免 HIGH terminal 尚未聚合为 OFFLINE 时，另一 due replacement 抢先启动。
- 第一次接受 OFFLINE 撤销所有当时 pending work，禁止后续 automatic admission。
  后续 manual/START 可执行原生命周期语义，但不能退出 OFFLINE 或重置预算。
  取消后遗留 RECOVERING lifecycle 标签不再充当第二份 admission gate，实际 active binding 才是 gate。
- Shutdown 先标记 shutting_down、RM.cancelAll 并排空取消工作，再停止线程/SM.stop_all。
  保留最大 outstanding grace deadline+5s、全部 PID 清除才返回、错误传播和 signal handler 恢复。
  正常关机不改变最后设备健康快照。
- Monitor/reap 身份使用 STARTING 时保存的 launched_generation。
  fault/stop generation 推进后，合法旧 child cleanup 仍可接受；旧 token 即使 PID 相同且 timestamp
  更新也不能失败 replacement。Backend 没有新增计数器或重写。

### 局部修正的理由

T2 的 reserveLocked 在 count 已到 5 时直接返回，旧 due 可能留在 active；第五次失败后可能再次获得
launch 资格。本次先清 due，再检查上限，保留五次累计 reservation 和原退避序列。
新增 exhaustion composition/真实 Runtime 测试源码保留六次总启动与 count5 断言。

Terminal checkpoint、manual prerequisite 分 turn 和取消后显式 START gate 是接线所需的 RM/SM
局部修正；没有将策略移回 Runtime/SM。restart_policy 的决策读取只有 RM；ConfigManager 仅解析/校验。

## Statically Verified

| 实际检查 | Exit status / 结果 |
| --- | --- |
| `python -B docs/P4/thread3_static_check.py` | 0；38 个 C++ 文件、104 个 main-wired test 函数、10 executable/11 CTest；owner、callback、checkpoint、candidate/result/ingress、severity/source、16 个保护文件、完整 441-entry 矩阵与六秒 regression |
| `python -B tests/phase2_static_check.py` | 0；include、括号、字面量/注释、冲突标记、测试函数和注册结构 |
| `python -B tests/phase3_state_static_check.py` | 0；原七个状态/九条无 target 边及声明矩阵 |
| `python -B tests/phase3_ipc_static_check.py` | 0；旧 frame/types、JSON 示例、健康查询/订阅/有界队列接线 |
| `git diff --check` | 0；LF→CRLF 提示不是检查失败；新增未跟踪源码由专项检查另覆盖 |
| `python -B tests/phase3_validation_static_check.py` | 1；历史 owner/adapter 条件与 P4 迁移不兼容，见下文 |

P3 综合入口的失败包括旧 aggregation checker 要求 `service_cause_->type == EventType::health_missed`、
固定两次 dispatcher drain/旧 callback 结构；旧 remediation 要求 SM 读取 restart_policy/maximum_restarts
并由 Runtime 读取 recovery_exhausted 发布 terminal。新实现用 captured failure_type、writer-local FIFO
及 RM sealed receipt 替换它们，不能保留这些生产分支只为满足旧字符串检查。
本次没有删除、放宽或改写旧检查；T4 应按新 owner 精确迁移并继续保留原状态/协议/关机断言。

以上仅为源码结构和契约审查。未发现 cppcheck/clang-tidy，未安装或调用 C++ 类型静态分析器。
`tests/phase4_static_check.py` 尚不存在，按分工留给 T4 全迁移审计，没有假称执行。

## Runtime Verified

无。用户要求隔离环境只静态检测。

## Not Verified / T4 交接

C++ 类型/链接、所有 C++ 测试实际执行、真实 exec/signals/reap/grace、observer/queue 并发、
socket 通知和 ARM64/RK3588 行为均未验证。Recovery deadline 保留同步后端的逻辑 checkpoint
语义，没有声称内核阻塞下的硬实时完成保证。

新 coordination 可执行源码覆盖：真实 crash 成功/candidate/旧 child exclusive reap、五次失败耗尽、
due 前 timeout、忽略 TERM 的 heartbeat cleanup、backoff shutdown、manual/STOP FIFO、manual 成功及
迟到 instance facts、manual 失败转独立 automatic episode、重复依赖边的停止/显式恢复、伪造 owner
和 device-wide/typed result ingress。所有场景使用有界等待；耗尽场景保留真实 2/4/8/16/32 退避。

P4-01/03/04/08/09/10/11/12/16/17/18/19/22/24/25/29/30/32 有对应新/迁移源码或保留契约，
不表示这些矩阵已经运行验收。T4 仍应完成 diamond、多 due/队列压力、signal/observer gates、
optional terminal/资源与真实设备通知的组合覆盖，以及 Linux 环境编译和运行（若后续获得相应环境）。

T4 必须迁移 `tests/ipc_integration_tests.cpp` 中 mock pending-restart 的旧语义断言：它当前还预期
IPC 合并为一个 STOP、轮询 query 后发 START。正确迁移应断言每个 type6 只投递 restart_request、
duplicate 由 RM coalesce、断连不取消、STOP 的取消由 Runtime/RM 完成。该文件是 T4 路径，本次没有
越界修改。现有真实 type6/ACK/SERVICE_STOP/PID-reap 场景和 device_ipc/所有旧目标均保留，不能据当前
静态通过声称整个旧 C++ 测试套件兼容或 Phase4 最终验收完成。
