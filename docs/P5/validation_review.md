# T4 — Static Validation / Architecture Review

日期：2026-10-03（Asia/Shanghai）。实际 HEAD：`4843061fce2a329832105d34aaea380a35560c0e`。
对象是包含 T1–T3 和本次 T4 的当前工作树。进入时 T1–T3 文件已在同一工作区集成；没有用 HEAD 的旧源码替代当前实现。

本次按用户明确指令只执行静态检测；任务包的编译测试要求不作为交付条件，也不生成编译测试报告。
设计文档保留其冻结时点，本报告给出当前实现的静态审查结论。未提交代码、创建 PR、安装依赖或修改历史证据。

## Implemented

T1–T3 已提供 Linux system/process Collector、typed snapshots、全局配置、只读 SM 身份接口、
Monitor 迟滞及 native/external Runtime 接线。本次审查新增七个断言测试函数、P5 checker 和以下三个证据文件。

| 本轮文件 | 交付 |
| --- | --- |
| `tests/event_aggregation_tests.cpp` | 两个 policy→FIFO→aggregation 测试：单事实降级、clear 后保留其他 source/service fault、READY/RECOVERING/OFFLINE |
| `tests/recovery_coordination_tests.cpp` | 真实健康 child 上报压力后 PID/token/generation/预算投影不变、launch 文件计数为一、无 RECOVERING/OFFLINE |
| `tests/phase2_integration_tests.cpp` | 新 RuntimeConfig 默认 native/default Reader 测试：系统与直接 child 的 CPU/RSS/身份、join 后缓存、独占 reap；原六秒函数不改 |
| `tests/resource_monitoring_tests.cpp` | 三个 native 组合：process ENOENT 只测量、在途 process stop 丢弃；身份变化的 unavailable scan/query 与 signal；observer 异常清理 |
| `tests/phase3_validation_static_check.py` | 仅替换旧 Monitor producer oracle 为 typed/validity/迟滞/change-only/旧阈值测试保护 |
| `tests/phase4_static_check.py` | 仅开放 main 的三处启动接线，逆向还原后与原文件字节比较；其余保护和全部 COVERAGE 原样保留 |
| `tests/phase5_static_check.py` | 36 ID/24 验收映射、源码格式/接线、保护文件/历史、旧测试函数、ownership/身份/单位/停止/模式/查询/调度结构 |
| `docs/P5/validation_review.md`、`review_fixes.md`、`static_validation.log` | 当前静态证据、精确 oracle 迁移及边界 |

没有修改 T1–T3 生产代码、CMake 注册或旧测试目标。保留进入时已有的 `AGENTS.md`、`docs/P5/`、`temp.log` 和所有实现改动。
完整相关规则/设计、三份交接报告、P3/P4 review/fixes、两个旧 checker 和当前生产路径均纳入审查。

## Statically Verified

环境为 Windows / PowerShell，使用已有 `python` 的 `-B` 和标准库；`python3` 的同等替代不增加依赖。
完整 stdout/stderr、Python/平台版本、命令与退出码见 [static_validation.log](static_validation.log)。

| 标签 | 命令 | 最终退出码 | 静态含义 |
| --- | --- | --- | --- |
| S1 | `python -B tests/phase3_validation_static_check.py` | 0 | P2 源码/注册，P3 state/aggregation/IPC，441 triples、11 个原修复函数及迁移后的资源 producer |
| S2 | `python -B tests/phase4_static_check.py` | 0 | 16 frozen files + main startup oracle，原六秒/PID/reap、RM owner/budget/bridge/terminal/cancel，43 个 P4 证据函数 |
| S3 | `python -B tests/phase5_static_check.py` | 0 | 全部 P5 guards 与 36 项函数映射、24 验收行、P5 protected/history/旧测试函数保护 |
| S4 | `git -c core.safecrlf=false diff --check` | 0 | tracked diff 无空白错误；untracked 新源码/报告另由 S3 扫描 |

首次 S1 exit 1：`resource producer wiring missing: thresholds_.memory_warning >= thresholds_.memory_critical`。
首次 S2 exit 1：`frozen interface/backend changed: src/ipc/main.cpp`。两项已按 CHANGE_LIST 精确迁移，详见 [review_fixes.md](review_fixes.md)。
P5 checker 开发时修正了 reader_ 初始化被误计为 I/O 的计数 oracle；报告未生成时曾明确报 missing file。
首次矩阵审计将 Collector 的 expect_percent helper 误判为没有断言；补充对该 helper 的 valid/has_value/finite/误差比较和 rejects helper 的捕获断言检查后通过，未删除矩阵行或修改被测源码。
这些是静态工具诊断，不是 C++ 行为失败。最终检查无遗留静态失败。

当前源码清单：43 个 C++ 文件、159 个 main 接线测试函数、12 个 executable / 13 个 CTest 注册。
原 10 executable / 11 CTest 及独立 60 秒 timeout 的六秒回归完整保留；没有 DISABLED/WILL_FAIL。
新 checker 不把函数名视为充分证据：检查函数体中的 require，验证 aggregation.expect 的状态 oracle，并核对 main 调用。
36 项映射涉及 61 个有断言的函数；27 个冻结文件和 73 个历史文档/日志保持原文。
同时比较所有旧测试函数原文，唯一例外是 CHANGE_LIST 已批准由 T3 迁移的 threshold 函数。

## Architecture Review

| 边界 | 源码证据与判断 |
| --- | --- |
| Collector 测量 | 默认 rb bounded Reader 仅三种 proc 路径；system cap 64KiB/process cap 4KiB；不依赖 event/DSM/RM，不 wait/kill/reap、不创建线程 |
| CPU/Memory | aggregate 前八字段、guest 不重复、idle+iowait 非忙；field shape/regression/sum/delta guards、首次 absent；Memory 只用两个唯一 kB 字段，无 MemFree fallback |
| 实例身份 | SM 一次 try-lock capture、一次 hash lookup 重验证；使用 launched_generation；proc I/O 位于两次调用之间；starttime anchor 不随 read failure/mismatch 丢失；未验证 pending 不提交 |
| Policy | Monitor 单 owner，两个输入整体 validation 后才进入 resource mutex；invalid/warming 不推进；CPU only warning；Memory 95/94.9/90/75；锁外 publish，change-only |
| 事实链 | 内置 private queue adapter→原 FIFO/writer→dispatcher→aggregation→DSM；downgrade 一条 active warning；clear 只本 source，原全量 evaluate；未改 DSM 表和 event 枚举 |
| Recovery | resource branch 不构造 RecoveryRequest、不调用 launch；P4 writer/recovery 函数与 HEAD 一致；RM policy/五预算/token/terminal 和 SM lifecycle 都保留 |
| 模式/配置 | 旧 path external 与新默认 native 互斥；两 public post 重载及异类 envelope 都防 reserved source；private sink 绕过 public guard；main 一次完整 load，IPC 同 services |
| 调度 | 两个既有 worker；先真实时间 heartbeat，再资源 due；最多一个 pending tick；完成后 interval 排期；expected failure 仍推进，无 catch-up burst |
| Cache/query | 唯一 current bundle，短锁副本；query 不 I/O、不访问 SM/policy/queue；age 倒退夹零，严格 >3interval stale；unavailable scan 空 rows 不等同全部退出 |
| 关闭/异常 | atomic stop gate；submit mutex 排序 stop/cache/private facts；逐 process/发布前 gate；worker catch 请求原 shutdown，join 后传回原异常；原 grace/reap/signal 后缀逐字保留 |
| 范围/兼容 | protected 生产文件、P0–P4 历史文件、heartbeat 四函数、原 ServiceConfig/JSON parser 和旧测试原文保护；UDS10字节/64KiB/types1..10/255不改，无 thermal/disk/IPC扩展/DB/新依赖 |

P5-30 的证据包括 main 的默认 native 接线、库注册、真实 default Reader 的新测试源码；
原 IPC integration main 仍 fork/exec 生产 executable。没有数值查询 IPC，所以外部 socket 不提供 CPU/RSS 数字。
这些静态路径支持生产接线结论，不能据此声称 executable 已在 Linux 实际采集成功。

## P5-01..36 Source Evidence

以下每行使用 S3（exit 0）检查函数定义、可观察断言及 main 接线；S1/S2（exit 0）补充旧阶段保护，S4（exit 0）补充 tracked 格式。
`Implemented + Statically Verified (source)` 只表示源码与结构。表中函数不表示行为测试 PASS。
完整、机器可核对映射在 `tests/phase5_static_check.py:COVERAGE`。

| ID | Assertion source (test_ prefix included) | Status |
| --- | --- | --- |
| P5-01 | `resource_collector_tests.cpp::test_cpu_optional_fields_and_guest_exclusion`; `resource_collector_tests.cpp::test_cpu_each_component_busy_definition` | Implemented + Statically Verified (source; S3=0) |
| P5-02 | `resource_collector_tests.cpp::test_first_sample_and_aggregate_delta`; `resource_collector_tests.cpp::test_cpu_zero_full_and_large_valid_delta` | Implemented + Statically Verified (source; S3=0) |
| P5-03 | `resource_collector_tests.cpp::test_first_sample_and_aggregate_delta`; `resource_monitoring_tests.cpp::test_policy_partial_validity_invalid_latch_and_atomic_input_validation` | Implemented + Statically Verified (source; S3=0) |
| P5-04 | `resource_collector_tests.cpp::test_each_cpu_counter_regression_and_rebaseline`; `resource_collector_tests.cpp::test_cpu_zero_delta_shape_change_and_wrap`; `resource_collector_tests.cpp::test_invalid_cpu_text_clears_baseline` | Implemented + Statically Verified (source; S3=0) |
| P5-05 | `resource_collector_tests.cpp::test_invalid_memory_text_and_no_free_fallback` | Implemented + Statically Verified (source; S3=0) |
| P5-06 | `resource_collector_tests.cpp::test_memory_available_formula_order_whitespace_and_boundaries` | Implemented + Statically Verified (source; S3=0) |
| P5-07 | `resource_monitoring_tests.cpp::test_policy_hysteresis_change_only_metadata_and_direct_clear` | Implemented + Statically Verified (source; S3=0) |
| P5-08 | `resource_monitoring_tests.cpp::test_policy_hysteresis_change_only_metadata_and_direct_clear` | Implemented + Statically Verified (source; S3=0) |
| P5-09 | `resource_monitoring_tests.cpp::test_policy_hysteresis_change_only_metadata_and_direct_clear`; `event_aggregation_tests.cpp::test_p5_policy_fifo_downgrade_and_clear_reaggregate_all_health` | Implemented + Statically Verified (source; S3=0) |
| P5-10 | `resource_monitoring_tests.cpp::test_policy_hysteresis_change_only_metadata_and_direct_clear` | Implemented + Statically Verified (source; S3=0) |
| P5-11 | `resource_monitoring_tests.cpp::test_policy_hysteresis_change_only_metadata_and_direct_clear` | Implemented + Statically Verified (source; S3=0) |
| P5-12 | `event_aggregation_tests.cpp::test_p5_policy_fifo_downgrade_and_clear_reaggregate_all_health`; `recovery_coordination_tests.cpp::test_p5_resource_pressure_does_not_restart_running_service` | Implemented + Statically Verified (source; S3=0) |
| P5-13 | `event_aggregation_tests.cpp::test_resource_critical_downgrade_and_clear`; `event_aggregation_tests.cpp::test_p5_policy_fifo_downgrade_and_clear_reaggregate_all_health` | Implemented + Statically Verified (source; S3=0) |
| P5-14 | `event_aggregation_tests.cpp::test_p5_policy_fifo_downgrade_and_clear_reaggregate_all_health` | Implemented + Statically Verified (source; S3=0) |
| P5-15 | `resource_collector_tests.cpp::test_process_read_failures_preserve_anchor_and_clear_cpu`; `resource_monitoring_tests.cpp::test_native_process_not_present_is_measurement_only_and_shutdown_discards_row` | Implemented + Statically Verified (source; S3=0) |
| P5-16 | `resource_collector_tests.cpp::test_process_reused_pid_token_and_repeated_starttime_mismatch`; `resource_collector_tests.cpp::test_process_validation_rejects_missing_duplicate_pid_and_anchor_commit`; `resource_monitoring_tests.cpp::test_sm_proc_io_outside_lock_and_post_capture_launch_change` | Implemented + Statically Verified (source; S3=0) |
| P5-17 | `resource_collector_tests.cpp::test_read_errors_partial_validity_and_recovery_metadata`; `resource_collector_tests.cpp::test_reader_size_limit_exact_and_oversize`; `resource_monitoring_tests.cpp::test_native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown` | Implemented + Statically Verified (source; S3=0) |
| P5-18 | `resource_monitoring_tests.cpp::test_native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown`; `resource_monitoring_tests.cpp::test_native_process_not_present_is_measurement_only_and_shutdown_discards_row`; `resource_monitoring_tests.cpp::test_native_observer_failure_stops_sampling_and_restores_signal_handler`; `recovery_coordination_tests.cpp::test_signal_and_observer_failure_during_recovery_cleanup` | Implemented + Statically Verified (source; S3=0) |
| P5-19 | `resource_collector_tests.cpp::test_process_batch_sort_partial_failure_completion_and_gate` | Implemented + Statically Verified (source; S3=0) |
| P5-20 | `recovery_coordination_tests.cpp::test_p5_resource_pressure_does_not_restart_running_service`; `recovery_coordination_tests.cpp::test_real_crash_candidate_success_and_reap`; `resource_monitoring_tests.cpp::test_native_process_not_present_is_measurement_only_and_shutdown_discards_row` | Implemented + Statically Verified (source; S3=0) |
| P5-21 | `resource_collector_tests.cpp::test_process_comm_fields_units_and_multicore_cpu`; `resource_collector_tests.cpp::test_process_zombie_and_malformed_stat`; `resource_collector_tests.cpp::test_process_states_read_limit_exceptions_and_post_validation_gate` | Implemented + Statically Verified (source; S3=0) |
| P5-22 | `resource_collector_tests.cpp::test_process_comm_fields_units_and_multicore_cpu`; `resource_collector_tests.cpp::test_process_actual_elapsed_long_gap_and_counter_regression`; `resource_collector_tests.cpp::test_process_platform_failure_isolated_from_system` | Implemented + Statically Verified (source; S3=0) |
| P5-23 | `resource_monitoring_tests.cpp::test_sm_try_lock_busy_skips_io_and_recovers_after_launch`; `resource_collector_tests.cpp::test_process_validation_busy_mismatch_and_unavailable_capture` | Implemented + Statically Verified (source; S3=0) |
| P5-24 | `resource_monitoring_tests.cpp::test_sampling_deadlines_boundaries_backwards_and_slow_completion`; `resource_collector_tests.cpp::test_process_actual_elapsed_long_gap_and_counter_regression` | Implemented + Statically Verified (source; S3=0) |
| P5-25 | `resource_monitoring_tests.cpp::test_config_legacy_roots_defaults_wrapper_and_service_fields`; `resource_monitoring_tests.cpp::test_config_seven_fields_and_partial_fixed_defaults`; `resource_monitoring_tests.cpp::test_config_json_boundaries_and_threshold_order`; `resource_monitoring_tests.cpp::test_config_wrong_types_unknown_duplicate_float_and_overflow`; `resource_monitoring_tests.cpp::test_config_preserves_service_validation_and_file_limit` | Implemented + Statically Verified (source; S3=0) |
| P5-26 | `resource_monitoring_tests.cpp::test_policy_legacy_derived_clear_explicit_validation_and_unlocked_sink`; `resource_monitoring_tests.cpp::test_config_programmatic_fractional_finite_and_legacy_thresholds`; `runtime_core_tests.cpp::test_resource_monitor_thresholds`; `resource_monitoring_tests.cpp::test_runtime_modes_reserved_sources_envelope_and_pre_run_fifo` | Implemented + Statically Verified (source; S3=0) |
| P5-27 | `resource_monitoring_tests.cpp::test_runtime_modes_reserved_sources_envelope_and_pre_run_fifo`; `runtime_core_tests.cpp::test_runtime_resource_fact_adapter` | Implemented + Statically Verified (source; S3=0) |
| P5-28 | `resource_monitoring_tests.cpp::test_native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown`; `resource_collector_tests.cpp::test_process_validation_busy_mismatch_and_unavailable_capture`; `resource_monitoring_tests.cpp::test_native_identity_change_query_unavailable_and_signal_shutdown` | Implemented + Statically Verified (source; S3=0) |
| P5-29 | `event_aggregation_tests.cpp::test_p5_policy_clear_to_ready_and_offline_remains_terminal`; `event_aggregation_tests.cpp::test_p5_policy_fifo_downgrade_and_clear_reaggregate_all_health` | Implemented + Statically Verified (source; S3=0) |
| P5-30 | `phase2_integration_tests.cpp::test_p5_native_default_reader_system_process_and_shutdown` | Implemented + Statically Verified (source; S3=0) |
| P5-31 | `device_ipc_tests.cpp::test_running_health_requires_confirmed_heartbeats`; `device_ipc_tests.cpp::test_subscriptions_and_burst_order`; `ipc_integration_tests.cpp::test_ipc_extension_boundaries` | Implemented + Statically Verified (source; S3=0) |
| P5-32 | `phase2_integration_tests.cpp::test_runtime_shutdown_respects_long_grace_period`; `recovery_manager_tests.cpp::test_policy_lifetime_success_cancel_and_manual_budget`; `recovery_coordination_tests.cpp::test_offline_cancels_other_task_and_leaves_no_recovering_service` | Implemented + Statically Verified (source; S3=0) |
| P5-33 | `resource_collector_tests.cpp::test_unexpected_reader_exception_reaches_caller_boundary`; `resource_monitoring_tests.cpp::test_sm_identities_launch_token_cleanup_and_read_only_validation` | Implemented + Statically Verified (source; S3=0) |
| P5-34 | `resource_monitoring_tests.cpp::test_policy_partial_validity_invalid_latch_and_atomic_input_validation`; `resource_monitoring_tests.cpp::test_native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown`; `resource_collector_tests.cpp::test_first_read_failure_has_no_success_or_value` | Implemented + Statically Verified (source; S3=0) |
| P5-35 | `resource_monitoring_tests.cpp::test_native_unexpected_worker_exception_shutdown_and_reader_lifetime` | Implemented + Statically Verified (source; S3=0) |
| P5-36 | `recovery_dependency_tests.cpp::test_invalid_graphs_have_no_side_effects`; `device_ipc_tests.cpp::test_optional_device_callback_and_sink_lifetime`; `device_ipc_tests.cpp::test_producer_overflow_disconnect_and_resubscribe`; `device_ipc_tests.cpp::test_unread_client_output_bound`; `phase2_integration_tests.cpp::test_p5_native_default_reader_system_process_and_shutdown` | Implemented + Statically Verified (source; S3=0) |

P5-18/20/23/30/32 的关键门已提供可审阅断言与结构保护；不将静态检测冒充关闭竞态、恢复计数或真实采集的行为证据。
P5-19 的 N=1/8/32、2+N reads、batch validator、排序/partial failure 为 fixture assertions；未测 p95/p99/CPU 开销。

## 24 Acceptance Mappings

| # | P5 IDs | 当前判断 |
| --- | --- | --- |
| 1 | 01,30 | 固定 aggregate proc 路径与默认 native 接线静态支持 |
| 2 | 02,04 | 八 counter/overflow/delta/rebaseline 与 75% fixture oracle 静态支持 |
| 3 | 03 | 首次 absent/warming、独立 Memory policy 静态支持 |
| 4 | 05,30 | 固定 meminfo 读取路径与默认 Reader 静态支持 |
| 5 | 05,06 | MemAvailable 公式、非法输入 absent/no fallback 静态支持 |
| 6 | 20,33 | 测量/Monitor/DSM 分层，protected owners 静态支持 |
| 7 | 07,10,12 | CPU/Memory warning 与设备 WARNING 组合断言源码存在 |
| 8 | 08,13 | Memory critical 覆盖 service recovery 的断言源码存在 |
| 9 | 09,10,34 | 迟滞/同 source downgrade/clear/invalid retention 静态支持 |
| 10 | 14,29 | 多 source/READY/RECOVERING/OFFLINE 全量重聚合断言源码存在 |
| 11 | 14 | control/optional fault 不被 clear 擦除的断言源码存在 |
| 12 | 20 | resource branch 无 request；真实 child PID/token/count/launch-file 断言源码存在 |
| 13 | 20,32,33 | RM policy reader/唯一 reservation owner、五次上限保护 |
| 14 | 16,23,32 | SM 原 lifecycle 全文还原比较、两批量 try 接口保护 |
| 15 | 19,24,33 | 两个既有 worker、coalesce/完成后 due、无新采样线程 |
| 16 | 15,16,21 | PID/token/starttime 与 pending 重验证提交保护 |
| 17 | 17,34 | 独立 quality/invalid 不推进 latch 与恢复断言源码存在 |
| 18 | 18,35 | process/system 在途 gate、join/lifetime/异常/signal 断言源码存在 |
| 19 | 01–11,15–17,33 | reader/clock/platform fixtures、不写真实 procfs 静态支持 |
| 20 | 26,27,32,36 | legacy constructors/wrapper/三值布局、heartbeat/旧测试保护 |
| 21 | 31 | protocol frozen、GET_HEALTH/subscription/overflow 原 oracles 保留 |
| 22 | 33 | 无 scope 膨胀、history/DB/GPU/thermal/disk/新IPC 实现 |
| 23 | 33,36 | T1–T3 交接、CHANGE_LIST、36 行与本轮文件范围对应 |
| 24 | T4 | 实现/静态/Not Verified 分开；按用户要求省略编译测试报告 |

## Not Verified

源码与文本 guards 不能证明实际 procfs/BSP 字段、进程时序、signal/observer 竞态、socket 流量、
错误日志节流行为、N=1/8/32 性能和实际设备行为。所有这些保留未验证状态，不由函数存在或静态退出码推出完成验收。

特别保留以下设计边界：

- 共享 Monitor worker 的慢 I/O 可延迟 heartbeat；内核不可中断 read/join 无硬实时保证。
- 初次 starttime anchor 依赖受管前台直接 child 和独占 reap；不支持外部 reaper/daemonizing。
- source publication/内存快照/writer 状态非事务；queue FIFO 不按 sampled_at 重排；文件不是原子整机快照。
- SM try 接口避免等其 launch 锁，未缩短既有同步 exec；process rows 排序 O(N log N)，身份匹配与缓存 O(N)。
- Logger 仍在成员构造时创建原有线程；graph 验证在 lifecycle/monitor/timer/recovery worker 启动前，不能描述为任何线程之前。
- 原 global queue 容量与 socket 会话身份风险延续；change-only/coalesce 不等于解决全部背压。
- Native signal test 覆盖已完成 unavailable scan 后的 signal/join；在途 read 的停止边界由显式 shutdown gate 测试，不声称精确 signal/read 交错已经实证。

本次 T4 的静态范围已完成，没有遗留相关静态失败；不宣布完整 Phase5 或 RK3588 行为验收完成。
