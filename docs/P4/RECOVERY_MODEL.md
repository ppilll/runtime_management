# Recovery State / Request / Result Model

## 三种状态各自目的
ServiceState：进程生命周期，只有SM可写；DeviceState：多服务/资源整体可用性，只有DSM可写；RecoveryState：一个协调任务当前能否执行，只有RM可写。service RUNNING可发生于exec后且无首心跳，不意味着设备或业务完全健康。

## FailureFact（内部类型设计，非C++源码）
新增结构化 failure_type 作为生命周期回调尾部可选元数据：PROCESS_CRASH、CLEAN_EXIT_RESTART、HEARTBEAT_TIMEOUT、STARTUP_FAILURE、STARTUP_TIMEOUT。reason保留exec/chdir等细节，raw_exit_status可选。SM验证PID/state/deadline后生成，不能从Device ERROR反推。
必须包含service_name、发生时捕获的lifecycle_generation、producer_time、reason、type。recovery操作引起的生命周期回调还带不可伪造为“当前操作”的captured operation binding（见GENERATION）。

无效service/nonzero token/source/reason、重复同generation/type事实拒绝或coalesce，不计attempt。RESOURCE_WARNING本身不是service failure，不自动创建恢复。未来Monitor service故障同样必须经过当前实例验证。

## RecoveryRequest
| 字段 | 必需 | 含义 |
| --- | --- | --- |
| service_name | 是 | 静态已注册服务 |
| origin | 是 | AUTOMATIC_FAILURE 或 MANUAL_RESTART |
| failure_type / reason | 自动是；manual用MANUAL_REQUEST reason | 决策不解析reason字符串 |
| service_generation | 是 | 入场时捕获的服务generation；自动为fault generation |
| producer_time | 是 | steady metadata，FIFO不排序 |
| recovery_generation | admission时分配 | per-service单调非零episode编号 |
| accepted_at / deadline | admission时生成 | 使用writer当前时间，不使用可能旧producer_time |

(service_name,recovery_generation)就是request ID，不增加第三个request_id。自动请求的initial_fault_generation保留不变，expected_generation/latest_fault_generation可以由受绑定生命周期事实更新；两者不混用。

submit返回ADMITTED、COALESCED、SUPPRESSED_POLICY、STALE、REJECTED_SHUTDOWN等admission disposition。never首次自动故障为SUPPRESSED_POLICY，无RECOVERY_START/FAILED；它不是一次失败的恢复任务。已经耗尽预算的enabled新故障仍admit一个零attempt的terminal episode，产生一次FAILED(RETRY_EXHAUSTED)，无RECOVERY_START，因为没有执行开始。

## 最小active状态
没有active record就是IDLE。每service最多一个active：
- BACKOFF：已reserve自动attempt、等待due/旧PID回收/依赖RUNNING，绝对deadline运行中。
- EXECUTING：正在执行一个stop/reap/start primitive；同步start返回时立即判定或回BACKOFF。manual从EXECUTING开始等待stop/reap，无backoff。
终结SUCCEEDED/FAILED不增加长期state；保留last_result后移除active。每服务仅保留active+last_result+累计预算+generation计数，不保存无界history。

| 输入/条件 | 当前 | 行为 |
| --- | --- | --- |
| 合法enabled自动故障 | IDLE | admit；reserve下一attempt；BACKOFF；一次RECOVERY_START |
| 重复同fault | active | coalesce，不改due/deadline/预算 |
| due到且PID不存在、prerequisite可用 | BACKOFF | EXECUTING，绑定launch |
| launch失败，仍有预算且未到期 | EXECUTING | 更新捕获fault token，reserve下一attempt，BACKOFF；不重置deadline |
| launch成功，关联和deadline均有效 | EXECUTING | SUCCESS，移除active，健康重聚合 |
| exhausted / timeout | active或零预算admit | FAILED或TIMEOUT一次，移除active、禁止launch |
| explicit STOP / parent dependency STOP / shutdown | active | CANCELLED一次；不产生RECOVERY_FAILED |
| 新独立故障或manual supersede | active | CANCELLED(SUPERSEDED)旧任务，按新输入admit；旧success拒绝 |

同一attempt产生的已绑定失败可继续原episode，但latest generation更新使前一个执行结果失效。来自新启动/新独立操作的故障是supersede，不能合并。不能仅按service_name判断。

## RecoveryResult
必需：service_name、recovery_generation、origin、initial_fault_generation、latest_fault_generation、execution_generation（无launch时可空）、outcome、terminal_reason、attempts_reserved_total、completed_at。SUCCESS还必须有对应captured launched_PID；PID仅辅助，generation才是身份。

| outcome | Phase4必要性 / 判定 | 健康事件 |
| --- | --- | --- |
| SUCCESS | 对应exec成功；返回时未到deadline，SM仍同execution gen/RUNNING/PID，active未取消 | 自动episode RECOVERY_SUCCESS |
| FAILED | RETRY_EXHAUSTED，或不可继续的executor invariant failure | 自动episode RECOVERY_FAILED |
| TIMEOUT | now>=absolute deadline，不再启动；不是简单一个attempt失败 | 自动episode RECOVERY_FAILED，reason=RECOVERY_TIMEOUT |
| CANCELLED | STOP、shutdown、dependency停、supersede | 仅内部结果/日志；无需新event |

manual事务也输出上述结果供内部诊断，但不在健康通道宣称自动recovery trio；它的stop/start/failure仍走现有健康生命周期链。manual launch产生新failure时：先终结manual FAILED，再由该failure创建独立自动episode（enabled且预算允许），never则只保留首次failure。取消manual也不算自动terminal。

## 唯一结果生产与幂等
仅RM的finish路径写Result；先标记terminal并撤销所有due/operation binding，再发布规范化事件。重复finish/result无动作。Runtime不能再从SM.recovery_exhausted或service_recoveries_推导第二份结果。last_result辅助拒绝重复；active generation严格单调已足够拒绝更早结果。异常若超出领域结果（observer throw/queue failure），沿用Runtime清理并传播，禁止吞错冒充SUCCESS。

“标记terminal”先撤销执行资格；如需SM finalization，关联上下文保留到取得捕获finalization token后再commit/关闭。canonical输出用sealed receipt投递一次，不再次依赖已经删除的active；任何外部result仍过active gate。精确顺序见RECOVERY_GENERATION。
