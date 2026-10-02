# Current Repository Assessment
基线：master，2ccbc733a1386c46917e6aee2fc7e0680e4321e9；通过已连接 GitHub 的 repository/branch/tree/fetch_file 读取，非网页搜索。递归树 truncated=false；缓存了 90 个非空文本文件，包括 P0–P3、所有生产模块、测试与 CMake。仓库没有 docs/P4。

## 真实目录与实现
ServiceManager / ProcessSupervisor 头文件在 include/runtime；实现位于 src/service。没有 include/service 或 include/ipc。IPC 头文件与 frame 位于 src/ipc，Monitor/config/logger 实现在各自 src 子目录。不能按提示词假设不存在的目录。

P0 冻结 Supervisor、进程隔离、Unix socket、五次 restart 和 2–60s。P1 已落实单写者、线程/队列与进程后端；P1 历史三次心跳 miss 和 2/5/10/30/60 延迟已被 P2 first accepted timeout 与 2/4/8/16/32 替代。P2 的 RecoveryManager 是文档职责，源码没有独立 RecoveryManager，策略实际在 SM.fail/tick。P3 设备/聚合/IPC 已完整接线；不能把 P4 定义为另一次 IPC 实现。

| 当前事实 | 源码依据 | Phase4 含义 |
| --- | --- | --- |
| maximum_restarts=5；delay 2、4、8、16、32，公式 ceiling60；reserve 时 ++restart_count，成功/START 不 reset | S01/S02/S15 | 迁移预算原语义，禁止 success reset 造成无限抖动 |
| fail 发 FAILED 并 stop_dependents，SIGTERM，设 restart_at，再 RECOVERING；tick 发强杀并 replacement | S02/S04 | 抽出 retry 与 due，保留停止升级/依赖执行 |
| lifecycle generation 在 start/fail/explicit stop 增长；RUNNING 保留 launch generation | S03 | 请求 fault token 与成功 launch token天然不同，必须显式 binding |
| Runtime service_recoveries_ 推导成功；exhausted 回调推导 terminal | S05 | 删除这些策略推导，让 RM 唯一出结果 |
| 主循环 queue/drain/reap/tick；reaped exit 当前重新 post | S06 | 复用 writer，调整 recovery checkpoint 对退出的处理顺序 |
| aggregator 拒绝缺失/旧/未来具名 recovery token；SERVICE_STARTED 会先清除故障 | S08 | 增加 request/launch bridge，并阻止未经结果门确认的 recovered start 提前清故障 |
| HIGH 或 heartbeat terminal OFFLINE；普通 optional terminal WARNING；资源独立 | S09 | 保留该细分，成功重聚合 |
| IPC pending_restarts 保存 manual 操作，poll STOPPED 后 post START | S10/S11 | 必须迁移，不能只抽 SM 策略 |
| start 同步 fork/exec handshake，rollback waitpid | S12/S13 | 超时有 writer 可推进限制；风险要明确 |
| 配置无 max_restart_attempts/restart_delay/max_restart_delay/recovery_timeout；timeout1..3600 | S14、include/runtime/config_manager.hpp | 冻结前三者为常量，只添加一个 recovery_timeout |
| tests 8 executable、9 CTest，含独立 shutdown regression | S18/S17 | 原测试行为迁移到含 RM 的 fixture；不得删除/屏蔽 |

## P3 历史审查不能当成当前缺陷
validation_review.md 顶部说明后续已修复。review_fixes.md 与代码一致：
- V01 资源 critical 已实现，Phase5 延后的是实际采集，不是 severity 字段。
- V02 lifecycle generation 与 aggregator 防迟到结果已实现；尚缺跨 fault→replacement launch 的真实 RM 关联，P4 必须补齐。
- V03 enabled 自动耗尽已有 recovery_exhausted → RECOVERY_FAILED，不再是“枚举无人生产”。迁移时必须同时移除旧生产者。
- V04 fixed4s 已替换为最大 outstanding shutdown_deadline+5s；成功返回需无活动 PID，失败抛异常。无需另开 prerequisite 修复线程，但现有回归必须作为 migration gate。
GET_HEALTH 首心跳前 UNKNOWN、非原子跨模块快照、异常清理、IPC 有界通知队列已存在。P4 不回退这些修复。

## 测试与事实限制
已核对 recovery_dependency、event_aggregation 的 generation/terminal 关键用例、phase2_integration 的真实进程/六秒 shutdown 用例，以及 runtime_core、device_state_manager、process_lifecycle、IPC 测试场景和注册。现有具名手工 recovery 测试并不能证明真实 executor 的 request→new launch→result 关联正确。

本次没有执行仓库静态脚本、CMake、编译器、CTest 或 Linux 运行测试。P1–P3 报告仅声称源结构静态通过；本包不会将它升级为运行证据。

## 源码证据索引
以下链接全部绑定同一提交，行号为从所读取完整文本计算的 1-based 行号。
- S01: [src/service/service_manager.cpp:31](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/service/service_manager.cpp#L31)
- S02: [src/service/service_manager.cpp:264](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/service/service_manager.cpp#L264)
- S03: [src/service/service_manager.cpp:67](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/service/service_manager.cpp#L67)
- S04: [src/service/service_manager.cpp:342](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/service/service_manager.cpp#L342)
- S05: [src/runtime/runtime_manager.cpp:70](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/runtime_manager.cpp#L70)
- S06: [src/runtime/runtime_manager.cpp:194](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/runtime_manager.cpp#L194)
- S07: [src/runtime/runtime_manager.cpp:238](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/runtime_manager.cpp#L238)
- S08: [src/runtime/service_aggregation.cpp:27](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/service_aggregation.cpp#L27)
- S09: [src/runtime/service_aggregation.cpp:120](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/service_aggregation.cpp#L120)
- S10: [src/ipc/ipc_manager.cpp:352](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/ipc/ipc_manager.cpp#L352)
- S11: [src/ipc/ipc_manager.cpp:617](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/ipc/ipc_manager.cpp#L617)
- S12: [src/service/process_supervisor.cpp:117](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/service/process_supervisor.cpp#L117)
- S13: [src/service/process_supervisor.cpp:94](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/service/process_supervisor.cpp#L94)
- S14: [src/config/config_manager.cpp:190](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/config/config_manager.cpp#L190)
- S15: [tests/recovery_dependency_tests.cpp:68](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/recovery_dependency_tests.cpp#L68)
- S16: [tests/event_aggregation_tests.cpp:289](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/event_aggregation_tests.cpp#L289)
- S17: [tests/phase2_integration_tests.cpp:346](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/phase2_integration_tests.cpp#L346)
- S18: [tests/CMakeLists.txt:1](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/CMakeLists.txt#L1)

## 必读设计来源
[Phase0 architecture](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P0/RK3588_Runtime_Management_Phase0_Architecture_Design.md)；P1 runtime_core_contract；P2 recovery_dependency_design/ipc_config_extension/process_backend_implementation；P3 ARCHITECTURE、DEVICE_STATE_MODEL、STATE_TRANSITION、AGGREGATION_RULE、EVENT_MODEL、IPC_EXTENSION、MONITOR_INTEGRATION、phase4/RECOVERY_STRATEGY_INTERFACE、validation_review、review_fixes；P3/codex 的四个线程模板。详见 [SOURCE_MANIFEST.md](SOURCE_MANIFEST.md) 的全文件目录。
