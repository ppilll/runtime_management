# Resource Event / Device State / Recovery Boundary

## 既有Event模型已经足够

RuntimeEventType不增加；ResourceSeverity不增加normal枚举。正常用active=false。
source是稳定resource identity：cpu_monitor、memory_monitor。它不是service_name；generation/recovery_context留空。进程测量留在typed snapshot，不发具名资源事件（当前aggregator拒绝带service_name的resource_warning）。

路径：Monitor→private ResourceSink queue adapter→Event(runtime_event)→Runtime writer→dispatcher→ServiceAggregation.handle→evaluate/reconcile→DSM表校验与提交→既有DEVICE_STATE_CHANGED通知。external模式沿用report→同policy→queue；直接post用于已有受信fact ingress。

没有CPU_WARNING/MEMORY_WARNING/CPU_CLEAR/MEMORY_CLEAR新type，不新增总线。FIFO按入队顺序，不按timestamp重排。只变更facts减少global queue流量；不宣称全局队列容量问题已解决。

## 设备状态

| 资源事实 | 聚合语义 |
| --- | --- |
| 无latch | 仅按其它服务/恢复事实evaluate |
| warning | 健康服务就绪时WARNING |
| critical | ERROR，服务正在恢复也不能盖住critical资源 |
| severity downgrade | 覆盖同source，再全量evaluate |
| clear | 擦该source，再全量evaluate；不能直接set RUNNING |

现有OFFLINE为terminal latch，优先级高于resource critical。服务critical/recovering/warning/readiness各按既有evaluate；所以resource clear的最终结果可能是OFFLINE、ERROR、RECOVERING、WARNING、READY或RUNNING。不能只写“clear后恢复RUNNING”。

聚合示例：healthy running + CPU80→WARNING；memory95→ERROR；memory90→WARNING；memory75但CPU仍warning→WARNING；CPU75而control仍失败→ERROR/原recovery状态；所有故障clear且无running服务→READY。P4 recovery_success不能清resource latch。

不修改DSM状态枚举、原表、24allowed/417rejected目标声明矩阵，不在采集层产生DeviceStateEvent.health_target。

## Recovery最重要的边界

资源事实不形成RecoveryRequest，不调用RecoveryManager.submit/restartService，DSM没有“ERROR就restart”的回调。Runtime处理resource_warning的分支只走dispatcher/聚合；P4 apply_change已验证具名SM故障形成恢复请求的路径保持。

不实施memory critical→restart vision_service，也不实施CPU warning→restart control_service。系统压力缺少唯一责任service；这样的动作可能加剧fork/memory压力和restart storm。

Collector也不产生service_failed/heartbeat_timeout。PID消失或Z只能观察，原Supervisor/SM验证才可发生命周期失败；其后是否恢复仍由RM判定。

若未来需要资源驱动恢复，须另ADR明确目标服务、故障归属证据、policy owner=RM、admission/预算/代次/取消与storm控制；不能在本次“接线”中加入。
