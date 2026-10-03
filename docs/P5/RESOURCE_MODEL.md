# Minimal Resource Snapshot & Interface Contract

名称是后续 C++17 接口的设计名称；本文件不提供 C++ 源码。只定义必需语义。

## SystemResourceSnapshot

| 字段 | 类型/单位/语义 |
| --- | --- |
| sampled_at | steady_clock 时间：采集完成；审计与 age，不作为事件重排依据 |
| cpu | MetricObservation：quality、optional cpu_percent、last_success_at、consecutive_errors |
| memory | MetricObservation：quality、optional total_bytes/available_bytes/used_percent、last_success_at、consecutive_errors |

quality 最小枚举：valid、warming_up、unavailable。stale 不另建状态机：query 以 now-last_success_at > 3×interval 派生 stale 标记。无有效值用 absent，不能用 0 或 NaN 冒充 normal。第一次尚无成功时间为 absent。

失效时最新 observation 为 unavailable，value absent；保留 last_success_at 和错误计数，不把上一次 good 值包装为本次 good。CPU previous counters 是 collector 内部数据，不进 public snapshot。若 GUI/IPC 将来需要 last good value，另审，不增加历史缓存。

## ProcessResourceSnapshot

| 字段 | 语义 |
| --- | --- |
| service_name / pid / instance_generation | SM 捕获 identity；instance_generation = ServiceStatus.launched_generation |
| proc_start_time_ticks | /proc/PID/stat field22；有效观测及 previous 的身份锚点 |
| sampled_at | 每个进程实际读取完成时刻 |
| observation_status | observed、not_present、zombie、identity_changed、unavailable |
| cpu_quality / cpu_percent | optional；单核等价百分比，允许 >100；首次 warming_up |
| rss_bytes | optional；stat field24 pages×系统页大小；仅当前直接子进程 RSS，不含后代 |

not_present 是读取时 ENOENT/ESRCH 的观察，不代表已确认 lifecycle exit。observed 只代表有可读取非 zombie 对应进程；不代表 heartbeat 或业务健康。不为停止且 PID<=0 的服务构造假 process row。STOPPING/FAILED 仍持有效 PID/token 的对象可观察，供 cleanup 诊断。

ResourceSnapshotBundle：一个 system snapshot + 当前 eligible processes vector + collected_at + process_scan_quality（valid/unavailable）。锁忙而无法获得/重验证身份时scan unavailable、process rows为空；不能将空rows理解为所有服务已退出。
集合按 service_name 排序保证报告/测试稳定；不同文件并非原子同一瞬间，不声称整机原子快照。

## 设计接口（输入→输出）

- ResourceCollector.collectSystem(now)：采集 system，独立 cpu/memory validity；保留 CPU previous。
- ResourceCollector.collectProcesses(identities, reader, now)：批量只读，维护每个 identity 的上一 stat/time；已停止或换代立即淘汰旧 previous。
- Monitor.observeResources(system)：仅对 valid metric 推进 policy，产出变更 facts；CPU unavailable 不影响 valid memory。
- 既有 Monitor.report_resources(cpu,memory,at)：两个 double 先整体校验，构成两个 valid observations，再调用同一 policy。保留非法值抛错/不部分发布。
- ConfigManager.load_runtime_file(path)：返回 RuntimeConfig（services + MonitoringConfig）。
- 既有 ConfigManager.load_file(path)：兼容 wrapper，返回 runtime config 的 services；旧 ServiceConfig 布局/字段不变。
- 新 RuntimeManager(RuntimeConfig, aggregation, device_sink, input_mode, optional reader seam) 入口：默认 native；旧 config_path 构造入口保留 external。
- RuntimeManager.queryResourceSnapshot(now)：线程安全副本，派生 age/stale；不读 procfs、不推进采样/阈值/设备状态；native 未采样返回 empty。
- SM trySnapshotProcessIdentities()：try_lock批量复制(name,pid,launched_generation)；eligible PID>0、generation>0；无资源策略。
- SM tryValidateProcessIdentities(captured)：try_lock一次批量重验证，返回仍匹配的身份；可实现为等价的批量 current identity snapshot 比对，不能 N 次 O(N) list 查询。

接口只新增所需模型，不创建大型通用 Metrics 层；接口名可在等价语义内调整，报告变更。process 不新增 thresholds，不产生 service_failed/resource_warning。

now参数表示实际读取完成时刻，可由窄clock callable取得；不能用排队health_check的旧at替代。process CPU记录每row完成时间。
