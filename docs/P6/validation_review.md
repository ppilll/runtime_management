# P6 Thread4 — Static Validation / Limited Review

日期：2026-10-15（Asia/Shanghai）。按用户指令仅进行静态检测；编译测试不作为本次要求或报告内容。
对象为 T1/T3 已修改的 Runtime 工作树和本地 T2 VisionArm 隔离源码包，保留所有进入时修改。
本次没有提交、PR、安装依赖或改动 P0–P5 历史记录。

## 组合基线与 gate

Runtime HEAD：`f8507027c87a9ae9899f51da708f8d469552c473`，不是已提交的 P6 实现。
VisionArm：`F:\嵌入式Linux学习资料\Vision Arm\Visionram_win`，无 Git 元数据；遵守其隔离规则，
没有尝试 Git。不能声称它等于设计引用的 `05148303c5ec5bba0ee89e707ab340e94e8aa3e1`。
MCU 设计引用 `92d8f45bc9c7df55f6e2abea4b4abfeb090a3e3d`，本次没有读取、改动或确认 MCU 工作树。
实际可复核组合由 Runtime 源码/迁移 checker 的固定 SHA256、VisionArm T2 原源码清单及本次日志中的
产品/测试文件 SHA256 标识；不存在可声称的双仓库组合 HEAD。

T1 schema 两个 VisionArm 副本和 Runtime 原件 SHA256 均为
`9c3a7d1543d259a281eb8003f80c7fe09254e029719194b6453c00629dac3680`。
联合静态入口还逐份比较 T2 清单中的合同文档，禁止两份合同各自漂移。

**仍然 Blocking：ADR-P6-T2-01。** 产品有限 duration 结束后，原 control_ok / EvaluateRuntimePass /
PublishRuntimeReport 仍可能将 UART 不可用映射为失败退出码，再被 Runtime 进程策略消费。
该问题已由 T2 明确记录，T4 再次核对路径；没有通过修改业务 PASS、强置 control_ok 或改 report 函数绕过。
因此 T2 的完整产品 gate 和 P6 整体完成 gate 尚未关闭。本次继续完成不依赖该决定的静态工作。
具体见 [validation_review_visionarm.md](validation_review_visionarm.md)。

## Implemented

- Runtime 追加 `test_p6_component_candidate_and_independent_resource_sources`：RM success 不能清链路故障；
  unfresh HEALTHY 不清 adverse；fresh clear 后 memory critical / CPU warning 依次独立保留；OFFLINE 快照与通知冻结。
- VisionArm 新 `runtime_pty_tests.cpp` 三个断言函数：生产 UART + canonical codec、CRC 损坏、协议静默、
  close master / HUP、新 PTY / 稳定 symlink、boot 0 / changed ID / same ID、旧 ACK、新 issue、HELLO reject、
  missing path 与有界 join。没有复制协议定义、放松 TIOCEXCL 或模拟执行器。
- 串行追加 VisionArm harness 注册和 `phase6_pty_static_check.py`，原八个测试源码及注册保持。
- 迁移四个旧 checker；新增 `phase6_static_check.py`，调用全部 P3/P4/P5 入口并补 P6 准入、owner、
  时序结构、旧测试原文、合同与矩阵检查。精确迁移见 [review_fixes.md](review_fixes.md)。
- 本报告、双仓库审查、修复记录及 [static_validation.log](static_validation.log)。没有修改产品 C++。

## Statically Verified

使用已有 Python `-B` 和标准库。最终完整 stdout/stderr/exit、平台信息、源文件指纹见日志。
Python 文本/结构检查不是 C++ 类型检查或并发证明。所有 PASS 均限定在静态层。

| 检查 | 最终 exit | 静态含义 |
| --- | --- | --- |
| `python -B tests/phase3_validation_static_check.py` | 0 | 全部四个旧 suite、原事实及唯一新增事实、旧协议编号 +11、441 声明空间 |
| `python -B tests/phase4_static_check.py` | 0 | 单 RM policy、五预算、bridge/cancel/terminal、原六秒/PID/ECHILD、全部 P4 映射 |
| `python -B tests/phase5_static_check.py` | 0 | 全部 P5 measurement/policy/native/resource/identity/owner 及历史保护 |
| `python -B tests/phase6_static_check.py --visionarm-root <本地源码包>` | 0 | 复用旧入口、P6 准入/格式/聚合/生命周期、双仓库合同、T2/T4 源码审计 |
| VisionArm `phase6_adapter_static_check.py` | 0 | 100 原文件、74 原函数、合同、owner/client/CLI/关闭及八个函数接线 |
| VisionArm `phase6_pty_static_check.py` | 0 | 三个新增 PTY 函数、codec/UART 引用、故障与 deadline/RAII、串行注册 |
| `git -c core.safecrlf=false diff --check` | 0 | tracked diff 空白；新源码/文档由静态入口另检查 |

Runtime 共 47 个 C++ 文件、190 个 main 接线函数，14 executables / 15 注册；原 12 / 13 前缀逐字保留。
VisionArm harness 为原八个 + 新三个断言函数、两个串行注册，直接引用原四个 production sources。
原 DSM 24 allowed / 417 rejected 和全部旧测试函数原文保持；没有 DISABLED / WILL_FAIL。

## Owner / admission / lifecycle 审查

| 边界 | 最终源码证据 / 限制 |
| --- | --- |
| process policy | RM 源码冻结，五次 reservation/delay、deadline、manual/stop/dependency/cancel 原 owner 不变；component admission 无 process 动作 |
| PID / launch / measurement | SM 仅 heartbeat 追加 launched_generation 谓词；原 health_missed 谓词保持；Collector、Monitor、backend 冻结 |
| UART | Runtime 无 UartLink/SerialPort/tty/termios/RS485；VisionArm 新测试引用生产 codec/transport，MCU 未改 |
| 身份 | server SO_PEERCRED + accepted_at/token；writer exact RUNNING child PID / launched_generation / epoch / session；不用 producer timestamp |
| ACK / replay | 完成事实派发、DSM 与 committed cache 后 weak completion；same seq 无续租；issue/current 窄例外、重连下界和 boot/epoch 比较 |
| bounded / fairness | private outstanding<=32、writer batch<=8、service 每轮16帧、health<=4096；peer tombstone 独立于满队列，旧 output cap 保留 |
| type4 | P6 producer 必须同绑定连接；writer 重验后交 SM；heartbeat 不续 component lease，不依赖 link status |
| 聚合 | 去除明确 component 扩展后 aggregation.cpp 与 P5 HEAD 全文一致；critical component 优先 RM；只清本组件并全量 evaluate |
| 兼容 | 原八事实 ordinal、frame 10-byte/64KiB/types1..10/255、EVENT 出站、旧 {} GET_HEALTH 和旧命令保护保留 |
| shutdown / query | component gate 先于 RM cancel；原 grace/reap/signal 后缀不变；query 仅复制 cache、派生 age，不推进 lease |

## 38 场景逐项源证据

缩写：CH=`tests/component_health_tests.cpp`，CI=`tests/component_ipc_tests.cpp`，
EA=`tests/event_aggregation_tests.cpp`，RC=`tests/recovery_coordination_tests.cpp`，
VT=VisionArm 原 `runtime_supervision_tests.cpp`，PT=新增 `runtime_pty_tests.cpp`。
函数名省略 `test_`。I=Implemented（有测试/生产源码）；S=Statically Verified（源码/接线）；
Partial 表示仍缺指定场景源码或有 Blocking，不能当覆盖完成。该表不报告编译测试结果。

| ID | 源证据与 oracle | 本次状态 / 尚缺 |
| --- | --- | --- |
| P6-01 | CH required_readiness_high_loss_and_local_latch；VT initial_unknown_and_bind_retry_no_unbound_heartbeat | I/S；exec、首心跳与 component readiness 分离 |
| P6-02 | PT crc_silence_hup_and_reopen；production HandleHelloAck/HandleStatus → READY | I/S；生产 UART 到 projection，未接真实 Runtime |
| P6-03 | CI subscription_and_link_loss_preserve_process_identity；PT crc_silence_hup_and_reopen | Partial；normalized loss 与 PTY 分开；产品 ADR 未关闭 |
| P6-04 | CH session_overflow_never_wraps_and_link_loss_has_no_process_action；CI subscription_and_link_loss_preserve_process_identity | I/S（Runtime）；完整 product 受 ADR 限制 |
| P6-05 | CI subscription_and_link_loss_preserve_process_identity；CH 无进程动作；原 RC 预算保护 | Partial；尚缺跨多次 reopen 的真实 client→Runtime storm 组合 |
| P6-06 | CH issue_current_same_revision_and_new_issue_priority；VT applied_ack_rereads_newer_owner_issue_before_current；PT reopen | I/S 分层；尚缺 production client + Runtime 联合断言 |
| P6-07 | PT confirmed_boot_change_and_same_id_reconnect；VT stable_revision_boot_zero_and_epoch | I/S；不同有效 ID 与同 ID 降级，不承诺每次 reboot 可检测 |
| P6-08 | CI disconnect_and_process_exit_are_independent；new_launch_rejects_previous_session_and_resets_owner_bounds；RC real_crash_candidate_success_and_reap | I/S 分层；真实 owner harness crash 联合场景尚缺 |
| P6-09 | CH launch_change_same_pid_and_shutdown_gate；CI new_launch_rejects_previous_session_and_resets_owner_bounds | I/S；fake 同 PID 换 launch 与真实 child 换代分别标识 |
| P6-10 | CH ack_lost_reconnect_preserves_owner_and_confirmation；CI reconnect_replay_revision_and_epoch_isolation | I/S；旧 tombstone/session 不撤销新连接 |
| P6-11 | CH required_readiness_high_loss_and_local_latch | I/S；initial gate、HEALTHY、HIGH loss、ever-healthy |
| P6-12 | CH optional_low_required_low_and_optional_high | I/S；required 与 criticality 两维 |
| P6-13 | EA p6_component_candidate_and_independent_resource_sources；CH clear_reaggregates_resources_process_recovery_and_terminal | I/S；RM success 与 component/resource 各清本 latch |
| P6-14 | EA p6_component_candidate_and_independent_resource_sources | I/S；memory clear 后 CPU warning 保留 |
| P6-15 | PT crc_silence_hup_and_reopen；T2 protocol/transport 原文件哈希 | I/S；生产 parser CRC oracle，不新增降级阈值 |
| P6-16 | PT crc_silence_hup_and_reopen | I/S；close master 后新 PTY、稳定 symlink，保留 exclusivity |
| P6-17 | CH launch_change_same_pid_and_shutdown_gate / stop_committed_rejects_health_and_bound_heartbeat / private_queue_bound_pending_revocation_and_weak_lifetime | I/S；late ingress/query cache/weak lifetime 分层 |
| P6-18 | CI disconnect_and_process_exit_are_independent；原 RC reap | Partial；EOF→UNKNOWN 与异常退出有源码，反向 exit/EOF 双 gate 排列尚缺 |
| P6-19 | CI restart_runtime_epoch_and_old_instance_report；CH epoch_and_replaced_session_identity | I/S；新 epoch 及旧 session 拒绝 |
| P6-20 | B owner 决策；原 recovery_dependency_tests graph/start/stop 保护 | 条件 N/A（独立 mcu_service）；旧 graph I/S，非 PASS |
| P6-21 | VT real_owner_open_failure_keeps_independent_heartbeat_and_stop；managed 初始化分支 | Partial；ADR-P6-T2-01 阻塞完整产品结论 |
| P6-22 | HARDWARE_VALIDATION H01–H10 | Not Verified；只有硬件证据缺口清单 |
| P6-23 | CI real_peer_bound_heartbeat_and_committed_query；CH bind_exact_pid_launch_start_and_early_retry | I/S；真实同 UID 非 child 与 fake 捕获 peer 分别审计；wrapper/fd delegation 不支持 |
| P6-24 | CH bind_exact_pid_launch_start_and_early_retry | Partial；fake STARTING/old accepted_at 有断言，精确真实 exec 登记竞态 gate 尚缺 |
| P6-25 | CH sequence_duplicate_no_renewal_conflict_and_lower_bound / issue_current_same_revision_and_new_issue_priority / ack_lost_reconnect_preserves_owner_and_confirmation；CI reconnect_replay_revision_and_epoch_isolation | I/S；序号、revision、ACK loss 与 current 后禁止旧 issue |
| P6-26 | CH lease_exact_boundary_query_and_stale_queue；CI unknown_registry_duplicate_bind_and_normalized_owner_failures | I/S；fake clock 等号边界，不以调度睡眠冒充精确时间 |
| P6-27 | CI strict_health_parser_and_direction / integer_null_utf8_and_exact_payload_boundaries / real_peer_bound_heartbeat_and_committed_query | I/S；legacy {}、boot0/null、UTF8、uint64、4096/4097 |
| P6-28 | CI async_ack_queue_cap_tombstone_and_control_fairness；CH private_queue_bound_pending_revocation_and_weak_lifetime；原 device_ipc overflow | Partial；有限输入/满队列/旧输出 cap 有源码，持续混合 flood 延迟组合尚缺 |
| P6-29 | VT issue_recovery_ack_race_and_severity；PT pending_issue 在 recovered 后仍存在 | Partial；coherent record 瞬态有断言，尚缺严格同100ms内生产 UART fault-cycle gate |
| P6-30 | VT applied_ack_rereads_newer_owner_issue_before_current；PT delayed old AckThrough | I/S；旧 ACK 不清更新 issue，ACK 后重新读 owner |
| P6-31 | PT confirmed_boot_change_and_same_id_reconnect；VT stable_revision_boot_zero_and_epoch | I/S 分层；Linux restart 后 owner baseline 无跨实例保证 |
| P6-32 | PT hello_reject_and_missing_path_are_nonfatal；VT real_owner_open_failure_keeps_independent_heartbeat_and_stop | Partial；missing/reject 源码有断言；permission/occupied tty 专项及产品 ADR 尚缺 |
| P6-33 | CI real_peer_bound_heartbeat_and_committed_query；CH stop_committed_rejects_health_and_bound_heartbeat；SM inverse | I/S；未绑定/旧 session 不接受 type4，legacy 谓词保持 |
| P6-34 | CI health_config_legacy_strict_schema_and_wrapper / config_policy_merges_and_conflicts / config_registry_limits_and_legacy_vision_option | I/S；32/33、0..1、显式 producer/policy、旧入口 |
| P6-35 | EA p6_component_candidate_and_independent_resource_sources；CH clear/priority/readiness；原 DSM matrix | I/S；component ERROR 优先 active RM；24/417 不变 |
| P6-36 | 原 phase2_integration::runtime_shutdown_respects_long_grace_period；RC signal_and_observer_failure_during_recovery_cleanup；VT STOP；PT bounded Stop | I/S 分层；原六秒/ECHILD 全文不变；product 在途业务关闭仍受环境限制 |
| P6-37 | P3/P4/P5 所有原入口、COVERAGE、test bodies、注册前缀及冻结 owners | I/S；原保护全部保留 |
| P6-38 | 32-operation/4096/16/weak bounds；P5 service-scale fixtures 不代表 P6 overhead | Partial；1/8/32 health 性能与 latency/CPU 源码及测量均未交付，不声称预算达标 |

## 28 验收逐项结论

S 仅代表指定静态边界通过；Partial/Not Verified 不改写成整项验收完成。

| ID | 关联与当前静态结论 | 状态 |
| --- | --- | --- |
| AC-01 | Runtime no UART/protocol 扫描、冻结 backend、P6-15 | S |
| AC-02 | Runtime 无 SerialPort/tty 与新 UART 依赖 | S |
| AC-03 | harness 引用 canonical 4 sources；PT production UartLink | I/S；行为无证据 |
| AC-04 | Runtime 层 process/link 分离，完整 product 退出耦合仍在 | Partial / Blocking ADR |
| AC-05 | 五状态、唯一 fact、冻结 JSON 与 owner reason validation | I/S |
| AC-06 | pending issue + loss 源码，严格瞬态与端到端 gate 尚缺 | Partial |
| AC-07 | issue ACK/current/新 fault 源码；联合 owner→Runtime gate 尚缺 | Partial |
| AC-08 | changed ID / same ID / boot0 与合理限制 | I/S 分层 |
| AC-09 | peer/launch/epoch/session/seq 提交重验；P6-09/10/19/23/24/25 | I/S；真实登记竞态 gate 尚缺 |
| AC-10 | Runtime 不发 process action；完整 product/multi-reopen storm 尚缺 | Partial / ADR |
| AC-11 | RM 唯一 policy、UART 本地 retry；product ADR 保留 | S 边界 / Partial 产品 |
| AC-12 | required/criticality、ERROR 优先、readiness/全量 evaluate | I/S |
| AC-13 | EA 组合回归与 aggregation 精确逆向还原 | I/S |
| AC-14 | memory critical / CPU warning / RM candidate latch 分开 | I/S |
| AC-15 | 无 MCU DeviceState；原 DSM 源/441 声明空间不变 | S |
| AC-16 | terminal 优先和原 OFFLINE 快照、notifications 断言 | I/S |
| AC-17 | 单 ServiceHealthPolicy、显式 component registry、legacy 空配置 | I/S |
| AC-18 | initial UNKNOWN/CONNECTING gate 与 managed 分支；完整 product ADR | Partial |
| AC-19 | type11 direction/parser/peer/binding；旧 EVENT 出站 | I/S |
| AC-20 | normalized 字段白名单，无 metrics copy/stream | S |
| AC-21 | PTY 源码与 H01–H10 证据边界明确 | S 文档 / 硬件 Not Verified |
| AC-22 | 生产 protocol/transport 原文件哈希不变 | S（本地 T2 基线）；MCU 上游版本未核实 |
| AC-23 | 本次未改 MCU；无 MCU 实际版本一致性证据 | S 本次范围 / Not Verified 版本 |
| AC-24 | P1–P5 owners、旧测试原文、budget/PID/ECHILD/六秒不弱化 | S |
| AC-25 | RM policy readers、reserve、唯一 backend launch、无 component process action | S |
| AC-26 | UART owner 重用、Runtime lease 非 wire timeout | S |
| AC-27 | 本报告显式列38/28/H10、迁移与缺口；联合完成 gate 尚未满足 | S 文档 / Partial 总验收 |
| AC-28 | 五层证据边界明确；静态 PASS 不当运行或硬件结论 | S 文档 |

## 硬件证据缺口

| ID | 未提供的实物证据 | 状态 |
| --- | --- | --- |
| H01 | RK板/BSP/kernel/MCU固件/profile、实际tty/引脚/权限/baud/RS485模式 | Not Verified |
| H02 | 实际唯一 tty owner、产品启动及 process 首心跳 | Not Verified |
| H03 | 实际 STM32 HELLO/STATUS、HEALTHY/readiness | Not Verified |
| H04 | 未供电跨 readiness budget 的实际产品持续心跳与退出语义 | Not Verified；另有 ADR |
| H05 | 物理拔插/重连、PID/launch/预算证据 | Not Verified |
| H06 | 实际 powercycle/reset 与 boot evidence | Not Verified |
| H07 | 板端真实 product crash、RM backoff/reap/replacement | Not Verified |
| H08 | 板端 UDS/Runtime restart/正常 stop、fd/tty清理 | Not Verified |
| H09 | 实际 P5 资源与 service fault 叠加 | Not Verified |
| H10 | 连续运行/cycles/soak、CPU/内存/延迟与实际 shutdown | Not Verified |

RS485 driver/pinmux/DE/turnaround/示波器证据均缺；open 或 PTY 源码不构成电气 PASS。

## 证据等级

| 等级 | 本次结论 |
| --- | --- |
| Implemented | checker 迁移、组合回归、三个 PTY 源码、串行 harness 接线、矩阵及报告 |
| Statically Verified | 日志所列 Python source/contract guards 与 diff 空白检查通过 |
| Host Linux Runtime Verified | 本次仅静态，不提供该层证据 |
| RK3588 + MCU Hardware Verified | 不提供该层证据；H01–H10 Not Verified |
| Not Verified | 表中联合场景/压力/产品退出 ADR/实际版本一致性/硬件门；不能宣称 P6 DONE |

T4 的静态交付已完成；完整 T4 联合验收仍受上述明确缺口和 ADR 阻塞。没有用源码存在替代通过证据。
