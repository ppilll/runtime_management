# T2 implementation — Process Monitoring & Global Configuration

日期：2026-10-03（Asia/Shanghai）。入口：`codex_package/thread2_prompt.md` → `thread2_task.md`。
按本次用户指令只记录实现与静态检测，不增加编译测试要求或对应报告。

已读取根/P5 AGENTS、P5 README/CHANGE_LIST、package README、DESIGN_FREEZE_CANDIDATE、
PROCESS_MONITORING、CONFIGURATION、RESOURCE_MODEL、SAMPLING_AND_FAILURE、TEST_PLAN 和 T1 报告；
核对实际 SM/Config/Collector、P2 process lifecycle/backend 独占回收契约、P4 SERVICE_INTEGRATION/RECOVERY_GENERATION。
实际 HEAD 为 `4843061fce2a329832105d34aaea380a35560c0e`，与设计基线一致。

开始时已有 T1 的 Collector/model/test 与两个 CMake 改动，以及未跟踪的 AGENTS、docs/P5、temp.log。
本任务保留这些内容，不修改根 CMake，不覆盖 T1 报告或任何旧阶段证据。

## Implemented

修改限定为 T2 Allowed Files，共十个源码/注册文件和本报告：

| 路径 | 本轮交付 |
| --- | --- |
| `include/runtime/resource_snapshot.hpp` | 增加最小 `ProcessResourceScan{quality,processes}`，已有 process row/identity 字段含义保持 |
| `include/runtime/resource_collector.hpp` | `ProcessPlatform` HZ/pagesize seam、mandatory 批量身份 validator、可选停止 gate、4KiB process cap、私有 launch anchor/CPU previous |
| `src/monitor/resource_collector.cpp` | 只读 process stat parser、CPU/RSS、逐行完成时间、暂存/重验证/提交、当前 eligible cache 淘汰 |
| `include/runtime/config_manager.hpp` | `MonitoringConfig` 七字段、`RuntimeConfig{services,monitoring}`、完整 loader 与 monitoring validation |
| `src/config/config_manager.cpp` | 全局整数 JSON 解析、固定默认值后整体验证、未知 monitoring key 拒绝、旧 `load_file` wrapper |
| `include/runtime/service_manager.hpp` | 两个 const 批量 try 只读身份接口 |
| `src/service/service_manager.cpp` | 一次 try-lock 复制 eligible 身份；一次 try-lock/hash lookup 批量重验证；不修改 registry/lifecycle |
| `tests/resource_collector_tests.cpp` | 增加十个进程 fixture 函数；T1 的十四个系统函数保留 |
| `tests/resource_monitoring_tests.cpp` | 新增六个配置、三个 SM 身份/锁忙 fixture 函数 |
| `tests/CMakeLists.txt` | 新 `resource_monitoring_tests` / `phase5_resource_monitoring_unit` 及 20 秒 timeout；所有旧注册/属性和 T1 注册保留 |

Collector 只测量受管直接子进程，不调用 wait/kill/reap，不产生健康事实或恢复请求。
SM 仍是 PID/lifecycle/launched_generation owner；保存的 identity token 是缓存的 owner 输出，不是新 token 发放者。
Monitor/Runtime/main/IPC、RM/DSM/ProcessSupervisor/event/Logger 和原六秒回归函数均未修改。

### Process semantics

- 只从捕获的正 PID 构造 `/proc/PID/stat`，每文件最多 4KiB。以最后一个右括号定位 comm 末尾，按 field14/15/22/24 取 own CPU/starttime/RSS；子进程 CPU 字段不累加。
- 拒绝 PID mismatch、不足字段、非法整数、负 CPU/starttime/RSS、CPU sum 与 RSS signed-pages/乘法溢出。`Z` 是 zombie，CPU/RSS absent；其它合法字母 state 仅为观测，包含 Linux 的小写 `t`/`x`，不推导服务故障。
- 默认构造时各读取一次 `_SC_CLK_TCK` / `_SC_PAGESIZE`；可注入不同单位或失败值。非正值让 process scan unavailable、rows 为空，不影响 system 采集。
- 每个 row 在读取/解析完成后调用 `Now`。CPU 使用 own ticks delta / HZ / 实际 elapsed，允许大于 100%；首次 warming_up 但 RSS 可有效。elapsed<=0 或 ticks 回退不生成 CPU 值，以合法当前 counters 重建 baseline；超过 3×interval 则重新 warming_up，恰等于边界仍可计算。
- ENOENT/ESRCH 通过 Reader 的 `not_present` 分类表达；权限/解析/超限为 unavailable。失效 row 不继承 CPU/RSS。read error、zombie、同 launch starttime mismatch 清 CPU previous，但保留已验证 starttime anchor；重复 mismatch 始终 identity_changed，不在下一轮接受复用 PID。
- scan 将新 rows/eligible-only cache 暂存。owner 验证锁忙、数量缺失、name/PID/token 不匹配、重复返回 identity 或停止 gate 关闭时，丢弃全部 rows 和 pending cache；旧 cache 不被未验证结果污染。成功后只保留当前 eligible launch，并按 service_name 排序。

### Configuration semantics

`MonitoringConfig.sample_interval_seconds` 的 C++ 类型是 `std::chrono::seconds`，默认 2 秒、范围 1..60；
六个 percentage 的 C++ 类型是 double，默认依次 CPU 80/75、Memory 80/75/95/90。
JSON 七字段均以既有 `long long` 整数分支读取，bool/decimal/exponent/null/wrong type 不接受。
programmatic double 必须 finite，百分比均为 0..100。

完整验证：`cpu_clear < cpu_warning`；
`memory_clear < memory_warning <= memory_critical_clear < memory_critical`。
部分配置先采用各自固定默认值，再整体验证；不暗调 clear，冲突诊断包含字段名。
monitoring 必须为 object，其中未知 key 拒绝；原 root/service 未知字段处理保持。
原 single/services root、服务 timeout/recovery validation、重复 key/name、1MiB 限额保持。
旧 `load_file(path)` 委托 `load_runtime_file(path).services`，因此两入口都校验完整文件。
ServiceConfig 布局及旧 ResourceThresholds 三值顺序未变；derived clear 属 T3 的 Monitor 工作，本轮不修改该头/源。

## Statically Verified

环境提供 `python.exe`，无 `python3` 命令；使用同等 `-B` 选项运行现有 checker，不改任何 checker 源码。

| 检测 | 实际结果与静态证据 |
| --- | --- |
| `python -B tests/phase3_validation_static_check.py` | 退出码 0；43 个 C++ 文件、145 个 main-wired 函数、12 targets/13 CTest entries；DSM/aggregation/IPC 与 441 triples 等原 oracle 保留 |
| `python -B tests/phase4_static_check.py` | 退出码 0；17 frozen files、原六秒/PID/reap 源码与旧注册、单 policy owner/bridge/cancel/terminal/IPC、43 个 P4 证据函数保护通过 |
| T2 inline audit：PowerShell here-string → `python -B -` | 最终通过；十个 allowed 源码/注册文件的 include/delimiter/main/格式、owner/身份/提交 guards、SM/Config 基线恢复比较与注册范围核对通过 |
| `git diff --check`（最后一次使用 `git -c core.safecrlf=false diff --check`） | 退出码 0；新增未跟踪源文件和报告另做尾空白/末行检查 |

补充审计复用 `phase2_static_check.check_source/masked_source/check_registration/require`，
并通过 `git show HEAD:<path>` 作下列严格比较：

1. 从 SM 源移除本轮两个 try 方法、从头文件移除对应声明/include 后，分别与 HEAD 完全一致。现有 lifecycle/reap/recovery/预算没有改写。
2. Config 的原 integer Parser、service 解码、ServiceConfig validation/recoveryTimeout 与 HEAD 完全一致。
   完整 loader 仅更名并更换返回包装；还原这两处后与原 loader 完全一致。
3. try 方法各只有一次 try-lock、使用 launched_generation；无 process/monitor/logger/lifecycle 调用。
4. 检查最后右括号、固定 stat offsets、算术/elapsed/anchor guards、per-row Now、捕获 gate→reader→validator→唯一 cache commit 的源码顺序；sysconf 各仅一个构造调用。
5. 24 个 collector 与九个 monitoring 函数都由 main 调用；SM 锁忙 fixture 的 RAII guard 先 release 再 join；I/O 竞态 stat 字面量的 offsets 正确。
6. 旧 tests/CMakeLists 是当前文件的完整前缀；新目标有 timeout；tracked 改动限 T2 及原 T1 注册。没有 DISABLED/WILL_FAIL 或 checker 迁移。

补充审计首次在函数计数处将相对路径传给绝对路径比较的 helper，产生工具路径检查失败；
改为 `Path.cwd()/path` 后重新执行整套补充审计通过，没有改动 checker 或放宽 oracle。

最终输出节选，仅代表静态源码检查：

```text
PASS (static): 43 C++ files, 145 main-wired tests, 12 targets/13 CTest entries
PASS (static): 17 frozen files, original six-second/PID/reap regression and old registrations
PASS (T2 static): original SM header/lifecycle restored exactly
PASS (T2 static): integer parser, legacy service/recovery/loader unchanged
PASS (T2 static): staged owner revalidation/commit, single sysconf reads
PASS (T2 static): 24 collector + nine config/identity fixtures main-wired
PASS (T2 static): tracked scope T2 plus pre-existing T1 registration; no checker changes
LIMIT: source-only audit; fixture behavior is not certified
```

### Fixture source mapping

下表记录可审阅的 assertion 源码覆盖，不把函数存在或 main 接线写成行为 PASS。

| ID / T2 部分 | 函数或源证据 |
| --- | --- |
| P5-15 | `test_process_read_failures_preserve_anchor_and_clear_cpu`；not_present 不保留旧值、无 lifecycle 动作 |
| P5-16 | `test_process_reused_pid_token_and_repeated_starttime_mismatch`、`test_process_validation_busy_mismatch_and_unavailable_capture`、`test_process_validation_rejects_missing_duplicate_pid_and_anchor_commit`、`test_sm_proc_io_outside_lock_and_post_capture_launch_change` |
| P5-17 / process | `test_process_zombie_and_malformed_stat`、read failure/platform failure 函数；仅测量边界 |
| P5-19 / source | `test_process_batch_sort_partial_failure_completion_and_gate`；N=1/8/32、排序、2+N reads、单次 validator、逐行完成时间、局部失败隔离 |
| P5-21 | comm/fields/unit 函数、zombie/malformed 函数、states/read-limit 函数；signed RSS/乘法溢出和 PID mismatch |
| P5-22 | `test_process_comm_fields_units_and_multicore_cpu`、`test_process_actual_elapsed_long_gap_and_counter_regression`、`test_process_platform_failure_isolated_from_system` |
| P5-23 | `test_sm_try_lock_busy_skips_io_and_recovers_after_launch`；真实 registry lock 的 fake launch gate、零 process I/O、有限等待/release guard；validation busy 不提交 baseline 的独立 fixture |
| P5-25 | 六个 `test_config_*` 函数；旧 roots/defaults、七字段/partial/boundaries/order、wrong type/float/overflow/duplicate/unknown、programmatic finite、旧 service 校验/限额 |
| P5-26 / T2 | `test_config_legacy_roots_defaults_wrapper_and_service_fields`、`test_config_programmatic_fractional_finite_and_legacy_thresholds`；旧 wrapper 与三值 aggregate；derived clear/native precedence 留给 T3 |

## T3 interface handoff

1. 单 Monitor worker 持有 Collector；它是 single-caller 对象，没有自己的线程或 mutex。
   `ResourceCollector(Reader, Now, optional<ProcessPlatform>)` 保留前两个默认参数的旧调用方式。
   默认 Reader 仍为原 bounded read-only proc reader，unexpected exceptions 向调用者传播。
2. 先完成 heartbeat/system，再调用 `SM.trySnapshotProcessIdentities()`；它返回
   `optional<vector<ProcessIdentity>>`：disengaged 是锁忙，engaged empty 是没有 eligible child。
   eligible 只看正 PID 与正 launched_generation；STOPPING/FAILED 未回收 child 也包含。
3. 调用 `collector.collectProcesses(captured, validator, config.monitoring.sample_interval_seconds, keep_running)`。
   validator 接收 captured vector，直接返回 `SM.tryValidateProcessIdentities(captured)`；不能替换为 N 次 list/query。
   validator 必须提供，返回仍匹配的 captured 身份；Collector 校验完整集合后才提交。
   keep_running 使用跨线程安全停止 gate，不读 writer-only shutdown 字段。
4. `ProcessResourceScan.quality`/`processes` 分别转入 bundle 的 process_scan_quality/processes。
   unavailable scan 的空 rows 不代表服务退出；successful owner validation 的 scan 可以包含个别 unavailable/not_present/zombie row。
   数值 row 的 instance_generation 始终是采样前捕获的 launch token，不作为最新 lifecycle 状态。
5. scan 不是两次 owner 查询之间的事务。生产仍依赖前台直接子进程、独占回收、无自动回收/外部 reaper 契约。
   Collector 不引入 pidfd 或另一个进程后端。不要将 system/process 文件称为原子整机快照。
6. 新生产入口用 `load_runtime_file` 的完整配置；programmatic RuntimeConfig 的 monitoring 应通过新增 validate。
   旧 ResourceThresholds 参数仍由后续 legacy external 构造入口处理；完整 config 不再接受第二份阈值覆盖。
   本轮没有为 native/external 添加 JSON 字段。
7. typed scan 本轮无日志/健康 policy；后续 worker 按既有失败节流规范记录采集 unavailable，遵守停止前/逐进程/发布前 gate。
   Collector 的 gate 丢弃在途 process 提交不替代 Runtime 发布 bundle/facts 前的再次停止检查。

## Not Verified

`tests/phase5_static_check.py` 尚不存在，CHANGE_LIST 将其归属 T4，不在 T2 Allowed Files；本轮未创建该文件。
T2 补充审计只覆盖本轮源码契约，不能替代 T4 完整 P5 checker。
资源迟滞、native/external、Runtime 采样/cache/query/main 接线及端到端 health/recovery 组合是后续 T3/T4 工作。
本报告只给静态证据，不宣告完整 Phase5 完成。
