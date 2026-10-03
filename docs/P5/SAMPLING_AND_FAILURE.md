# Sampling / Failure / Performance / Persistence

## 调度

默认2s，JSON整数1..60s。使用既有1s timerfd ticks驱动 Monitor worker；资源due按真实 steady_clock now检查，不用排队的 producer timestamp 计算利用率。不新增 worker。
worker 每轮先 Monitor.check(Clock::now()) 保持心跳优先，再资源 due。首次合法 worker tick可采样，CPU baseline尚不产生数值，memory立即可用。

一个周期：system两文件→SM identity try snapshot→O(N) process stat→批量 try revalidate→提交 current snapshot→Monitor对valid system metrics产出facts→既有queue。
若本轮失败仍推进next_due；不得tight retry loop。结束时 next_due=completion_now+interval，错过周期只采一次，不catch-up burst；不承诺严格等距。CPU delta使用真实counter差，process用实际elapsed。

Timer→Monitor的 health_check 最多一个 pending：一个小原子pending flag coalesce，worker取出时释放；shutdown envelope不可coalesce或丢弃。Monitor.check 本身用现有 next_miss 计算错过周期，不重新实现 heartbeat。此处只限制定时tick积压，不重做全局 EventQueue/backpressure。

## Shutdown/lifetime

原 run 关闭入口先 shutting_down/cancelAll，设置 running=false；Timer退出后投monitor shutdown并join，再关timerfd及停止/回收服务。P5保留次序。worker在采集前、每个process之间、发布snapshot/facts前检查停止gate；停止后丢弃在途结果，不能给shutdown后的设备快照发布clear或新warning。
正常关闭保留最后 DeviceState，不人为设OFFLINE。collector、reader、SM、Monitor、cache 在worker join前保持存活，lambda不能捕获已销毁局部对象。保留异常清理/observer throw/独立6s shutdown回归。

已经处于内核不可中断read时无法靠gate立即退出；procfs生产路径和文件大小限制减少通常开销，但join没有硬实时保证。禁止detach、销毁在途collector或伪造shutdown成功。test gate等待必须有release guard以免失败测试卡死。

## Failure Semantics

| 情况 | snapshot / baseline | policy / DeviceState |
| --- | --- | --- |
| /proc/stat read/permission/parse失败 | cpu unavailable；error++；清previous | 不评估CPU、不发clear/ERROR；memory独立 |
| counters regression/wrap/delta0 | cpu unavailable；合法current重建baseline | 不用无效数字推进CPU状态 |
| /proc/meminfo失败/缺字段 | memory unavailable；error++ | 保留该source已确认pressure latch |
| process ENOENT/ESRCH | not_present；清该identity previous | 不调用SM失败或RM；原reap独立推进 |
| process EACCES/parse/limit | unavailable；清该process previous | system不受影响，不产生资源设备critical |
| generation/starttime mismatch | identity_changed；丢弃result/previous | 不能转为“新服务资源” |
| identity try_lock失败 | process_scan unavailable；不提交process新previous | 不等待SM launch，不影响system policy |
| 首次CPU / 失效后首次合法CPU | warming_up | memory仍评估；不能以0%clearCPU |
| unexpected worker exception | 日志fatal，投既有shutdown；后续join清理 | 不直接写ERROR，不吞异常继续伪正常 |

CPU/memory各自连续错误计数成功重置。首次失败warning日志；后续持续失败最多每30s一条聚合日志；恢复日志一条。process日志按周期聚合数量，避免N个进程每次刷屏。没有monitor-health复杂状态机。

未知不是正常。已激活resource latch在失效期间保持到有效恢复样本；可因此保守保持WARNING/ERROR，诊断snapshot显示unavailable/stale及age。第一次监控即失败且无既有latch，DeviceState仍仅反映已知健康，不能声称“资源已正常”。未来若要求未知→设备降级，另审独立availability fact，不复用cpu_monitor伪造压力。

## Performance Budget（目标，未测量）

- 2s默认：每周期2+N次proc读取；一个SM批量try snapshot与一个批量try revalidate；没有每服务线程、N次全量list查询或O(N²)匹配。
- 配置1s响应更快、噪声与读取量翻倍；5s负担更小但clear与异常发现更迟。2s在嵌入式轻量监控/演示间折中。
- 参考验收N=1/8/32；目标N<=32时正常周期p95<20ms、p99<50ms，Runtime进程增量平均CPU<1%单核等价。目标需host与RK3588分别测量，不是现有性能事实。
- identity/cache/policy 锁目标通常<1ms；proc I/O和日志/queue sink在锁外。SM try锁失败计数纳入测试，不能承诺既有SM同步launch锁已变短。
- 文件上限64KiB/4KiB；current rows + one previous per active launch，内存O(N)。状态变更才发资源事件，正常稳定期只首次每source一条初始化clear。
- 单次周期>interval记录节流overrun，下一次从完成后排期；不并行采集补赶，不因超时restart服务。
- 系统procfs一般轻量但slow I/O仍可延迟同worker心跳；若目标实测不满足预算，再做线程隔离ADR，不在P5暗增线程。

Memory Only：current snapshot、system previous、每process previous与少量policy/error/due元数据。无历史ring buffer、trend、SQLite、InfluxDB、TSDB或外部exporter。

跨线程停止gate使用atomic（可复用running并另设resource stop gate在关闭入口生效），不能从worker无锁读取writer-only shutting_down_。queue只保留停止前的已入队facts，停止边界不能撤销旧排队；writer进入shutdown后不再dispatch这些资源facts，保持最终DeviceState。reader异常请求shutdown后也应关闭采样gate。计数器作饱和递增，不因错误累计溢出。
