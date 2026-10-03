# T3 implementation — Monitoring Policy & Runtime Integration

日期：2026-10-03（Asia/Shanghai）。入口：`codex_package/thread3_prompt.md` → `thread3_task.md`。
按本次用户指令，仅交付实现和静态检测；不执行或报告编译测试。

已读取根/P5 AGENTS、README、CHANGE_LIST、package README、DESIGN_FREEZE_CANDIDATE、
ARCHITECTURE、THRESHOLD_POLICY、SAMPLING_AND_FAILURE、DEVICE_STATE_INTEGRATION、
CONFIGURATION、RESOURCE_MODEL、TEST_PLAN、P3 MONITOR_INTEGRATION 及 T1/T2 报告。
HEAD 为 `4843061fce2a329832105d34aaea380a35560c0e`，与设计基线相同。
开始时工作区已有 T1/T2 源码、注册与未跟踪的 AGENTS、docs/P5、temp.log，均保留。

## Implemented

本轮只修改以下 T3 Allowed Files 及本报告：

| 文件 | 实际改动 |
| --- | --- |
| `include/runtime/monitor.hpp`、`src/monitor/monitor.cpp` | 尾部 optional clear 阈值、typed observation、独立资源 mutex、共享迟滞 policy/change-only facts |
| `include/runtime/runtime_manager.hpp`、`src/runtime/runtime_manager.cpp` | native/external 构造、reserved source 准入、private queue adapter、现有 worker 采样、due/coalesce、快照/query、失败日志与关闭 gate |
| `src/ipc/main.cpp` | 一次 load_runtime_file；新构造默认 native；同一 config.services 供 IPC |
| `tests/runtime_core_tests.cpp` | 仅迁移 test_resource_monitor_thresholds 的迟滞/数量 oracle，加强非法双输入不部分发布 |
| `tests/resource_monitoring_tests.cpp` | 保留 T2 九函数，增加七个 policy/schedule/runtime fixture 函数 |
| `tests/CMakeLists.txt` | P5 monitoring 目标 timeout 从 20 调为 60 秒，容纳串行 timer/gate fixtures；旧目标/属性保持 |

T2 Config、Collector、SM 接口只读，未修改。事件、聚合、DSM、Recovery、后端、Logger、IPC 协议、fake_service、P0–P4 文档和原六秒回归函数均未修改。

### Policy / compatibility

- `ResourceThresholds` 原前三值仍为 CPU warning、Memory warning、Memory critical。
  新 optional clear 未指定时派生 max(0,warning-5)、max(0,warning-5)、max(memory_warning,critical-5)。
  显式 clear 要 finite、0..100，并满足冻结的严格次序；原合法低阈值三值构造仍接受。
- `Monitor::observeResources(SystemResourceSnapshot)` 为唯一资源 policy。
  `report_resources` 先整体校验两 double，再构造 typed valid measurements 委托同一 policy。
  typed valid 缺值/非法值也在任何 policy 状态改变前整体拒绝。
- CPU 80 激活、75 clear，100 仍只有 WARNING。Memory 80/75、95/90，94.9 保持 critical，90 一条同 source active warning 降级，75 一条 clear。
- 首份 valid metric 发布初始化 fact，之后仅 state/severity 改变发布；warming_up/unavailable 不初始化、不 clear 已确认 latch。
  CPU 与 Memory validity 独立。reason 含 source/百分比/阈值/action，事件 at 使用 system.sampled_at。
- 资源 policy mutex 与 heartbeat mutex 独立；所有 sink 调用在两者锁外。既有 heartbeat 四个函数逐函数保持原文。
  外部生产者仍须按原契约串行提交，同一 source 不混合直接 facts 与百分比生产。

### Runtime / startup

- 新 `RuntimeManager(RuntimeConfig, aggregation, sink, input_mode, reader, now)` 默认 native，使用完整 config 的七字段，构造时验证 monitoring；不接受第二份 legacy thresholds。
  Reader/Now 是窄 seam，默认 bounded proc reader 和 steady clock。
- 旧 path 构造委托 external；完整 loader 仍校验文件，但明确的 ResourceThresholds 参数决定 policy，legacy interval 保持默认 2 秒。
  external 不构造 Collector、不自动采样，保留 run 前 report → FIFO → 聚合。
- native 的 reportResourceUsage 抛 logic_error；public post(RuntimeEvent) 委托 post(Event)，后者检查所有携带 runtime_event 的 envelope 的保留 source，封住重载与 envelope 绕过。
  其它 source 的原 resource fact ingress 保持。内部 ResourceSink 只经 private enqueue_resource_fact 直接 queue_.push。
- 生产 main 只加载一次配置，默认 native，即旧无 monitoring 配置也启用采样。
  CLI/socket、IPC service 定义、device_changes sink 生命周期与启动/停止结构保持。

### Sampling / current cache / shutdown

- 仅复用原 Monitor 与 Timer 两个 worker。1 秒 timer health_check 用一个 atomic pending slot coalesce；worker 取出时释放，shutdown envelope 不经过该 slot。
- 每轮先 `Monitor::check(Clock::now())`，再检查资源 due。资源时钟使用实际 steady clock，首次合法 tick 可采样；不使用 queued Event.at。
- 每周期 system → SM try capture → Collector process reads → SM batch try validate → current cache → Monitor facts。
  process Continue 回调检查原子停止 gate；无 heartbeat/SM/cache/policy 锁包住 proc I/O。
- `ResourceSamplingSchedule` 接显式 now，next_due 由整轮完成（含 policy/logging）时间 + interval 排期；expected failure 也推进，无 catch-up burst/tight retry。
- current bundle 为唯一内存快照；queryResourceSnapshot 返回独立 `optional<ResourceSnapshotView>` 副本，不访问 Collector/SM/Monitor/queue。
  view 含 snapshot、CPU/Memory optional age 与 stale；无成功时间时 age absent，stale false 并保留原 quality。
  age 对倒退 query 时间夹到零，stale 用严格 age > 3×interval；bundle 的 process_scan_quality 不被空 rows 解释为退出。
- CPU/Memory unavailable 分别首次告警、连续失败每 30 秒至多一条、恢复一条日志；CPU warming baseline 不算新 read failure。
  process 非 observed/CPU unavailable 按周期聚合数量；overrun 每 30 秒至多一条。不产生 availability DeviceState 或恢复请求。
- post(shutdown)、worker 异常、writer 关闭入口和析构均关闭 atomic resource gate。
  stop、cache commit 和 private fact enqueue 共享短 submit mutex，使停止边界之后不能提交新快照/facts；writer 关闭后也拒绝资源 dispatch。
  停止前已排队 facts 仍按原 FIFO 处理，不能撤销旧队列。正常关闭保持最后 DeviceState，不伪造 OFFLINE/clear。
- worker 捕获 unexpected exception，保存 exception_ptr、记录 fatal 文意的 error 日志并请求既有 shutdown。
  run 在 worker join 后取回异常，在原 stop/grace/reap/signal restoration 清理完成后抛出。
  Collector/reader/cache/SM/Monitor 在 join 前存活；不 detach、不伪造不可中断 read 的退出保证。

## Statically Verified

环境使用已有 `python.exe` 的 `-B` 选项。两个旧 checker 文件均未修改，未新增 T4 的 checker。

| 检测 | 实际结果 |
| --- | --- |
| `python -B tests/phase3_validation_static_check.py` | 退出码 1；P2/P3 state/aggregation/IPC 子检查通过，汇总在旧 Monitor 表达式 oracle 失败，见迁移表 |
| `python -B tests/phase4_static_check.py` | 退出码 1；在 main 字节冻结失败，不宣告其完整通过 |
| T3 inline audit（PowerShell here-string → `python -B -`） | 退出码 0；源码结构、未迁移 owner/协议/DSM/六秒/注册、T3 接线与格式通过补充静态检查 |
| `git -c core.safecrlf=false diff --check` | 退出码 0；未跟踪 monitoring tests 与本报告另外核对末行/尾空白 |
| `tests/phase5_static_check.py` | 不存在，归属 T4，不在本任务 Allowed Files；未创建/运行 |

补充审计复用现有 `phase2_static_check.check_source/check_registration/masked_source/require`，
直接调用未修改的 P4 owner/bridge、aggregation、protocol、IPC 和 COVERAGE helper，
以及 P3 explicit-target/scenario helper。未对 checker 做文件修改或运行时 monkey patch。

核对结果仅为静态源码证据：

1. 43 个 C++ 文件本地 include/delimiter/literal/main wiring 通过；152 个 main-wired 函数；12 executable / 13 CTest 注册。
2. P4 owner/budget/token/cancel/terminal/bridge、DSM 441 target triples（24 allowed / 417 rejected）、IPC contracts 和 43 个 P4 evidence 函数通过对应 helper。
3. 除明确开放 main 外，P4 16 个冻结文件与其 baseline 完全一致；P5 protected event/aggregation/recovery 生产头源与 HEAD 一致。
   原六秒函数逐字一致，旧 tests/CMakeLists 为现有文件完整前缀；无 DISABLED/WILL_FAIL。
4. Monitor watch/unwatch/heartbeat/check，Runtime captured_recovery/cancel_closure/apply_change/drain_work/reap_children 与 HEAD 函数体逐字一致。
   handle_event 仅新增 shutting_down 资源 dispatch guard；原 stop/grace/reap/signal restoration 后缀逐字一致。
   runtime_core 所有其它测试函数含 FIFO adapter/observer cleanup 未修改。
5. main 逆向还原三处获批配置接线后与 HEAD 完全一致；一次完整 load、同 config.services 供 IPC。
6. 核对 native 的两 public ingress 路径/private adapter，copy-only query、strict stale、采样顺序/批量身份/gate、恰两 worker、pending coalesce 与 exception join cleanup。
7. 核对单 policy、整体 validation 先于状态、inclusive clear、change-only 和 sink 锁外；tracked scope 限已存在 T1/T2 + T3 Allowed Files。

静态输出节选：

```text
PASS (T3 static): 43 C++ sources; 152 main-wired functions; (12, 13) target/CTest entries
PASS (T3 static): RM/SM owner, budget/terminal/generation, DSM 441 triples, FIFO, IPC contracts; 43 P4 evidence functions
PASS (T3 static): 16 P4 frozen files excluding explicitly opened main; P5 protected production files; original six-second function/registrations
PASS (T3 static): heartbeat functions, recovery/lifecycle writer, stop/grace/reap suffix and other runtime_core oracles unchanged
PASS (T3 static): single policy, native ingress/private adapter, copy-only age/stale query, ordered sampling/atomic gates, two workers, coalescing/exception cleanup
PASS (T3 static): tracked scope and tracked/untracked formatting; no checker edits
LIMIT: supplemental source audit only; T3 C++ assertions are not executed; P3/P4 full checker failures remain recorded
```

最终补检脚本首次因工具脚本多写一个右括号产生 SyntaxError；修正脚本后重跑退出码 0，未改变检查条件或 checker 文件。

### 旧静态断言 → 批准语义 → T4 新 oracle

这些失败为实际失败，不笼统标 false positive。依据 CHANGE_LIST 及 thread3_task Dependencies，迁移属于 T4。

| 旧 oracle / 实际失败 | 获批语义 | 后续须保留的新 oracle |
| --- | --- | --- |
| P3 check_review_fixes：`thresholds_.memory_warning >= thresholds_.memory_critical`（首先失败）；之后还要求旧 `memory_percent >= thresholds_.memory_critical ? ...`、`memory.active = memory_percent >= thresholds_.memory_warning`、`resources_(std::move(memory))` | 构造 validation 移入 resolve_thresholds；legacy adapter 委托 typed 唯一 policy；Memory 迟滞/change-only | finite/激活及 clear 次序 validation、双输入先整体校验、typed validity、95/94.9/90/75 transition、private FIFO；保留该函数所有其它 owner/terminal/DSM/IPC/6s 检查 |
| P4 check_freeze：`frozen interface/backend changed: src/ipc/main.cpp` | main 仅一次完整加载、native 构造、同 services 供 IPC | CLI/sockets/sink 生命周期、一次 load/native/defaults/同 config.services；其余 16 frozen files、原六秒及全部 COVERAGE 不变 |

### Fixture 源码映射

下表为可审阅 assertions 的源码覆盖，不代表 C++ 行为 PASS。

| P5 IDs / 边界 | 本轮函数与证据 |
| --- | --- |
| 07..11 | `test_policy_hysteresis_change_only_metadata_and_direct_clear`：inclusive activate/clear、CPU 100 only warning、jitter、Memory 同 source downgrade/direct clear、metadata |
| 03、17、34 | `test_policy_partial_validity_invalid_latch_and_atomic_input_validation`：首次 CPU warming、部分 validity、重复 invalid 保 latch、typed 缺值/NaN、双输入无 partial policy/publication |
| 26 | `test_policy_legacy_derived_clear_explicit_validation_and_unlocked_sink`：三值 derived clears/低阈值/显式次序及 nonfinite/reentrant sink；T2 原配置函数保留 |
| 24 | `test_sampling_deadlines_boundaries_backwards_and_slow_completion`：首次 due、相等/倒退/0 elapsed、完成后排期、100 ticks 只一个 pending，consume 后重新准入 |
| 18、26、27 | `test_runtime_modes_reserved_sources_envelope_and_pre_run_fifo`：early shutdown 零 reads、reserved 两 source/两 public 重载/异类 envelope、独立其它 source、完整 config precedence、legacy override/run 前 FIFO |
| 03、17、18、28、34 | `test_native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown`：五轮 controlled samples、unknown/error retention、valid recovery、CPU warning 与 Memory clear、clear 到 READY、strict stale/倒退 age、并发独立 copy 零 I/O、在途第六轮 shutdown 丢弃 cache/facts |
| 18、35 | `test_native_unexpected_worker_exception_shutdown_and_reader_lifetime`：reader unexpected exception → shutdown/cleanup/异常传回、无 fake cache、capture 存活到 join/Runtime 析构后释放 |
| 26、27 / 旧回归 | `test_resource_monitor_thresholds` 与未修改的 `test_runtime_resource_fact_adapter`：旧 static override/80/95/invalid/FIFO 保留并迁移明确 state-change 数量 |

新的 runtime gate fixture 都有 release-before-join guard；等待有 deadline。fake clock 的 cycle completion 同步避免 query 见到 cache 后提前推进下一轮时钟；资源值来自 Reader fixture，不依赖真实系统压力。

## Not Verified / T4 handoff

本报告只认证上述静态结构、guards、保护范围与 assertion 接线，不能把 152 个函数存在解释为执行通过。
T3 supplemental audit 不能替代 P5 完整 checker。P3/P4 两个完整 checker 的迁移失败仍待 T4 处理。

后续 T4 须在自己的 Allowed Files 内迁移上表两个 oracle、新增 phase5_static_check，并补齐 36 项整体映射、native process_scan unavailable/query 组合、signal/observer 与慢周期/关闭组合、资源不触发 recovery 的完整回归。
生产采样 seam/未知/身份/迟滞/停止/快照接线已交付，不宣告完整 Phase5 DONE。
