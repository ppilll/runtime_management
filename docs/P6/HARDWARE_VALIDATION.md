# RK3588 + STM32 Hardware Validation

全部待执行；HostPTY只能证明userspace stream/protocol/lifecycle，不能证明UARTdriver/device tree/pinmux/RS485DE或电气层。

| ID | 真实板端步骤 | 必留证据 / 通过条件 |
| --- | --- | --- |
| H01 | 固定RK板型/BSP/kernel/MCU固件commit、供电、线缆、转接器与profile | 实际device path/引脚/权限/baud/termios；不沿用示例ttyS3猜配置 |
| H02 | 检查tty存在、权限、无其他owner，启动实际VisionArm | open/configure成功、唯一ttyowner，Runtime/client/ownerPID；process首心跳确认 |
| H03 | 连接STM32、原HELLO/STATUS运行 | ownerREADY→HEALTHY，requiredgateREADY→RUNNING；不声称actuatorsafe |
| H04 | 启动时MCU未供电/未接线，跨5sbudget观察 | managed VisionArm保持活、heartbeat持续、linkUNHEALTHY、DeviceERROR或profileWARNING；无额外launch |
| H05 | 物理拔线/重新接线，多次重复 | loss先可见，本地reopen/HELLO、恢复全量evaluate；PID/launch/budget不变 |
| H06 | MCU断电/上电或reset | 不同bootidpeer_reboot；相同ID记录检测限制但链路降级/恢复合理；Linux不被重启 |
| H07 | 真实VisionArm进程crash与恢复 | 原RM预算/backoff/generation/reap，newcomponentunknown→fresh；不能混MCUreset |
| H08 | Runtime IPC断开/Runtime重启/正常stop | UNKNOWN观测失效、session/epoch隔离、laststate冻结/join/reap；无socketfd/tty泄漏 |
| H09 | 与P5 CPUwarning/memorycritical及其它servicefault叠加 | linkHEALTHY不清别的fault；OFFLINE终结仍保持 |
| H10 | 连续运行、fault cycles、退出和预算 | 建议正常60s+10次线缆/供电cycle+600ssoak；无storm、内存增长/CPU/通知延迟记录，原6sshutdown回归 |

RS485使用时附加硬门：确认Linux转接器是自动方向还是kernelRS485；若需TIOCSRS485，检查实际driver能力、device tree/pinmux/收发极性/DE配置及读回。当前生产SerialPort没有此ioctl，不能把open成功当RS485配置成功。
用示波器/逻辑分析仪核实实际DE上升/最后stopbit/TC后下降、turnaround、终端电阻/偏置/接地/电平、碰撞与噪声下真实双向收发；记录MCU GPIOD7与Linux端的测量。MCUcallback有DE控制代码只属source证据。
如实板确认必须新增LinuxRS485选项，修改ownertransport属于独立硬件适配任务，记录ADR/板端证据；不能把这个未决适配偷偷放T3 Runtime，更不能改wire/MCU业务。

没有板端环境时报告Not Verified及缺的材料。H01/RS485未确认会阻止硬件发布验收，不阻止本包已定的健康模型/IPC实现设计。电气验证和启动productprofile必须完成才能称P6硬件完成。
