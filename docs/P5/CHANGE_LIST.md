# Module / Interface Change List

全部是后续实施计划；本轮未改C++。新增路径为冻结建议，等价方法拼写可调整，语义/owner不得暗改。

| 文件 | 改动 | Thread |
| --- | --- | --- |
| include/runtime/resource_snapshot.hpp（新） | 最小typed observation/system/process/bundle/identity | T1 |
| include/runtime/resource_collector.hpp、src/monitor/resource_collector.cpp（新） | reader seam、CPU/memory/delta，后续process stat | T1→T2 |
| include/runtime/config_manager.hpp、src/config/config_manager.cpp | RuntimeConfig/MonitoringConfig、load_runtime_file、旧load_file wrapper | T2 |
| include/runtime/service_manager.hpp、src/service/service_manager.cpp | 两个批量try身份只读接口，无lifecycle/recovery修改 | T2 |
| include/runtime/monitor.hpp、src/monitor/monitor.cpp | ResourceThresholds兼容扩展、typed observe、单一迟滞policy/change-only、heartbeat保留 | T3 |
| include/runtime/runtime_manager.hpp、src/runtime/runtime_manager.cpp | native/legacy external、reserved source准入、采样due/coalesce、cache/query、shutdown gate | T3 |
| src/ipc/main.cpp | 完整config只读一次，新native构造，同services供IPC，保留CLI/sink | T3 |
| CMakeLists.txt | collector加入runtime_core | T1 |
| tests/resource_collector_tests.cpp（新） | system fixtures，后续process fixtures | T1→T2 |
| tests/resource_monitoring_tests.cpp（新） | T2 config/identity、T3 policy/runtime、T4补充integration | T2→T3→T4 |
| tests/CMakeLists.txt | 新P5目标，原10executables/11CTest与独立6s回归全保留 | 对应线程串行 |
| tests/runtime_core_tests.cpp | resource threshold/index/次数/94.9期望按迟滞迁移，保留其它oracle | T3 |
| tests/event_aggregation_tests.cpp、tests/recovery_coordination_tests.cpp | 必要clear/多source/不触发recovery/关闭组合回归 | T4 |
| tests/phase2_integration_tests.cpp、tests/ipc_integration_tests.cpp、tests/device_ipc_tests.cpp | 必要native startup/shutdown/旧UDS兼容增补；原6s函数不改 | T4 |
| tests/phase5_static_check.py（新） | P5结构/注册/范围/owner检查 | T4 |
| tests/phase3_validation_static_check.py | 旧逐样本Monitor表达式迁移为typed policy/adapter/阈值测试检查；保留generation/RM/DSM/IPC | T4 |
| tests/phase4_static_check.py | 只解除main的P4字节冻结，替换为native startup/同config/sink oracle；其它保护/COVERAGE保留 | T4 |
| docs/P5/implementation_threadN.md、validation_review.md、review_fixes.md、static_validation.log | 后续证据，不能覆盖P3/P4历史日志 | 各线程 |

Protected：event.hpp/event.cpp类型及dispatcher、service_aggregation生产头/源、DSM状态表/头/源、ProcessSupervisor头/源、recovery.hpp/recovery_manager头/源、IPC frame/types/ipc_manager、Logger、fake_service、P0–P4历史文档/日志、device_state_manager_tests/process_lifecycle_tests、原六秒shutdown回归函数。确需越界需先记录具体缺陷与架构变更；不能借Review大重构。

src/ipc/main.cpp仅启动接线，非协议扩展。P4 checker的PROTECTED确实含main，P5有明确开放依据；不得把静态失败笼统标false positive，必须记录旧断言→批准语义→新oracle。

Source compatibility保留旧Monitor构造、ResourceThresholds原前三值顺序、report_resources、Runtime旧config_path构造/reportResourceUsage、ConfigManager.load_file、SM query/list/ServiceStatus字段、全部IPC。
行为迁移明确：hysteresis、stable不重复发、94.9保持critical、native禁止外部覆盖保留source。旧构造external、新生产main native见[CONFIGURATION.md](CONFIGURATION.md)。
不承诺未冻结的C++ binary ABI；不得通过DISABLED/WILL_FAIL/删除suite/弱化PID/reap/代次/6s断言掩盖失败。
