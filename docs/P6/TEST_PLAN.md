# Phase6 Testing Matrix

本表是未来实现/验证要求，本轮没有执行C++/CTest/PTY。U=unit/fake clock，I=Linux UDS/process integration，S=static，PTY=Linux userspace，RK=RK3588+STM32。
前22行逐一对应原任务Test1–22；P6-20明确条件N/A而不是PASS。后16行覆盖实际源码发现的身份、瞬态、compatibility与关闭风险。

| ID | 场景 | 可观察oracle | 层 | Owner |
| --- | --- | --- | --- | --- |
| P6-01 | communication owner启动 | initial UNKNOWN→CONNECTING；exec/首processheartbeat分开，requiredDeviceREADY | U/I | T1/T2/T4 |
| P6-02 | MCU READY | 现有ownerREADY映射HEALTHY，freshidentity后满足linkreadiness | U/PTY | T2/T4 |
| P6-03 | cable disconnect | processheartbeat不断、PID/launch不变，ownerwatchdog→UNHEALTHY/HIGH→ERROR | PTY/RK | T4 |
| P6-04 | loss不是processcrash | 无service_failed/heartbeat_timeout/RecoveryRequest | U/I | T1/T4 |
| P6-05 | loss不restartstorm | 持续失联跨多次openretry，launch计数/五预算不增 | I/PTY | T4 |
| P6-06 | localreconnect成功 | issue先ACK再currentHEALTHY，重聚合；同LinuxPID/launch | PTY/I | T4 |
| P6-07 | MCU powercycle/bootchange | 不同validID→peer_reboot；同ID仅reconnect，不声称未reboot | U/PTY/RK | T2/T4 |
| P6-08 | 真实producerprocesscrash | 受管直接child异常退出→原RM，replacement重建UNKNOWN；旧childECHILD | I | T4 |
| P6-09 | 旧generationhealth | 旧launched_generation即使同PID与futuretimestamp也拒绝，不清新fault | U/I | T1/T3/T4 |
| P6-10 | IPC disconnect stale | 旧report/reply/disconnect不影响新session；lease失效UNKNOWN | U/I | T3/T4 |
| P6-11 | requiredHIGH失联 | ERROR，初始UNKNOWN/CONNECTING READY；HEALTHY满足readiness | U/I | T1/T4 |
| P6-12 | optionalLOW失联 | WARNING，不阻required gate；optionalHIGH另测ERROR | U/I | T1/T4 |
| P6-13 | recovery但其他criticalfault | componentHEALTHY不能清processcritical/activeRM；memorycritical仍ERROR | U/I | T4 |
| P6-14 | 恢复但resourcewarning | CPU/memory同source保留，HEALTHY后WARNING | U/I | T4 |
| P6-15 | CRCburst | 原parser计数/丢坏frame，Runtime无CRC解析；未触发owner降级不虚DEGRADED | PTY/S | T4 |
| P6-16 | PTYdisconnect/reconnect | closemaster→HUP/读错；重建PTY+稳定symlink、ownerreopen成功 | PTY | T4 |
| P6-17 | shutdown时linkevent | admissiongate后latehealth/reply/closed无state变化，join/weaklifetime | U/I | T4 |
| P6-18 | producer异常退出 | EOF/HUP先UNKNOWN，原reap独立processfailure，事件次序均测 | I | T3/T4 |
| P6-19 | Runtime重启fresh身份 | epoch换代拒绝旧session，即使新实例generation数字相同；不adoptorphan | I | T3/T4 |
| P6-20 | 独立mcu_service依赖顺序（条件项） | 本包B方案N/A；保留原SM graph/start/stop回归，未来A启用必须增加commandIPC与依赖测试 | S/I | T4 |
| P6-21 | Bownerdecoupling | VisionArmmanaged无MCU超过readybudget仍活；进程心跳正常+linkunhealthy，实际productRK验证 | Harness/RK | T2/T4 |
| P6-22 | RK3588+STM32 | 硬件章H01–H10完成证据；不可用则NotVerified | RK | T4 |
| P6-23 | peercredentials伪造 | 另一受管service同UID自报producer，未受管PID、wrapper、旧conn早于launch拒绝 | I | T3/T4 |
| P6-24 | bind期间launchrace | child早connect/SM登记未完成→RETRY_BIND；重连成功；无generation自贴 | U/I gate | T3/T4 |
| P6-25 | seq/revision/replay | 重复samepayloadACK但不renew；conflict/低seq/revision回退拒绝；same revision 续租合法；issue APPLIED 但 ACK 丢失重连确认、current 已应用后旧 issue 禁止回滚 | U/I | T1/T3/T4 |
| P6-26 | reportlease边界 | now=expiry−1仍fresh，等于expiryUNKNOWN；query不推进；不是wiretimeout | U clock/I | T1/T3/T4 |
| P6-27 | RPC格式兼容 | 新direction/version/keys/type/null/boot0/uint64string/UTF8/大小，旧{}GET_HEALTH完全保持 | U/I | T3/T4 |
| P6-28 | 有界背压 | flood/full/未读output/断连tombstone/epollfairness；issue不丢，oldclients可服务 | I gate | T3/T4 |
| P6-29 | 瞬态loss不能漏 | LINK_LOST→NEGOTIATING→READY全部发生在100ms采样之间，pendingissue先应用 | U/PTY | T2/T4 |
| P6-30 | ACK期间新fault | 读取issue→新fault→旧ACK，AckThrough只擦旧revision，保留newissue | U gate/PTY | T2/T4 |
| P6-31 | bootID证据降级 | 首次unknown/跨reopen不同ID/相同IDcollision/Linuxrestart均不虚reboot或未reboot | U/PTY | T2/T4 |
| P6-32 | open/HELLO失败 | 不存在/权限/占用tty、明确HELLOreject与长期无STATUS，managed不exit；Start真失败另测 | U/PTY | T2/T4 |
| P6-33 | processheartbeat身份 | P6producer绑定前/旧session/type4自报名不能renew；原合法heartbeat仍有原周期/timeout | I | T1/T3/T4 |
| P6-34 | 配置 | 旧single/services/P5monitoring；healthdefaults/duplicates/unknown/missing/conflicts/0..1components/timeout | U | T3/T4 |
| P6-35 | priority/DSM | componentcritical+RMrecovering仍ERROR；clear到READY/RECOVERING/WARNING/OFFLINE；原441矩阵 | U/I | T1/T4 |
| P6-36 | shutdown回归 | 原六秒/PID/ECHILD、signal/observerthrow、owner/client在途I/Ojoin、无detach | I | T4 |
| P6-37 | P1–P5回归 | 所有原12executable/13CTest、budget/token/manual/dependency/resourceunknown/source准入保持 | S/I | T4 |
| P6-38 | normaloverhead与观测限制 | 1/8/32service、health队列有限、clientCPU/latency；READY不证明actuatorsafe | I/RK | T4 |

## Host Linux策略

Runtimeunit只注入normalizedfact/peer/snapshot/clock，不链接UART。使用受管fake producer真实socket/崩溃与SMlaunch/reap覆盖identity；新测试不得让父test伪造peer PID代替真实SO_PEERCRED场景。fakeclock证明lease精确边界；races用有deadline和release-before-join guard的gate，不用sleep猜正确性。

VisionArmhostharness以standaloneCMake直接编译生产UartLink/protocol/transport/client，引用原路径而非copy。用openpty（必要时libutil）、slave交给真实SerialPort、master交给最小fake MCU。
fakeMCU只负责HELLO_ACK、HEARTBEAT→STATUS、可选PING→PONG以及bootid/seq、faultdrop/corrupt/disconnect。优先链接VisionArm现有codec或显式固定MCUportableCcodec；不复制常量/parser/firmware到Runtime；不实现control/gimbal/actuator。fixtures只有合法/坏帧用于测试，原decoder作为oracle。
为模拟物理断线先close master使slaveHUP，再重建PTY与原固定symlink→新slave；owneropenretry访问稳定路径。仅丢STATUS测试协议silence（与PTYHUP不同）。保留raw8N1/TIOCEXCL实际行为，不用test强绕exclusive掩盖资源竞争。
end-to-end将通信-only harness作为SM直接child，真实type11/type4、Runtimeaggregate、订阅/opt-inquery、其他resourcefacts；证明managedintegrationlibrary，不能把harness代替真实productcamera/RKNN接线的板端证据。
现有real-boardwire_qualification仍物理工具，不改成simulator；host tests不操控physicaltty或执行器。

## 既有回归证据落点

- device_ipc_tests::running_health_requires_confirmed_heartbeats：exec非首心跳confirmed。
- producer_overflow_disconnect_and_resubscribe / unread_client_output_bound：保持原限额与weak生命周期。
- runtime_core_tests::monitor_instance_reuse_and_cleanup_generation：故障generation与launchedtoken分开。
- recovery_coordination_tests::real_manual_restart_and_stale_instance_facts：同PID旧instance事实拒绝；新增health同等gate。
- untrusted_recovery_ingress_cannot_clear_resource：public新事实不得绕trustedadmission。
- p5_resource_pressure_does_not_restart_running_service：link同样断言PID/budget/launch不变。
- event_aggregation_tests::resource_critical_downgrade_and_clear / p5_policy_fifo_downgrade_and_clear_reaggregate_all_health：clear全量重算。
- 原test_runtime_shutdown_respects_long_grace_period独立6秒、ECHILD和全部注册不变。
静态脚本的source存在不是这些断言已执行。

## 后续命令（本轮未执行）

Runtime：
```text
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 -B tests/phase3_validation_static_check.py
python3 -B tests/phase4_static_check.py
python3 -B tests/phase5_static_check.py
python3 -B tests/phase6_static_check.py
git diff --check
```
P6checker创建并按CHANGE_LIST迁移后才可运行。CTest保持默认串行，processglobalsignal测试不随意-j。
新VisionArmhostharness建议入口（T2/T4创建后可用）：
```text
cmake -S tools/test_apps/runtime_supervision -B build/p6-host -DBUILD_TESTING=ON
cmake --build build/p6-host
ctest --test-dir build/p6-host --output-on-failure
```
选择MCUportablefixture时另给固定MCUcheckout的VISIONARM_MCU_SOURCE_DIR，不能隐式下载latest。
VisionArm产品使用已读visionram/README的./build.sh与现有SDK overrides；目标依赖RKNN/RGA/MPP等，不能声称单关optionalflags即可x86完整构建。交叉编译不执行ARMbinary，不算RKruntime。

## 验证等级与预算

每ID记录Implemented / Statically Verified / Host Linux Runtime Verified / RK3588 + MCU Hardware Verified / Not Verified。
报告命令、exit/stdout/stderr、kernel/arch、三个commit、profile、fault脚本、日志、PID/launch/restart_count、component/device观测次序。
目标：常态1report/s+1processheartbeat/s；issue到Runtime提交正常host/RKp95<300ms（不包含owner1000mswireloss检测）；每componentpendingO(1)，privateoperations<=32；managedclient平均CPU增量<1%单核等价。全部是待测目标，无硬实时承诺，不用host结果推算RK。
本设计包自检只验证文档/覆盖/链接，不认证运行行为。
