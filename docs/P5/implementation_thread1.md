# T1 implementation — Linux System Resource Collector

日期：2026-10-03（Asia/Shanghai）。本任务按用户指定范围只记录实现和静态检测证据。

入口：`codex_package/thread1_prompt.md` → `codex_package/thread1_task.md`。
已读取根与 P5 AGENTS、README、DESIGN_FREEZE_CANDIDATE、CHANGE_LIST、RESOURCE_MODEL、PROCFS_COLLECTION、ARCHITECTURE、TEST_PLAN；并核对现有 Monitor/event 接口，补读 PROCESS_MONITORING、SAMPLING_AND_FAILURE 的模型及失败契约。

实际 HEAD：`4843061fce2a329832105d34aaea380a35560c0e`，与设计基线一致。
开始时已有未跟踪的根 `AGENTS.md`、`docs/P5/`、`temp.log`；保留这些文件，P5 文档只新增本报告。

## Implemented

严格限定为以下七个 Allowed Files：

| 文件 | 实现 |
| --- | --- |
| `include/runtime/resource_snapshot.hpp` | 无 event/state/recovery 依赖的 typed model：quality、optional 当前值、last_success_at、饱和连续错误计数；system、process identity/row 和 bundle |
| `include/runtime/resource_collector.hpp` | 单调用方 Collector；窄 Reader/Now seam；分类读取结果；CPU previous 私有；64KiB 系统文件上限 |
| `src/monitor/resource_collector.cpp` | 固定 `/proc/stat`、`/proc/meminfo` 只读采集；bounded reader、严格整数解析、CPU delta/baseline、独立 Memory validity |
| `tests/resource_collector_tests.cpp` | 14 个由 main 调用的内存 fixture 测试函数；不读写真实 procfs，不增加线程 |
| `CMakeLists.txt` | 将 Collector 加入现有 `runtime_core` |
| `tests/CMakeLists.txt` | 新 `resource_collector_tests` / `phase5_resource_collector_unit` 注册及 10 秒 timeout；旧目标和属性保留 |
| `docs/P5/implementation_thread1.md` | 本报告与静态证据 |

CPU 只识别 aggregate `cpu` 行，解析前八个 counter；至少四字段，缺省可选字段按零，记录实际字段数量用于形状重验证。`guest/guest_nice` 和后续字段不加入 sum、不影响八字段形状。`idle+iowait` 为非忙；所有已使用分量不回退，sum 无溢出且 total delta > 0 才产出百分比。浮点转换后再计算，避免整数乘 100 溢出。

首次合法 CPU counters 仅 warming_up，值 absent，无成功百分比时间。合法 counters 发生 regression、形状变化或 invalid delta 时，本次 unavailable，以完整当前 counters 重建 baseline；读取/解析/超限失败清 baseline，下次合法读取重新 warming_up。合法 baseline 受理后重置连续错误计数，但不更新 last_success_at；只有有效百分比更新成功时间。

Memory 仅使用唯一的 `MemTotal` / `MemAvailable` 整数 `kB`，按 1024 转 bytes 并检查溢出、total > 0、available <= total。无 MemFree fallback。失效时清本次值，保留上次成功时间，错误计数饱和递增；CPU/Memory 互不影响。

只实现测量，不生成健康事实、不修改 lifecycle、不进行恢复。当前 executable 尚未接线到 Collector；T2/T3 继续承担 process/config/policy/runtime 工作。

### T2/T3 interface handoff

- `ResourceClock` 是 `steady_clock`，与现有 `Clock` 的底层类型相同；model 不需要 include event.hpp。
- CPU 是 `MetricObservation<double>`，字段为 `quality/value/last_success_at/consecutive_errors`。
- Memory 是 `MetricObservation<MemoryResourceUsage>`；有效时 `value` 包含 `total_bytes/available_bytes/used_percent`。无效时整个 value absent。
- `Reader(const std::string& path, std::size_t limit)` 返回 `ProcReadResult{text, error}`，错误为 none/not_present/permission_denied/io_error/too_large。expected failures 用结果表达，unexpected exceptions 交由后续既有 worker 边界请求 shutdown，Collector 不吞异常伪造正常。
- 默认 reader 以只读模式打开固定 proc 文件，最多保留限额内文本，用额外一字节检测超限；Collector 同时校验注入 reader 的返回文本上限。
- `collectSystem()` 在两个文件读取/解析完成后咨询 `Now`；`collectSystem(completed_at)` 为显式完成时间 seam。CPU 利用率只依赖 counters，不使用 elapsed 时间。
- ProcessIdentity 的 `instance_generation` 对应 SM 的 `launched_generation`；starttime 使用 process row 的 optional `proc_start_time_ticks`。本轮只声明，未引入 generation/PID owner 或进程采集方法。
- Collector 是单调用方对象，无新增 worker/mutex。CPU previous 不公开。age/stale 留给后续 query 派生。

## Statically Verified

环境提供 `python.exe`，未提供 `python3` 命令；使用同等 `-B` 选项运行现有 checker。旧 checker 源文件均未改动。

| 检测 | 实际结果 |
| --- | --- |
| `python -B tests/phase3_validation_static_check.py` | 退出码 0；P2 source/registration、P3 DSM/aggregation/IPC、441 target triples、review fixes 均通过静态 oracle |
| `python -B tests/phase4_static_check.py` | 退出码 0；17 frozen files、原六秒/PID/reap 函数、旧注册、owner/bridge/cancel/terminal/IPC 和 43 个 P4 证据函数保护通过 |
| T1 inline audit（PowerShell here-string → `python -B -`） | 退出码 0；四个新增 C++ 文件 include/delimiter/main/格式检查，Collector owner、只读路径、大小/算术/baseline/error guards、完成时钟顺序及 Allowed Files 检查通过 |
| `git diff --check` | 退出码 0；新增未跟踪 C++ 文件的末行/尾空白另由 inline audit 检查 |

最终 source inventory：42 个 C++ 文件；126 个 main-wired 测试函数；11 个测试 executable / 12 个 CTest 注册。旧 10 executable / 11 CTest 注册保留；无 DISABLED/WILL_FAIL。

T1 inline audit 使用现有 `phase2_static_check.check_source/masked_source/require`，检查新增四个 C++ 文件。额外核对：

- 无 EventSeverity/ResourceSeverity/RuntimeEvent/DeviceState/Monitor/SM/RM、线程或 kill/wait/fork/exec 引用。
- reader 调用仅有两个固定 proc 路径；默认 reader 为 `rb`，无写入路径；文本长度及 fread 错误/超限边界保留。
- 字段形状/逐分量回退/零 delta/idle delta/sum 与 kB overflow/失效清值/错误计数饱和的源代码 guards 存在。
- 两个文件解析均先于默认完成时钟读取；实现不引用 MemFree。
- 14 个 fixture 函数声明并由 main 调用；测试只注入内存 Reader/Now。
- Collector 在库中注册一次；tracked diff 只含两个 CMakeLists，新增 C++ 只含任务允许的四个文件。

静态输出节选（仅源码证据）：

```text
PASS (static): 42 C++ files, 126 main-wired tests, 11 targets/12 CTest entries
PASS (static): 17 frozen files, original six-second/PID/reap regression and old registrations
PASS (static): single policy owner, captured bridge, cancellation/timeout/ingress and IPC contracts
PASS (T1 static): four new C++ files; includes/delimiters/main wiring/format
PASS (T1 static): measurement ownership, fixed read-only paths, size/arithmetic/baseline/error guards
PASS (T1 static): 14 assertion-bearing fixture functions declared and invoked; CMake/scope checked
LIMIT: source audit only; fixture assertions were not executed
```

### T1 fixture source mapping

以下是源码中的 oracle 覆盖，不是 fixture 执行 PASS。

| ID / 系统部分 | 实际函数 |
| --- | --- |
| P5-01 | `test_cpu_optional_fields_and_guest_exclusion`、`test_cpu_each_component_busy_definition` |
| P5-02 | `test_first_sample_and_aggregate_delta`、`test_cpu_zero_full_and_large_valid_delta` |
| P5-03（测量部分） | `test_first_sample_and_aggregate_delta`、`test_first_read_failure_has_no_success_or_value` |
| P5-04 | `test_each_cpu_counter_regression_and_rebaseline`、`test_cpu_zero_delta_shape_change_and_wrap`、`test_invalid_cpu_text_clears_baseline` |
| P5-05 | `test_invalid_memory_text_and_no_free_fallback`、`test_memory_available_formula_order_whitespace_and_boundaries` |
| P5-06 | `test_first_sample_and_aggregate_delta`、`test_memory_available_formula_order_whitespace_and_boundaries` |
| P5-17（系统部分） | `test_read_errors_partial_validity_and_recovery_metadata`、`test_reader_size_limit_exact_and_oversize`、CPU/Memory 非法文本测试 |
| P5-33（T1 范围） | 固定只读路径、内存 fixture、无新增 worker/owner/恢复依赖：P3/P4 与 T1 补充静态审计 |
| 时间/异常 seam | `test_completion_clock_and_cpu_without_elapsed_time`、`test_unexpected_reader_exception_reaches_caller_boundary` |

## Not Verified

`tests/phase5_static_check.py` 尚不存在；CHANGE_LIST 将其归属 T4，且不在 T1 Allowed Files。本任务未创建或修改该 checker，T1 补充审计不能替代后续完整 P5 检查。

上述静态检测只证明源码结构、声明、保护契约和检查分支存在，不能证明 C++ 执行行为。T1 的 14 个 fixture 只记录源码覆盖。P5-03/17 的 policy pipeline 与 P5-33 的全 P5 范围等待后续任务；本报告不宣告 P5 整体完成。
