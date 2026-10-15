# Communication Ownership

## 决策：B 保留 VisionArm 现有 owner

| 维度 | A 独立 mcu_service | B 现有 visionarm_runtime 保留 UartLink |
| --- | --- | --- |
| 开发速度/改动 | 抽库、新 executable、control command IPC、部署，慢 | 仅 projection/client/managed 接线，快 |
| 架构一致性 | 隔离通信，但需改现有 UartControlSink 到跨进程命令 | 与真实 composition 一致，Supervisor 只读 facts |
| 重复风险 | 必须抽 canonical library，复制不可接受 | 保留生产协议；工具重复作为已有债务记录 |
| Runtime边界 | 合理，Runtime只管理进程 | 同样合理，Runtime不建UartLink |
| 故障隔离 | 摄像/推理崩溃不必丢UART，优势 | VisionArm崩溃导致UART一起停止，明确代价 |
| 部署/维护 | 多二进制、更多UDS与跨repo版本，成本高 | 单现有产品进程 + Runtime，新wire合同小 |
| 测试 | 独立进程易测；命令IPC也须测 | 通信-only harness易测；实际product接线须RK验收 |
| RK3588/资源 | 额外进程/IPC，仍需driver验证 | 少一个进程，新增一个client loop；实际开销未测 |
| 展示价值 | 进程隔离清晰但范围大 | 两层健康/身份/故障注入体现管理边界，足够 |

推荐 B。control_service 只是可能部署别名，不是仓库已有独立程序；实际 executable=visionarm_runtime。别名可由 Supervisor service config 选择，producer授权必须按该配置，不靠默认名字。

A 以后若必要：将生产 UART 在 VisionArm 内抽为 canonical exported library；独立服务链接它，现有控制请求另有命令 IPC，tty 同时只能由一方拥有。仅链接 visionarm_control 会携业务依赖，工具库目前另有代码副本，不能直接宣称已解决 production/library ownership。不推荐复制源码。A 需要新的 ADR/预算/命令身份/业务接线审查，延期。

方案 C RuntimeManager 创建 UartLink 违反 Supervisor 管理边界且引入 tty/protocol/vendor依赖；本轮禁止。

## 唯一 owner 表

| 事实/行为 | owner |
| --- | --- |
| tty/open/baud/termios/RS485、frame/CRC/HELLO/ACK/sequence/boot、wire watchdog/reopen | VisionArm UartLink + SerialPort |
| normalized projection、待确认故障摘要 | 同一通信 owner 的窄观测层；只映射原决策，不增加 wire timer |
| 健康上报/进程心跳/UDS retry | VisionArm managed client loop |
| peer身份/帧校验/连接I/O | Runtime IPC；健康接受最终由 writer 决定 |
| 当前 component事实、session/lease、Device aggregate决策 | Runtime writer + ServiceAggregation |
| PID/ServiceState/launched_generation/signals/reap | ServiceManager/原ProcessSupervisor |
| retry策略/预算/terminal | RecoveryManager |
| DeviceState 写入/合法转移 | DeviceStateManager |
| procfs measurement /资源迟滞 | ResourceCollector / Monitor |
| actuator/remote stop/control validity | MCU/原业务 owner；P6不改 |

required/criticality 表达设备管理影响，不能让 Runtime 控制执行器安全状态。HEALTHY 仅协议链路可用。
