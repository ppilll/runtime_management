# VisionArm Service Integration / Shutdown

实际程序visionarm_runtime；deployment service_name可为control_service或其他静态名字。这个程序既持有业务pipeline也持UartLink；P6不凭空新增control_service/mcu_service executable。

## managed mode

新增可选 --runtime-service-socket PATH、--runtime-service-name NAME，两者必须同时给；启用managed mode必须control_backend=uart且UART编译开启。未给参数保持现有standalone行为（包括原fail-fast等待）；纯视觉无UART部署不注册mcu_link组件。
process heartbeat与link report由managed client loop独立调度；不得在UNKNOWN、NEGOTIATING、LINK_LOST时停发process heartbeat。managed heartbeat周期固定1s，与wire heartbeat200ms不同；Supervisor选timeout>=4s，默认15s保留。

client生命周期在成功解析CLI后尽早建立，覆盖可能较长的camera/RKNN初始化；initial component UNKNOWN，不伪HEALTHY。server尚未登记child时bind重试，绝不从环境自报generation获得信任。新增worker异常通过现有fatal/退出清理，不能吞异常仍伪process健康。

managed UART Start true但未READY：启动client持续上报，现有可配置 readiness budget（默认5000ms，由 VisionArm 原 UART 参数管理）耗尽仅产生startup_not_ready health并继续owner open/re-negotiate；不得Stop/throw/exit。Runtime 不覆盖该通信配置。进程SM RUNNING表示exec成功，不表示MCUREADY。READY后仍复用UartControlSink；其现有not-ready gate和freshness保留，不改算法、frame或actuator。
managed UART Start false若配置非法/worker无法创建：记录真正owner启动失败，按原进程错误路径清理退出；不能把不存在tty的可重试open错误混为Start失败。要测试生产Start实际区分。

仅移除managed路径的UART-ready fatal退出，不改变camera/RKNN等真实fatal行为，不重构业务初始化或算法。owner未ready时现有control sink无法发有效控制是通信事实；设备安全仍由原MCU/业务契约负责。若此接线需要更大business redesign，T2停止记录具体依赖，而非扩大P6。

## 线程/停止顺序

VisionArm接收到既有SERVICE_STOP或SIGTERM后：原停止gate生效→停止新的管理health提交并取消inflight重试→原pipeline producer/consumer drain→保证control sink不再使用link→UartLink.Stop唤醒I/O并join、恢复termios/close→client loop停止/join→销毁引用对象/退出。client可在业务drain期间发送process heartbeat与处理STOP，但不再发link health；不得阻塞等待最后一份ACK才退出。析构/异常路径遵守同一生命周期，禁止detach。

Runtime shutdown：原停止入口先关闭component admission/回复gate、冻结最后device快照、RM cancelAll；保留原Monitor/Timer join、stop_all、最长服务grace+5s/reap逻辑。IPC可以在run返回后按既有main停止；任何health pending在writer shutdown后只丢弃或返回unavailable，不改变DeviceState。
显式STOP/故障/新launch立即撤销该producer session，清本实例freshness，component观测UNKNOWN；旧launched_generation可供reap，但state!=RUNNING不得接纳新health。正常shutdown不由link CLOSED触发OFFLINE。
