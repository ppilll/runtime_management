# Architecture Decisions

每项均依据实际源码。评估维度：开发速度、稳定性、维护成本、Linux/RK3588适配、资源开销、可测试性、后续扩展与面试展示价值。以下判断不是硬件基准结果。

| ADR | 方案A：优点 / 缺点 | 方案B：优点 / 缺点 | 推荐及理由 |
| --- | --- | --- | --- |
| 01 Collector | Monitor直接读proc：文件少、早期快；OS解析/heartbeat/policy耦合，测试维护变难 | 轻量Collector+typed snapshot：分离测量/策略/状态、fixture易测；增加少量接线 | B，原生Linux接口、O(N)内存，扩展与展示清楚，不引metrics framework |
| 02 Scheduling | 新resource thread：隔离慢I/O；线程、取消、join、锁成本增加 | 复用Monitor worker：线程数不增、已有1s tick、保护writer；I/O可拖延同worker heartbeat | B，辅以try身份快照、coalesce和预算。第三方案writer采集更简单但会阻塞既有SM/RM/reap，不推荐 |
| 03 Interval | 固定1s：快；噪声/读取量多、部署不可调 | 默认2s，静态1..60s：折中且边界可测；增加7字段中的一个校验 | B；固定5s更省但响应迟，未采用。无remote动态配置 |
| 04 Threshold | 原单阈值开/清：最快、旧测试少变；80附近flapping | activate+clear迟滞：状态少、稳定且边界易测；恢复到79不立刻clear | B；第三方案连续N次/迟滞+N可去尖峰但增加counter/延迟/配置耦合，缺持续窗口产品需求 |
| 05 CPU severity | WARNING only：保留现有行为，不把满负载等同故障；不能仅CPU显示功能不可用 | CPU CRITICAL：饱和明显；本项目无deadline/业务失效证据，易误报ERROR | A；CPU100%不证明关键功能失效，未来经验证功能故障应走独立事实 |
| 06 Memory formula | MemFree：简单；把page cache当不可用，阈值失真 | MemAvailable：贴近可供应用预算、仅两字段；老BSP缺字段时unknown | B；无复杂reclaim fallback，适配证据需板端验证 |
| 07 Process identity | PID/status.generation：现成；stop/failure变token且同child尚在，PID可能复用 | PID/launched_generation/starttime、批量前后try验证：复用实例契约；非原子且依赖独占reap | B；不传pidfd ownership，不新增lifecycle owner，O(N) |
| 08 Process CPU | 全机归一化0..100：与system量纲近；需aggregate窗口/拓扑对齐 | 单核等价elapsed/HZ：简单可测，top风格；多线程可>100 | B；明确量纲，不硬编码HZ/pagesize。process仅观测，无threshold |
| 09 Config | 每ServiceConfig系统阈值：loader类型不变；多份冲突、owner错误 | RuntimeConfig+global MonitoringConfig+旧wrapper/native重载：全局唯一、一次加载；接口与模式迁移有成本 | B；保留旧接口、JSON整数parser，不引框架 |
| 10 Event | 每metric新type：名字明确；重复已有severity/active/source功能 | 复用resource_warning：已有aggregator替换/clear/evaluate；type名称需解释可含critical | B；最低兼容/资源/测试成本，不增枚举或总线 |
| 11 Recovery | resource ERROR反推任意restart：表面自动；无责任service，可加剧压力与storm | 只设备health，具名故障仍RM决策：owner明确；压力不能自动消除 | B；P4 boundary硬约束，未来资源恢复另ADR |
| 12 Failure | 一次采集失败ERROR或clear：简单；availability与pressure混淆或误清已确认critical | validity/age/error计数、invalid不推进policy：事实诚实；可保守保持旧latch | B；无monitor-health状态机，日志节流/只读stale |
| 13 Persistence | DB/历史趋势：诊断丰富；依赖/IO/维护成本大 | current+previous memory-only：满足delta、开销小；无趋势 | B，后续独立需求再审 |

Temperature/Disk/IPC的A/B、优缺点见[SCOPE_AND_IPC.md](SCOPE_AND_IPC.md)。三者均延期：缺BSP sensor契约、指定mount需求及真实IPC客户端；typed seam和UDS留下扩展边界，不创建空接口。展示价值不能替代实际需求或RK3588运行证据。

阈值默认80/75、80/75/95/90沿用已有activate，新增5点迟滞；这是候选默认，不声称已针对RK3588负载调优。稳定性来自明确语义与测试，不能从文档推断实际功耗/实时SLA。
