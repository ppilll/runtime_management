# Local Communication Recovery vs Runtime Process Recovery

| 故障 Case | 检测者 | Runtime所见 | 恢复者 / process recovery |
| --- | --- | --- | --- |
| A cable disconnected | UartLink I/O/watchdog | UNHEALTHY，process仍可HEALTHY | UartLink reopen/HELLO；不触发RM |
| B MCU power off | UartLink无有效响应/watchdog | 同A，原因不声称已证明电源坏 | 同A |
| C MCU reboot | UartLink/保留boot锚点 | peer_reboot DEGRADED或同时loss的UNHEALTHY→HEALTHY | owner重新协商；不重启Linux |
| D tty open失败 | SerialPort/UartLink | UNHEALTHY open_failed | owner定时open retry；不RM |
| E VisionArm真实crash | 原Supervisor/reap/SM | process失败+componentUNKNOWN | RM按原restart_policy/预算/deadline |
| F negotiation reject/timeout | 原owner / managed原readiness预算 | UNHEALTHY hello_rejected/startup_not_ready | owner原HELLO/reopen；不RM |
| G temporary corruption/CRC | 原parser；response/watchdog | 通常状态不变，只有owner降级才发fact | 原parser resync/owner；Runtime不解析CRC |
| H Runtime IPC disconnected | service client/Runtime IPC | UNKNOWN observation_unavailable，不能伪linklost | client UDS reconnect/bind；不RM |
| UART worker真实不可恢复失败 | owner真实fatal处理/进程退出 | 先component不可用，退出后process失败 | 真实退出由RM；禁止“失联N次”主动exit升级 |

HEALTH_REPORT不产生FailureType/RecoveryRequest，不调用restartService/start/stop/kill，不占five lifetime reservations。DeviceERROR没有“ERROR就restart”回调。不会串口仍断→重启→仍断→重复重启。

Runtime服务心跳是独立processloop活性；若它真停止，既有heartbeat timeout可触发RM，这与wire loss并非同一条件。
UDS reconnect只恢复报告通道，不能调用UartLink.Stop/Start来绑Runtime连通性。Runtime重启也不得强制MCU reset。

P6默认无link故障升级到process restart阈值；将来若产品要求长期故障升级，必须另ADR提供不可恢复进程故障证据、RM唯一policy、有限预算、身份/取消、错误归属与storm实验。不能把这种扩展藏在adapter。

shutdown与OFFLINE仍由P4单owner取消和生命周期清理；P6仅撤销component session。
