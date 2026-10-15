# Current Repository Assessment

## 固定来源与阅读范围

| 仓库 | ref / commit | 当前事实 |
| --- | --- | --- |
| ppilll/runtime_management | master / f8507027c87a9ae9899f51da708f8d469552c473 | P5 源码与静态审查已在 HEAD；P5 README 是设计时点描述，不代表当前仍未实现 |
| ppilll/VisionArm | main / 05148303c5ec5bba0ee89e707ab340e94e8aa3e1 | product executable 是 visionarm_runtime；UART 在 runtime_components 中创建 |
| ppilll/VisionArm-MCU | main / 92d8f45bc9c7df55f6e2abea4b4abfeb090a3e3d | MCU 实际路径有 Visionarm_MCU/ 前缀；V1 portable codec 与 App core/transport 分层 |

读取了 P0–P5 的全部 95 份 Markdown/text 文档；重点指定十一份设计/验证文件完整审阅。读取 RuntimeManager、ServiceManager、ServiceAggregation、DeviceStateManager、RecoveryManager、IPC Manager、ConfigManager、event 的头/实现及相关资源模块；审查现有测试注册、身份/恢复/聚合/IPC/关闭断言。完整阅读生产 UART link/protocol/transport/sink、tools/test_apps/uart 的头、源、两个工具和构建；完整阅读 MCU portable/mcu_uart 与 App/visionarm_uart。manifest 记录取回与重点证据，不将取回等同执行。

仓库树没有根 AGENTS.md；P4/P5 历史报告引用 codex_package，但当前树没有这两个目录。不能声称读到了不存在的提示词；本包参考存在的 P2/P3 短提示词风格，生成独立 P6 任务。P0/P1 和 P2 的旧 recovery 描述已由 P4 实际实现替代。P4/P5 validation_review 明确仅静态检测，附件 DONE 不能作为 Linux/hardware 运行证据。

## Runtime 核查

| 实际位置 | 判断 / P6 必需改动 |
| --- | --- |
| service_manager.cpp::transition/start/process_exit | status.generation 在 launch/fault/STOP 改变；launched_generation 在 child 回收前保留。producer 用后者，不能用故障 generation |
| recovery_manager.cpp::submit/reserveLocked/tick/terminal | RM 唯一 retry/backoff/deadline/terminal owner，五次 lifetime reservation；P6 不增加 LinkLost FailureType |
| runtime_manager.cpp::post/handle_event/run | public post 目前仅允许资源事实；P6 新建窄私有 identity admission，不能让任意 post 构造 trusted health |
| service_aggregation.cpp::add_service | ServiceHealthPolicy 已有 high/medium/low + required，programmatic policies 可覆盖；main 默认空 options，JSON 没有 health policy |
| service_aggregation.cpp::evaluate/reconcile | OFFLINE terminal > critical resource > critical process > warnings > readiness；P6 插入 component critical/readiness，保持全部重算 |
| device_state_manager.cpp | 现有七 DeviceState，24 允许 / 417 拒绝 target triples；P6 不增状态和表项 |
| ipc_manager.cpp::Client / service heartbeat | 自报 service_name，未 SO_PEERCRED/session 绑定；迟到 heartbeat 身份不足。P6 producer 必须补强，不能把此风险继续延期 |
| frame.hpp/frame.cpp | 10 字节 LE header，64KiB JSON，types1..10/255；EVENT(type5) 向外，入站拒绝 |
| config_manager.cpp | integer JSON parser，single/services root，RuntimeConfig/monitoring，1MiB上限；P6 static health 根对象最小扩展 |
| event.cpp::EventQueue | 全局 FIFO 无总容量；P6 输入必须另有有限 admission，不宣称全局 backpressure 已解决 |
| tests/CMakeLists.txt | 12 executable /13 CTest，包括独立六秒 shutdown 回归，全部保留 |

GET_HEALTH 现有 service rows 是进程状态/PID/accepted heartbeat；successful exec 并不等于首心跳已确认 healthy。P6 保持这一定义；设备 readiness 的 required link gate 与 process start success 不混合。

## Existing MCU Communication Capability Assessment

Linux 已有 SerialPort：非阻塞 tty、raw8N1、baud、TIOCEXCL、termios restore；没有 TIOCSRS485。UartLink 有 poll+eventfd I/O thread、parser、message codecs、sequence tracker、boot tracking、heartbeat/status、PING/PONG、reliable ACK/NACK、watchdog、open retry/reopen、metrics、mutex snapshot。
默认 open retry1000ms、HELLO500ms、wire heartbeat200ms、response150ms、watchdog1000ms。这些属于通信 owner，不进入 Runtime timers/config。

生产 UART 在 visionarm_control 静态 target 内，并非干净 exported UART library。tools/test_apps/uart 已有独立的同功能源树与三个 library targets；两份 uart_link.cpp 仅 include 路径不同，替换后文本相同。P6 不能增加第三份协议。现有 standalone/wire_qualification 是物理板端工具；并未注册自动 PTY unit tests。wire_qualification 明说 real-hardware only。

生产 runtime_components 的 WaitForUartReady 超时后 Stop 并 throw；若受 Supervisor 管理，拔线可能让正常通信进程退出并重复恢复。这是 managed-mode 必修接线。FAILED enum 存在，但正常 I/O 路径不常设置；Start false 不能从 enum 名字臆推链路故障。

MCU：flag0x7E/escape0x7D/xor0x20，V1/header16/CRC2/payload128/raw146/encoded294；CRC16-CCITT-FALSE，HELLO1/HELLO_ACK2/HEARTBEAT3/STATUS5/ACK8/NACK9/PING10/PONG11；control4/remote-stop6/clear7 保持原 owner。
HEARTBEAT 是 Linux→MCU，MCU回复 STATUS，不能假定 MCU周期发送 HEARTBEAT。MCU core WAIT_HELLO/READY/LOST，link watchdog1000ms/control200ms；USART2、PA2/PA3、GPIOD7 DE，HAL TxCplt 后回 receive。这个事实不能证明 Linux RS485 driver/adapter 已配置。

## 能力缺口

1. 当前 snapshot 不携带可靠 transition revision/原因/episode，LINK_LOST 很快转 NEGOTIATING，100ms轮询可能漏。
2. reconnect 清 peer baseline；boot change counter 不保证跨断线重启可见。
3. MCU UID+tick+DWT boot_id 不保证每次冷启动唯一；合理降级是同ID只确认重连，不能宣称未重启。
4. 未有 Runtime health client、trusted session、component registry/readiness/config。
5. 产品 service alias、实际 device/RS485 driver、电气连线和生产 profile 需部署确认；不得按 control_service/mcu_service 名称猜。

本轮没有编译、运行仓库 checker、CTest、PTY 或硬件；只静态阅读和设计包自检。
