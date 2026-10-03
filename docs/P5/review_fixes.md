# T4 Review Fixes / Static Oracle Migration

日期：2026-10-03（Asia/Shanghai）。基线：`4843061fce2a329832105d34aaea380a35560c0e`。
按用户要求只做静态检测；下列诊断和路径审查不作为行为测试运行结果。

## F01 — P3 Monitor oracle 与批准的迟滞实现冲突

实际首轮 `python -B tests/phase3_validation_static_check.py` exit 1：

```text
FAIL: Thread4: resource producer wiring missing: thresholds_.memory_warning >= thresholds_.memory_critical
```

旧断言绑定了构造成员阈值表达式、逐样本 memory_percent→severity/active 和直接发送 memory。
CHANGE_LIST/THRESHOLD_POLICY 批准 resolve_thresholds、typed 唯一 policy、94.9 保 critical、change-only；
所以这些旧表达式必须迁移，不能笼统称 false positive 后忽略失败。

| 旧 oracle | 批准语义 | 新 oracle |
| --- | --- | --- |
| `thresholds_.memory_warning >= thresholds_.memory_critical` | validation 移至 resolve_thresholds | finite 与 `thresholds.memory_warning >= thresholds.memory_critical` 保留；P5 检查 global clear 顺序 |
| `memory_percent >= thresholds_.memory_critical ? ...` | typed valid value、依赖当前 latch 迟滞 | typed validity、CPU clear、memory direct clear/current critical/critical clear/activate |
| `memory.active = memory_percent >= thresholds_.memory_warning` | stable 不重复、normal active=false | `previous == next` return、`fact.active = next != Pressure::normal`、severity 映射 |
| `resources_(std::move(memory))` | legacy adapter→typed policy→锁外事实列表 publish | `observeResources(snapshot)`、resource mutex、`resources_(std::move(fact))` |
| 原测试逐样本数目/94.9降级 | T3 已批准迁移；本轮不改其源码 | 静态检查 80/95/94.9/90/75、数量/critical/NaN/三值 overrides；其他旧 test body 与 HEAD 原文比较 |

只修改 `check_review_fixes` 中 Monitor oracle；前后全部文件内容由 P5 checker 对 HEAD 比较。
generation/RM terminal/DSM/IPC/六秒及全部 suite 调用均不删、不移除。
最终 S1/S2/S3 exit 0，完整静态日志见 [static_validation.log](static_validation.log)。

## F02 — P4 main 字节冻结与批准的 native 启动接线冲突

实际首轮 `python -B tests/phase4_static_check.py` exit 1：

```text
FAIL (static): frozen interface/backend changed: src/ipc/main.cpp
```

CHANGE_LIST 明确只开放 main 启动接线。原 main 重复 load_file，P5 改为一次完整 RuntimeConfig，
Runtime 默认 native，IPC 使用同 config.services。不能把 main 整体从保护范围消失。

最小迁移：从 P4 PROTECTED 的逐字集合移除 main，立即执行 `check_native_startup()`。
检查一次 load_runtime_file、Runtime(config)、同 services，再逆向还原这三处编辑，与 P4 baseline 完整字节比较。
CLI、socket 参数、event/query lambdas、device_changes weak sink 生命周期、start/run/stop 和异常返回仍被冻结。
另外 16 PROTECTED 文件、原六秒/PID/reap、COVERAGE 的 43 个 P4 证据函数及 owner 检查保持。
P5 checker 还逆向还原 checker 的该迁移并与 HEAD 全文比较，防止顺便弱化其他 oracle。

最终 S2/S3 exit 0，没有协议或后端修改。

## F03 — 补充组合覆盖，没有生产修复

审查发现现有测试分散于 Collector fixtures、Monitor policy 和 raw aggregation，
缺少若干明确的 policy/FIFO/身份/query/关闭组合源码。本轮增加七个主入口调用函数：

| 文件/函数 | 可观察 oracle |
| --- | --- |
| EA `test_p5_policy_fifo_downgrade_and_clear_reaggregate_all_health` | 94.9 stable、90 单事实降级无中间 RUNNING、多 source/critical/optional fault/RECOVERING 保留 |
| EA `test_p5_policy_clear_to_ready_and_offline_remains_terminal` | 95→75 direct clear、READY、OFFLINE terminal 不解锁、三次事实数目 |
| RC `test_p5_resource_pressure_does_not_restart_running_service` | healthy child 的 PID/generation/launched_generation 不变、restart_count=0、launch 文件=1、无 recovery/terminal、正常 reap |
| P2 `test_p5_native_default_reader_system_process_and_shutdown` | 新构造默认 native、真实 default Reader、CPU/Memory bounds、process token/RSS/CPU/current completion、join 后缓存和 ECHILD |
| Resource `test_native_process_not_present_is_measurement_only_and_shutdown_discards_row` | 真 child + 注入 ENOENT，行 absent 值且不改生命周期；第二次 process I/O gate 时 shutdown，旧 cache/health 保持、零预算、ECHILD |
| Resource `test_native_identity_change_query_unavailable_and_signal_shutdown` | 锁外 I/O gate 中 writer stop/reap，批量重验证拒迟到 row；system 独立 valid、query scan unavailable/empty；native SIGTERM/join/handler restore |
| Resource `test_native_observer_failure_stops_sampling_and_restores_signal_handler` | native observer throw，停止/join/服务清理，invalid Reader 不伪 good、恢复原 handler |

EA=event_aggregation；RC=recovery_coordination；P2=phase2_integration；Resource=resource_monitoring。
等待均有 deadline，reader gates 有 release-before-join guard；读取 worker 结果的非原子数据在 join 后。
查询缓存不推进 policy 或 I/O；真实资源测试只断言合法性与身份，不假设固定机器压力。
native observer fixture 允许初始化之前定时 tick 已提交 unavailable 测量，避免依赖线程调度速度。

未发现需要修改 T1–T3 生产文件的具体缺陷，未借 review 重构 ownership 或扩大接口。
原六秒函数和所有旧测试原文保留；测试新增只证明 assertion 源码存在，不能称行为 PASS。

## P5 Checker 开发诊断与保留边界

首轮新 checker 将 constructor 的 `reader_` member initializer 与三个真实读取调用一起计数，
误判 bounded reader scope；修正为明确区分三次读取与一次初始化。之后在报告尚未生成时准确报 missing validation_review。
首次矩阵审计也将仅调用 expect_percent 的测试误判为没有断言；现明确核对该 helper 的 valid/value/finite/误差比较、expect_unavailable 的 absent 断言以及 rejects 的捕获断言后计入覆盖，保留全部 36 行。
未修改生产代码去迎合这两项工具诊断，也未删除 scope/证据文件检查。

新增 P5 checker 使用固定 HEAD 保护 event/aggregation/recovery/IPC/backend/history，
SM 去除两个新方法后与原生命周期全文比较、heartbeat 四函数全文比较、Runtime recovery writer/原关闭后缀全文比较，
检查 source/guards/测试矩阵和旧 oracles 的批准范围。C++ 语义、并发与性能边界见 [validation_review.md](validation_review.md)。

本轮静态范围完成；不覆盖 P0–P4 历史日志，不生成编译测试报告。
