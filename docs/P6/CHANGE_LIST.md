# Cross-Repository / Module / Interface Change Plan

本轮全为后续实施计划，无C++文件生成。新文件名为建议冻结路径，等价拼写须交接记录；owner/wire语义不可暗改。

## runtime_management

| 文件 | 改动 | Thread |
| --- | --- | --- |
| include/runtime/health_policy.hpp（新） | 原ServiceHealthPolicy/ServiceCriticality移入轻头，原名字保留 | T1 |
| include/runtime/component_health.hpp（新） | 五状态、fact、policyentry、snapshot；无UARTinclude | T1 |
| include/runtime/event.hpp、src/runtime/event.cpp | 尾部typedcomponent字段、一个事实type和窄ingresswake | T1 |
| include/runtime/service_aggregation.hpp、src/runtime/service_aggregation.cpp | registry、componentlatch、readiness、critical优先、全量evaluate/reconcile | T1 |
| include/runtime/runtime_manager.hpp、src/runtime/runtime_manager.cpp | privateadmission/committedcache、session/identity/lease/sink/生命周期撤销，原writer保留 | T1接口与核心 → T3transport/config接线，串行 |
| include/runtime/service_manager.hpp、src/service/service_manager.cpp | 仅P6boundheartbeatinstance_gate，必要只读identity API复用现有try方法 | T1 |
| include/runtime/config_manager.hpp、src/config/config_manager.cpp | RuntimeConfig尾部health，integer静态schema、policyoverride | T3 |
| src/ipc/frame.hpp | 尾部type11编号；header/frame编码不改 | T3 |
| src/ipc/ipc_manager.hpp、src/ipc/ipc_manager.cpp | peer/session绑定，严格newparser、asyncreply、newcommands/query，boundtype4 | T3 |
| src/ipc/main.cpp | 一次config、同healthregistry、privateweakreply/cache接线；保留CLI/socket/sink | T3 |
| CMakeLists.txt | 若需新轻model实现加入runtime_core；无UART依赖 | T1，T3不并行改 |
| tests/component_health_tests.cpp（新） | normalizedadmission、instance/revision、aggregation/leaseunit | T1 |
| docs/P6/fixtures/health_contract_v1.json（后续新建） | T1 将本文 JSON 及 issue/ACK/reconnect 顺序固化为合同测试样例；T2/T3 只读同版副本，不各自修改 | T1 |
| tests/component_ipc_tests.cpp（新） | bind/peer/seq/invalidpayload/query/backpressureLinux | T3 |
| tests/CMakeLists.txt | 注册新目标，旧12/13全保留 | T1→T3→T4串行 |
| tests/event_aggregation_tests.cpp、runtime_core_tests.cpp、device_ipc_tests.cpp、ipc_integration_tests.cpp、recovery_coordination_tests.cpp、phase2_integration_tests.cpp | 增补P6组合，保留旧assertion，原6s函数不动 | T4 |
| tests/phase3_aggregation_static_check.py、phase3_ipc_static_check.py、phase3_validation_static_check.py、phase4_static_check.py、phase5_static_check.py | 仅经下表批准迁移冻结/oldenum/count表达式 | T4 |
| tests/phase6_static_check.py（新） | 事实/peer/单owner/noUART/覆盖/旧保护 | T4 |
| docs/P6/implementation_threadN.md、validation_review.md、review_fixes.md、static_validation.log（实施后） | 分层真实证据；不得写假PASS | 各任务 |

Runtime不新增UARTcodec/SerialPort/mcu_service，不改recovery模型/manager、ProcessSupervisor、DSM表、Logger、ResourceCollector/Monitor、fake_service和P0–P5历史记录。

## VisionArm

| 文件 | 改动 | Thread |
| --- | --- | --- |
| visionram/include/control/uart_health_projection.h（新） | ownercoherentcurrent/pendingissue/revision/epoch/reason；管理投影不wire | T2 |
| visionram/include/control/uart_link.h、visionram/src/control/uart_link.cpp | 窄Read/Ackseam，在原timeout/boot/open/reopen/ready决策点记录投影；无协议重写 | T2 |
| visionram/include/runtime/runtime_supervisor_client.h、visionram/src/runtime/runtime_supervisor_client.cpp（新） | 非阻塞UDSbind/report/type4、1s/3.5s、pendingissue确认、STOP/lifetime | T2 |
| visionram/include/runtime/runtime_app.h、visionram/src/runtime/runtime_cli.cpp | 两个managedCLI参数，配对/uartvalidation | T2 |
| visionram/src/runtime/runtime_components.cpp | clientRAII接线，managedreadytimeout非fatal、原sink/关停 | T2，仅明确UART/管理接线区域 |
| visionram/src/runtime/CMakeLists.txt | 注册client/Threads；可选managed支持且原产品目标保持 | T2 |
| tools/test_apps/runtime_supervision/CMakeLists.txt、tests/harness/fixtures（新，实施后） | 独立hostharness直接链接生产UART+clientsource，不复制；PTY/fake MCU | T2最小unit → T4补齐 |
| docs/P6/implementation_thread2.md、validation_review_visionarm.md、设计合同副本 | 持相同包版本，报告实际scope与验证 | T2/T4 |

生产uart_protocol/uart_transport和controlsink/targetalgorithms冻结；projection观测不是搬迁成熟UART库。tools/test_apps/uart原源码和real-board程序本轮不变，hostharness只用生产canonical源，禁止再copy。VisionArm-MCU生产全冻结；fakeMCU若用Ccodec由hostharness通过显式VISIONARM_MCU_SOURCE_DIR链接固定版本portable源，仅testtarget；不把它复制进Runtime或改MCU行为。

## Interface Change List

1. healthpolicy原类型迁移保持source名字；RuntimeConfig附加HealthConfig。
2. ComponentHealthFact/status/snapshot及Aggregation.add_component，原service/resource/recovery接口不变。
3. Runtimeprivatecomponent ingress/reply/querycopy、有限session与lease，SMboundheartbeat追加instance核验。
4. IPC HEALTH_REPORT11 bind/report，type9 opt-inquery；frameheader/size/旧编号/原EVENT方向不变。
5. VisionArm projection Read/Ack、managedclient生命周期/两个CLI参数；生产wire/header/CRC/message/timeout原owner不变。
6. 不承诺未冻结的C++binaryABI；无rawSTATUS跨repo头、无通用metricsregister。

## 静态checker迁移批准范围

P5 checker按4843061保护event/aggregation/IPC/SMheartbeat/main，P6合法扩展会触发失败；不能简单删checker/PROTECTED或把失败叫falsepositive。
T4记录每条旧断言→本表批准变更→新oracle：
- 原8RuntimeEventTypes/旧type1..10：保留原八项/原编号，新增唯一component事实与11；EVENT仍出站拒入站。
- aggregation全文冻结：换为原RM/resource/terminal不变、componentpriority/readiness组合断言、DSM原441矩阵。
- SM原handle全文比较：只开放P6boundheartbeatinstance字段谓词，其余lifecycle/launch/reap/recovery保持逐函数保护。
- IPC头/source/main字节保护：限新增peer/session/newcommand/query/configwiring；旧commands/帧/GET_HEALTH{}、subscription/overflow/weaklifetime仍测试。
- P3/P4/P5checker的自身迁移历史比较：记录新的P6baseline，本次改动区域逐项白名单；保留全部P4/P5覆盖与原六秒oracle。
不能删除历史源码测试/注册、DISABLED/WILL_FAIL、放宽budget/PID/ECHILD/代次/DSM/owner测试。新phase6checker统一调用迁移后的原checks，静态通过仍不是runtimePASS。
