# Process Health vs MCU Link Health / Normalized Contract

## 两个维度

| process layer | link layer | 管理含义 |
| --- | --- | --- |
| RUNNING + 已接受fresh process heartbeat | UNHEALTHY | 进程健康，线/协议不可用；本地reopen；不RM restart |
| FAILED / heartbeat timeout | UNKNOWN | 没有可信当前producer；具名进程故障由RM处理 |
| RUNNING + heartbeat正常 | DEGRADED / peer_reboot | MCU重启或协商变化，不是Linux crash |
| RUNNING + heartbeat正常 | UNKNOWN / report_expired | 上报观测失效，不能猜MCU物理掉线；不制造process fault |
| RUNNING但未首heartbeat | 任一 | GET_HEALTH process仍UNKNOWN；component不替代它 |

五状态足够：UNKNOWN无可信观测；CONNECTING首次打开/协商；HEALTHY owner READY；DEGRADED仍有通信但owner确认质量下降或peer重启重新协商；UNHEALTHY owner已确认不可用。recovered、reboot是reason/episode metadata，不增加MCU_RESTARTED/event enum或DeviceState。

## 最小事实字段（设计接口，无源码）

| 字段 | 来源/含义 |
| --- | --- |
| component | 静态授权key，本期 mcu_link |
| status | 五状态之一 |
| reason_code | 固定枚举：initial、opening、negotiating、ready、response_timeout、link_lost、open_failed、hello_rejected、startup_not_ready、peer_reboot、recovered、observation_unavailable、report_expired、owner_stopped |
| reason | <=256 UTF-8 bytes诊断；不是策略输入，不能塞raw STATUS |
| projection_revision | uint64十进制字符串；owner每次语义变化递增；续租可不变 |
| link_epoch | uint64十进制字符串；owner每次首次启动/重新建立协议会话递增，不能当SM或RM generation |
| peer_boot_id | null或uint32；当前已验证peer ID，0是合法值不能冒充null |
| received_at | Runtime server monotonic接收时刻，内部字段，不信任producer wall-clock |
| identity | server导出的service_name/PID/launched_generation/runtime_epoch/session_id |
| report_seq | session内严格递增；与UART wire_sequence完全无关 |

不用 producer timestamp排序或认证；此期不发送 last_rx_age，Runtime不据它重算UART timeout。query用received_at派生report age，仅衡量管理链路新鲜度。全部UartMetrics和last_status保留在owner，本期无metrics查询/stream。

## 原状态映射和 episode 保持

| UartLink 状态/已确认原因 | normalized |
| --- | --- |
| CLOSED尚未启动 | UNKNOWN initial |
| OPENING/NEGOTIATING首次尝试 | CONNECTING |
| READY | HEALTHY ready/recovered |
| DEGRADED / response-window timeout | DEGRADED response_timeout |
| LINK_LOST/watchdog、I/O断线/open失败 | UNHEALTHY link_lost/open_failed |
| boot ID变化进入NEGOTIATING | DEGRADED peer_reboot；若同时loss，以UNHEALTHY保守覆盖 |
| HELLO明确拒绝 / managed启动等待预算耗尽 | UNHEALTHY hello_rejected/startup_not_ready；只在VisionArm判断，不在Runtime计时 |
| loss后OPENING/NEGOTIATING | 维持UNHEALTHY本故障episode，直到owner新READY |
| STOPPING/CLOSED有序停止 | 禁止health继续影响shutdown；非预期owner停止且process继续则UNHEALTHY owner_stopped |
| FAILED | 仅状态观测；不能直接推出进程failure。真实owner不可恢复fatal通过正常进程退出报告 |

没有CRC burst→DEGRADED新阈值：现有parser错误只增metrics；只有owner现有response/watchdog决策能改变health。DEGRADED的恢复也以owner现有READY判断为准，Runtime不加N次心跳策略。

## 不能仅靠当前 GetSnapshot

LINK_LOST常迅速转NEGOTIATING，轮询会漏掉；boot变化也可能在下次读取前重新READY。冻结窄owner projection：
- 与现有决策一起维护 current、projection_revision、link_epoch、一个pending_issue高水位摘要。
- pending_issue保留自上次确认后的最严重DEGRADED/UNHEALTHY（同级保留最近原因/boot证据和最新issue revision）；不是无界事件历史。
- client先发送pending_issue，收到Runtime writer应用ACK后才能确认其revision；再发送当前HEALTHY。新issue若发生在读取/ACK之间，revision更大，AckThrough不能擦除它。
- IPC断连不得确认pending；新session先上报待确认issue，再上报latest。同一producer重连保留boot锚点/revision/epoch。
- 多次快速fault coalesce可只保留最严重一条和latest，保证“发生过不可用”可观察，不保证完整每次抖动历史。detail metrics仍供owner诊断。
- owner只增加观测字段/短锁，不新增协议解析、timeout、reopen调用。接口建议 ReadHealthProjection / AckHealthIssueThrough；callback不做socket I/O。

## MCU reboot

owner记录同Linux实例内last confirmed boot（与parser当前baseline分开）。有效旧ID→不同有效新ID，是reboot evidence：发布peer_reboot并重新协商；确认READY后恢复。有效ID相同的disconnect/reconnect只确认重连，不证明未重启；旧ID未知或Linux实例重启，只建立新baseline，reason negotiating/recovered，reboot证据unknown。
MCU GenerateBootId 不是持久唯一boot counter，冷启动可能碰撞；碰撞情况下仍根据owner失联/协商合理降级，不改MCU firmware来伪造P6保证。没有失联且同ID reboot可能无法检测，列明能力限制。若产品要求每次boot绝对可识别，需要wire/firmware ADR并阻塞该扩展实施。
