# Current Repository Assessment

## 来源与验证等级

通过已连接 GitHub 固定读取 master@4843061fce2a329832105d34aaea380a35560c0e 的完整递归树（未截断），获取 120 个非占位文件。重点直接阅读 P0 架构、P1 contract/IPC/review、P2 process/config/lifecycle/recovery 文档、P3 Monitor/Aggregation/Event/State/review、P4 Architecture/Decisions/Freeze/validation/review_fixes，以及相关实现与 tests。完整取回清单见 SOURCE_MANIFEST。

“全部取回”不意味着本次执行了仓库测试。下面源码结论是人工静态路径审查；P4 原日志通过是仓库报告的历史证据。本轮没有编译、执行 C++ 或运行 Host Linux/RK3588。当前主机是 Windows，不能用它证明 Linux procfs 行为。

## 可核查的当前事实

| 实际位置（本提交行号） | 当前行为 | P5 影响 |
| --- | --- | --- |
| include/runtime/monitor.hpp:13、:31 | ResourceThresholds CPU=80、Memory=80/95；report_resources 接受两个 double | 复用三个激活阈值和既有方法；增加独立 metric validity 与 clear 阈值 |
| src/monitor/monitor.cpp:19 | 先校验两个百分比，随后每次各发一个 CPU/Memory fact；无 latch/迟滞；94.9 可立即将 critical 降为 warning | 不能只接 procfs：需要迁移 policy 与旧测试期望 |
| include/runtime/event.hpp:19 | resource_warning；ResourceSeverity warning/critical；active false 清 source | 足以表达 P5，不增 RuntimeEventType |
| src/runtime/service_aggregation.cpp:28、:192 | device-scoped source→severity map；clear 只擦本 source；evaluate 全部服务和资源 | 不新建设备状态或资源状态机；使用现有重聚合 |
| src/runtime/runtime_manager.cpp:39、:317 | loader 只取 ServiceConfig；资源外部调用转 Monitor→post | 添加完整 runtime config 与原生采集入口 |
| 同文件 :355、:366 | 已有 Monitor worker；timerfd 每1s向 monitor_queue 投 health_check；worker 当前仅 Monitor.check | 复用该执行上下文，不新增采样线程 |
| 同文件 :402、:422 | writer FIFO，reap/SM tick/RM tick；shutdown 停 workers 后 stop_all、最大 grace +5s drain | 采集与 writer 隔离，关闭前停止采样；不重写 P4 关闭协议 |
| include/runtime/service_manager.hpp:20、src/service/service_manager.cpp:231、:301 | ServiceStatus 有 generation 和 launched_generation；launch 给实例 token，reap 清 token/PID | process identity 使用 launched_generation；不能用 failure/STOP 后的新 generation |
| src/service/service_manager.cpp:401 | all_statuses 锁内返回副本 | 提供窄批量 identity 查询，锁外读 procfs；现有锁在 launch 内可能持有较久 |
| src/service/process_supervisor.cpp / P2 process_backend_implementation | fork/exec、pidfd 或 WNOWAIT、独占直接子进程 reap | Collector 不 wait/kill；PID 安全基于既有前台/独占 reap 契约 |
| src/config/config_manager.cpp:111、:258 | 内置 JSON parser 仅整数；root 为 services 对象或单服务对象；load_file 返回 vector<ServiceConfig> | global monitoring 最小兼容扩展，不加 decimal parser 或每服务系统阈值 |
| src/ipc/main.cpp:15、:21 | Runtime 与 IPC 各 load_file；构造后接状态 sink | 新入口解析一次完整 RuntimeConfig，向 IPC 传其 services |
| src/ipc/frame.hpp / ipc_manager.cpp | UDS、10字节帧、64KiB、types 1..10/255；只订阅 DEVICE_STATE_CHANGED | 无 GET_RESOURCE_USAGE；内部资源 fact 不等于 socket 原始资源广播 |
| tests/CMakeLists.txt | 10 test executables / 11 CTest（含独立6s shutdown regression） | 全保留，增加 P5 测试；不能删旧 oracle 来迁就实现 |

## P0–P4 文档与当前实现的差异

P0 声称 Phase1 应采集 CPU/Memory/RSS；当前实现实际延后到 P5。P3 Monitor 初段列 CPU/Memory/Process，不代表真实 collector 已存在；文档末尾明确是外部输入。P1 retry 序列和 P2 的 SM recovery owner 已由当前 P4 的 RecoveryManager 单 owner 替代。P3 review_fixes 中 Runtime 推导 exhaustion 的历史描述也不是当前 owner。

P4 DESIGN_FREEZE_CANDIDATE 开头明确：后续 T1–T4 状态看 validation_review，运行未验证。P4 validation_review/review_fixes 记录 F01 checker 迁移、F02 IPC mock 更新、F03 OFFLINE 取消悬空 RECOVERING 修复，明确只做静态审查。故不能接受参考附件里“P4 DONE”为运行验收事实。

当前 docs/P4/codex_package 缺失；使用现存 docs/P2/codex、docs/P3/codex 的按文件读取任务风格，生成本次独立 P5 包。不存在根 AGENTS.md，不需要覆盖仓库已有根规则。

## Existing Monitor Gap Analysis

1. 无 /proc/stat、/proc/meminfo、/proc/PID/stat 资源采集及 CPU delta。
2. 两个必填 double 不能表达 CPU 首次 warming-up、memory 同时 valid。
3. 无 counter regression、部分失效、staleness 与 PID reuse 语义。
4. 激活和 clear 共用阈值，每次重复发布；易抖动、积累无必要队列事件。
5. 无 typed resource snapshot/global monitoring JSON。
6. 真实采集与外部 report/raw source 可能竞争，必须冻结生产者模式。
7. 已有 heartbeat、severity/source latch、全量 evaluate、single owner 等应复用。

## Current validation status

| 能力 | Implemented | Statically Verified | Host Linux Runtime Verified | RK3588 Runtime Verified |
| --- | --- | --- | --- | --- |
| P1–P4 基础与恢复 | 仓库源码存在 | 历史 P4 报告有通过记录；本轮人工核查关键路径 | 本提交没有可据以宣告完成的运行证据 | 无证据 |
| P3 外部资源输入/clear/critical 聚合 | 源码及断言存在 | 本轮人工审查确认链路；不是重新运行检查器 | Not Verified | Not Verified |
| P5 collector/policy/process/config/native wiring | 未实现 | 本包只冻结设计，不能称源码静态通过 | Not Verified | Not Verified |

进入实施前核对 HEAD 差异；P4 运行验收债务需在最终集成门完成，失败则成为实施阻塞。设计可以基于当前明确事实推进，不把未执行 C++ 当已通过。
