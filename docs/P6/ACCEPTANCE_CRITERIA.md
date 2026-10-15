# Phase6 Acceptance Criteria

28项逐项映射原附件。本表为未来验收门，本轮不标runtimePASS。

| # | 条件 | 验证 |
| --- | --- | --- |
| 1 | Runtime不实现UARTprotocol | S范围+P6-15,36 |
| 2 | Runtime不直接拥有SerialPort | S依赖/owner+P6-21 |
| 3 | ExistingUART被复用 | productionharnesssource引用+P6-02,16 |
| 4 | Process/link分离 | P6-03,04,21,32 |
| 5 | normalizedcontract明确 | MCU_HEALTH_MODEL+P6-01,02,27 |
| 6 | 失联可观察 | P6-03,29 |
| 7 | 恢复可观察 | P6-06,30 |
| 8 | reboot正确或合理降级 | P6-07,31 |
| 9 | staleproducer不覆盖newinstance | P6-09,10,19,23,24,25 |
| 10 | loss不无界restart | P6-05 |
| 11 | local与process恢复owner清晰 | P6-04,05,08 |
| 12 | aggregation纳入MCUhealth | P6-11,12,35 |
| 13 | clear全量重算 | P6-06,13,14,35 |
| 14 | 恢复不清service/resourcefailure | P6-13,14 |
| 15 | 无MCU专用DeviceState | P6-35原441矩阵 |
| 16 | OFFLINE语义保留 | P6-35,37 |
| 17 | required/critical明确 | CONFIGURATION+P6-11,12,34 |
| 18 | startupreadiness明确 | P6-01,02,21 |
| 19 | IPC方向/validation明确 | P6-23,24,25,27,32 |
| 20 | 无全metrics复制 | S字段白名单+P6-27 |
| 21 | PTY/硬件边界明确 | P6-16,22+H01–H10 |
| 22 | 无新UARTwire | S原协议文件保护+P6-15 |
| 23 | 无MCU业务修改 | S MCU生产冻结 |
| 24 | P1–P5owner不破坏 | P6-36,37+S |
| 25 | 无第二RecoveryPolicyOwner | P6-04,05,08+S |
| 26 | 无第二LinkRecoveryOwner | P6-06,15,16+S |
| 27 | 文档/任务/矩阵一致 | PACKAGE_VALIDATION+T4最终review |
| 28 | 验证分Host/RK | 每ID分级报告+P6-22,38 |

设计冻结：核心owner/identity/wire/policy/readiness/线程文件边界一致，无未决定的编码设计门。
Host阶段：Runtime baseline+P6全部相关build/CTest/static通过；真实SO_PEERCRED/PTY/fault注入、lease/backpressure/shutdown证据完整。
RK阶段：实际productmanaged接线、STM32连接/powercycle/cable/串口配置/如适用RS485电气与H01–H10；不能用harness/交叉编译/PTY代替。
缺硬件只可报告Host阶段完成/Hardware NotVerified。P4/P5历史静态通过不替代当前baseline运行回归。
