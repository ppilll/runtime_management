# Runtime Event Integration

## A/B 选择

A：MCU_LINK_UP/DOWN/RESTARTED专用枚举，容易理解但漏UNKNOWN/DEGRADED与identity，硬编码多。
B：一个最小 component_health_changed + typed ComponentHealthFact；本期component=mcu_link，通过静态registry授权。推荐B；不是任意telemetry总线，无metric/rule/plugin注册。
C：复用service heartbeat错误，因为有线断开且Linux进程健康的组合，停心跳会触发P4 heartbeat故障和进程恢复。
也不伪resource_warning：这是组件可用性，CPU/Memory resource source/latch不可混用。

## 内部接线

建议 ComponentHealthStatus / ComponentHealthFact / ComponentPolicyEntry 模型（名字允许等价调整，语义不可变）。
RuntimeEvent尾部optional component_health，不改现有aggregate字段顺序；新增一个RuntimeEventType。
另有typed ComponentIngress envelope（bind/report/disconnect/expiry是admission操作，不是四个新RuntimeEventType），携server捕获identity、connection token、received_at、request_id、reply弱引用。Runtime拥有窄private adapter，IPC通过构造时注入sink使用；任意public post(RuntimeEvent)、post(Event)包装不得伪造component trusted事实。测试通过受限fake transport/peer seam驱动同一admission，而不是公开bool trusted。

IPC thread不决策DeviceState。writer步骤：处理原lifecycle/reap；重验证当前running PID/launched_generation/session/runtime_epoch；应用bind/report/disconnect；检查lease；changed事实→dispatcher→aggregation→DSM；完成异步reply。与原FIFO的必要相对次序用envelope入队/wake和writer检查点固定，不用producer时间重排。STOP或故障已在writer提交时，随后report必拒绝；report在STOP前合法应用则可有停止前设备变化，shutdown gate后禁止。

## 单一 registry / 去重

component key→producer service→ServiceHealthPolicy 在config加载后冻结；本期允许最多一个mcu_link、一个producer，不允许任意service创造key。writer保存已绑定session、最新report_seq/digest、committed current、received_at/lease、本launch是否曾HEALTHY与adverse latch。
同seq同payload仅重复ACK，不再renew lease/发event；同seq不同payload拒绝；低seq拒绝。revision不变的高seq允许stable续租；revision回退拒绝，同revision与status不同仅允许明确的pending_issue→current投递规则见IPC，避免任意改写。纯续租不dispatcher重复publish。
service lifecycle从RUNNING离开或launch变化立即invalidate session/当前可信观测，重聚合。新launch不继承旧HEALTHY/boot/revision/adverse；Runtime reboot不持久化这些字段。

## 有限输入和公平性

每producer最多一个bound session、一个inflight report；client先issue ACK后latest。Runtime私有component操作队列上限32，bind/report各连接最多一个pending；配置本期只有一个component。满时返回1003，不ACK成功；producer保留issue以稍后重试。一个coalesced wake投既有queue，writer每turn有限drain并检查lease；不能把每1s续租全部塞进无界global queue。
disconnect用session的atomic revoked/tombstone存于有限session record，即使操作队列满也立即阻止writer接纳旧report；writer每turn看到revoked生成UNKNOWN。reply队列同样有限且weak lifetime，原IPC每client输出上限保持。
epoll health新命令每轮最多16帧，health payload<=4KiB、pending数上限、不可持续读恶意流到无界input；限流只在新命令/受管producer入口，不借此宣称旧control客户端全局DoS已解决。
本期不保证无限全局EventQueue负载下硬实时；旧source背压债务保留，健康gate必须保证有限内存/不会丢失故障摘要。

## lease

固定report interval1000ms、lease3500ms，writer steady_clock now>=last accepted received_at+lease则observation UNKNOWN/report_expired。disconnect已确认立即UNKNOWN/observation_unavailable。新report可恢复观测，只有fresh HEALTHY才能清availability/adverse；若前已UNHEALTHY不能以unknown伪clear。
lease只监督Linux service→Runtime报告链路，不读取UART byte/last_rx_age；不触发RecoveryRequest。query派生age但不推进expiry或state。长writer阻塞下期限在下个可推进checkpoint执行，不承诺硬实时。

重连必须保留本 launch 的 owner revision/kind/已应用 issue 确认下界；derived UNKNOWN 不抹掉这些信息。issue→current 同 revision 的窄例外、ACK 丢失去重与新故障抢先规则统一见 IPC_CONTRACT 补充冻结规则。
