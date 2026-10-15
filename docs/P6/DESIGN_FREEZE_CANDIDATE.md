# PHASE6 DESIGN FREEZE CANDIDATE

本次冻结候选基于 2026-10-03 已连接 GitHub 的三个固定提交，交付架构、文档及后续任务。没有生成 C++、修改仓库或启动实现。附件 Final Output 保留 1–24、26 原编号。

## Frozen Decisions

1. **B：保留现有 VisionArm visionarm_runtime 的 UartLink owner**。Runtime 不接管 tty、协议或控制；独立 mcu_service 本期不创建。
2. process health 与 MCU link health 是两类事实。正常进程遇链路掉线仍发 process heartbeat；只有真实进程异常进入现有 RM。
3. 复用生产 UART 的 framing、CRC、HELLO、STATUS、可靠事务、序列、watchdog、reopen；MCU wire/firmware/control 行为冻结。
4. 五 normalized statuses；一个 generic component_health_changed；boot/recovered 用 reason 与证据表示。HEALTHY 只代表协议链路。
5. owner coherent current + 一个有限 pending issue，writer APPLIED ACK 后确认；保留瞬态故障、新 ACK race 与重连确认规则。不建全历史或 metrics 总线。
6. producer 身份 = SO_PEERCRED exact foreground child + SM launched_generation + Runtime epoch/session + writer 提交重验；P6 producer 的 type4 也必须绑定。
7. type11 HEALTH_REPORT bind/report、新 payload≤4KiB、private operations≤32；原帧/编号/EVENT 方向不变；type9 opt-in components，旧 {} reply 保持。
8. 1s heartbeat、1s report renewal、3.5s observation lease、100ms projection check；lease 监督 UDS 观测，不重算 UART timeout。
9. ServiceHealthPolicy 复用；显式 component→producer/policy；required fresh HEALTHY 参与设备 readiness；没有按服务名字猜默认 policy。
10. link clear 全量重聚合；critical component 优先于 active process recovery；OFFLINE terminal 与 DSM 原矩阵保留。
11. managed UART 等待超时发布不可用而不退出；真实 Start/fatal 仍退出。独立管理 loop、短锁、RAII、原 drain/join 与 Runtime shutdown gate。
12. Runtime fake normalized producer 与 VisionArm production-source host harness 分离；原 12 executable/13 CTest 保留；静态 checker 按明确 oracle 迁移。
13. 顺序 T1 → (T2 与 T3 可分仓库并行) → T4；合同与共享文件修改串行；没有隔离条件则全部串行。

## Unresolved Decisions

| 项 | 已确定边界 | 后续确定时点 |
| --- | --- | --- |
| 实际产品 profile | 机器人推荐 required/HIGH；纯视觉不启用或 optional/LOW；schema 无暗默认 | 部署/联调前填写实际 policy、producer alias 与完整产品 CLI |
| RK 板/BSP/tty/baud/权限/线缆与 RS485 模式 | 全归通信 owner；Runtime 仅传 args，不猜示例 ttyS3 | H01/H02；如 RS485 需要新 transport 适配，另 ADR/任务 |
| Host/RK 的实际性能与基线结果 | 文档给测量目标，P4/P5 历史静态证据不能当运行 PASS | T1 baseline、T4 全回归与 H01–H10 |

这些是部署与验证的未决材料，**健康模型、身份、IPC、聚合及恢复所有权没有待选架构分支**。

## Blocking Issues

**当前设计阻塞：无。** 可以建议后续由用户执行 Thread1；本轮只交付候选，不启动实现。

以下条件出现后必须记录新的 Blocking，不能以本冻结掩盖：
- 实施发现必须改 MCU wire/firmware 才能完成本期范围：停止相关 Phase6 实现，建立新的架构决策。
- baseline build/原回归失败或 producer 无法满足直接 child 身份合同：先定位/处理或明确范围，不能弱化旧断言/身份准入。
- managed 接线必须扩大到业务重构：停止超出授权部分，具体列出依赖。
- 硬件发布前尚未完成 actual profile、RK product、UART/RS485 电气门：**阻止硬件验收/发布**，不等于设计合同未定义。

## Deferred Issues

- 独立通信进程 A、canonical exported UART library、跨进程控制命令 IPC。
- 全量 UartMetrics 诊断查询/stream、长期事件历史、DB/telemetry framework。
- 所有 legacy 服务的 IPC 身份迁移、全局 EventQueue 背压及硬实时调度。
- 每次 MCU 冷启动都可唯一识别的持久 boot counter/wire 扩展。
- 长期链路故障升级 process recovery 的策略（本期没有这种阈值）。
- Camera/V4L2/RKNN/control/actuator、cloud/MQTT/Web/OTA/GPU/NPU 等范围外功能。

## Verification Status

| 等级 | 本轮真实状态 |
| --- | --- |
| Implemented | 设计文档、规则和执行包已生成；P6 软件实现未进行 |
| Statically Verified | 完整仓库阅读与设计包结构/覆盖/链接/JSON 自检；未运行仓库 C++ static checker |
| Host Linux Runtime Verified | Not Verified；未编译/CTest/PTY/真实 UDS 执行 |
| RK3588 + MCU Hardware Verified | Not Verified；没有板端运行或电气证据 |
| Not Verified | 所有本期实现行为、性能目标、RK product 与 RS485 电气门 |

因此冻结候选不是 P6 DONE。实际包自检结果见 PACKAGE_VALIDATION。
