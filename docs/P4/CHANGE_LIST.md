# Module / Interface Change List
所有路径为目标仓库relative路径；本设计包未修改它们。只允许下列必要局部改动；无路径存在性猜测。

## Module changes
| 路径 | 修改内容 | Owner thread |
| --- | --- | --- |
| 新include/runtime/recovery.hpp | Request/Result/Outcome/FailureType/RecoveryContext定义 | T1 |
| 新include/runtime/recovery_manager.hpp、src/runtime/recovery_manager.cpp | episode/generation/budget/policy/timeout/cancel及内部snapshot；无线程 | T1基础，T2策略，T3接线修正 |
| include/runtime/event.hpp、src/runtime/event.cpp | 尾部instance/context/result envelope、internal restart_request；不增RuntimeEventType | T1契约，T3接线 |
| include/runtime/service_manager.hpp、src/service/service_manager.cpp | lifecycle primitives、captured callback元数据、去restart_at/策略；保留stop/graph/registry | T2 |
| include/runtime/runtime_manager.hpp、src/runtime/runtime_manager.cpp | RM构造、facts gate、writer tick/退出顺序、取消；移除service_recoveries_/旧terminal producer | T3 |
| include/runtime/service_aggregation.hpp、src/runtime/service_aggregation.cpp | recovery active binding/candidate gate；重算与原severity规则 | T3 |
| include/runtime/monitor.hpp、src/monitor/monitor.cpp | watch保存/回送instance generation，仅心跳正确性 | T3 |
| include/runtime/config_manager.hpp、src/config/config_manager.cpp | recovery_timeout单字段/derived default及一致校验 | T2 |
| src/ipc/ipc_manager.cpp | type6提交internal command，删manual poll与独立cancel决策；保留通知 | T3 |
| CMakeLists.txt | recovery_manager.cpp进runtime_core，不新增依赖 | T1 |
| tests/CMakeLists.txt | 新RM/coordination测试注册，保留8目标9CTest | T1局部注册，T4最终 |
| tests/recovery_dependency_tests.cpp、runtime_core_tests.cpp、event_aggregation_tests.cpp、phase2_integration_tests.cpp | 旧auto fixtures接RM；新增关联/终结/取消断言 | T2各自相关，T3接线，T4最终 |
| 新tests/recovery_manager_tests.cpp、recovery_coordination_tests.cpp、phase4_static_check.py | 假时钟unit、真实Linux链、结构约束检查 | T1基础unit，T4扩展 |
| tests/ipc_integration_tests.cpp、device_ipc_tests.cpp | manual route/兼容及真实恢复→设备通知 | T4 |
| docs/P4 | implementation/validation报告与设计同步 | 各自报告，T4review |

include/runtime/process_supervisor.hpp与src/service/process_supervisor.cpp默认不改接口/实现。减少startup预算通过launch配置副本传给现有start，不改变静态服务定义。若发现必须修后端bug，只做有失败证据的局部修复并记录，不开展async重写。
device_state.hpp/device_state_manager.hpp/.cpp状态表默认不改；frame.hpp/.cpp、logger、tools/fake_service默认不改。integration优先re-exec测试自身为child，确需fixture能力才T4局部改tools/fake_service且不得业务化。
P0–P3文档保留历史内容；必要交叉引用可在T4最小添加，不能覆写历史测试结论。

## Interface changes and compatibility
| 现有/新接口 | Phase4契约 | 兼容影响 |
| --- | --- | --- |
| ServiceManager start/stop/query/list/startup_order | 保留名称/同步snapshot；start期间active gate | 公开生命周期接口保留 |
| restartService(name) | 仍bool表示是否成功route，same manual entrypoint | Runtime必须连接sink，不能留未连接悬空方案 |
| ServiceStatus.generation/restart_count | generation由SM，count值RM projection | wire字段保留，count lifetime语义保留 |
| ServiceStatus.launched_generation（尾部新增） | 当前未reap进程的STARTING token；fail/stop推进generation时它不变，清PID时清除 | 内部instance身份，不新增wire字段/计数器 |
| ServiceStateChange | 尾部cause/operation context | 内部追加字段；captured而非完成时query |
| recovery_exhausted | 可保留尾部默认false作source兼容，deprecated且不作为策略输入/结果producer | tests不再要求SM自主标terminal |
| Event / RuntimeEvent | 尾部typed binding / generation；internal restart command | 不变更IPCframe/types；旧aggregate init尽量保留 |
| Monitor watch / health envelope | instance gen捕获；旧调用可default但Runtime命名路径必须nonzero | 内部契约收紧，旧health tests更新fixture |
| RuntimeManager::post(RecoveryResult) | 可选内部producer入口，经active gate | 不新增socket RPC；禁止直通success |
| RuntimeManager constructor | 静态service config构建RM；旧参数次序保留 | 默认参数保持existing callers |
| RM submit/observe/tick/cancel/cancelAll/query | 新内部单写者API语义 | snapshot复制并同步；query不推进状态 |
| RM classifyExit | 唯一读取restart_policy分类clean/abnormal | SM执行explicit disposition，不再自主策略 |
| SM begin/launch/finish/project primitives | expected gen/context校验，无policy | 不暴露业务RPC |
| IpcManager Post/Query/QueryDevice构造 | 保留 | type6转内部单Event，无新增构造依赖 |

## 禁止修改范围
业务service、VisionArm/VisionArm-MCU、摄像头/推理/MCU/OTA、Kernel/driver、transport/frame协议、完整resource monitoring、数据库/cloud/MQTT/Web、第三方RPC/状态机/workflow框架。不得为兼容旧fixture留下legacy自动retry loop；可以修改fixture组合但不削弱行为断言。
