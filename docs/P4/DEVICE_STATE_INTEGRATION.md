# Device State / Health / IPC Integration

## 保留P3规则
仍用BOOTING/READY/RUNNING/WARNING/ERROR/RECOVERING/OFFLINE；保留原9条无target和15条aggregate target边（24个triples）。只有DSM提交快照，新增RECOVERING→FAILED是service边，不是device新边。

| 接受的健康情况 | 设备行为 |
| --- | --- |
| HIGH fault | RUNNING/READY→ERROR |
| optional非心跳fault | WARNING |
| 任意等级accepted heartbeat timeout | ERROR，保留P3政策 |
| 全部critical issue都有active recovery且无critical resource | ERROR→RECOVERING |
| 仍有未恢复critical / 停止的HIGH dependent | ERROR，即便某个RM任务active |
| SUCCESS但另一optional FAILED/STOPPED | WARNING |
| SUCCESS但另一critical未恢复或critical resource | ERROR；其它critical均active可RECOVERING |
| SUCCESS、required齐备且至少一个running、无issue | RUNNING |
| fault-free但required未齐或全inactive | READY |
| HIGH/heartbeat terminal FAILED或TIMEOUT | OFFLINE |
| optional普通terminal | WARNING（若另有critical则ERROR） |
| normal shutdown | 保留last snapshot，不虚构OFFLINE |

OFFLINE终止当前Runtime生命周期；后续explicit START/成功不能自动退出OFFLINE。RM在接受OFFLINE后取消其它active任务（内部CANCELLED，非额外FAILED），阻止继续automatic launch；manual/START控制保持既有命令入队与生命周期语义，但不会解锁设备OFFLINE或reset自动预算。依靠重建Runtime进入新的BOOTING，不新增RESET API。

## 聚合最小扩展
Health保持running/failed/stopped/heartbeat_lost/observed generation，增加active recovery context与candidate launch generation。recovery SERVICE_STARTED只记录candidate，不能在RECOVERY_SUCCESS gate前清failed/heartbeat；正确result清本服务，并重新evaluate全部records。
中间绑定attempt failure更新observed token但仍属同active episode；独立新fault取消旧context。timeout/finalize后FAILED用于健康，不再次触发request。cleanup stopped只增加unavailable，不能解除latched terminal或heartbeat。
资源来源锁存不被service success/start/stop清除。critical resource优先ERROR，只有同source测量clear/downgrade可解。
required与criticality不同：保留default control HIGH required，vision MEDIUM optional，OTA LOW optional，其它LOW/autostart；mcu_service在源码没有特别HIGH默认，部署必须显式AggregationOptions override，不能凭业务想象修改。

## IPC Impact Analysis
不增加GET_RECOVERY_STATUS、recover/reset命令或new subscription。保留两个Unix SOCK_STREAM通道、小端10字节header、64KiB JSON、类型1..10/255、error1001/1002/1003及request_id pairing。
RESTART_SERVICE type6仍payload service_name、ACK result OK表示入队接受；断连不取消已接受任务；STOP仍取消目标/依赖affected work，SERVICE_STOP通知保持。
改变只是IpcManager删除pending_restarts/cancel_restarts/poll循环，提交一个internal restart_request；RM执行stop/reap/start。IPC不再决定何时START。

GET_HEALTH/GET_DEVICE_STATE/QUERY_STATUS/GET_SERVICE_LIST继续投影现有状态和restart_count；内部Result通过日志诊断，不加wire字段。DEVICE_STATE_CHANGED只反映真正设备快照变化；optional retry可能设备一直WARNING，没有逐attempt通知，这不是事件丢失。不向control广播内部RECOVERY_*事件。

保留P3的首心跳宽限：生命周期RUNNING≠已确认HEALTHY。GET_HEALTH任意UNHEALTHY row→UNHEALTHY，任意UNKNOWN/空列表→UNKNOWN，非空全HEALTHY才HEALTHY；查询不改device。各模块分别读取不是atomic整包；不新增全局快照事务/revision。
保留subscription ACK先于notification、有界输出/producer队列、断开后重新订阅并query、weak sink析构安全。
