# Event Integration / Concurrency

## 最终事件语义与生产者
保留现有8个RuntimeEventType，无RECOVERY_TIMEOUT/RETRY_SCHEDULED扩枚举。
| 事件 | 唯一规范化生产路径 | 语义 |
| --- | --- | --- |
| SERVICE_FAILED | SM接受异常/启动失败→Runtime事实adapter | component fault，不等于terminal |
| HEARTBEAT_TIMEOUT | Monitor health_missed→SM验证→Runtime adapter | 接受的当前实例心跳故障；无额外SERVICE_FAILED双报 |
| SERVICE_STARTED | SM exec成功→Runtime adapter | 生命周期running；recovery context时只是candidate |
| SERVICE_STOPPED | SM STOPPING/STOPPED→adapter | unavailable；同stop generation幂等 |
| RECOVERY_START | RM第一次安排可执行automatic episode→adapter | 任务active且有attempt；backoff也属于恢复 |
| RECOVERY_SUCCESS | RM关联/期限gate成功→adapter | 对应请求已完成；重新聚合 |
| RECOVERY_FAILED | RM唯一terminal finish→adapter | FAILED/TIMEOUT；reason区分exhausted/timeout/execution |
| RESOURCE_WARNING | 既有Monitor或内部producer | 只更新该resource source严重度/清除 |

RECOVERY_START每episode最多一次；每次retry只日志记录attempt/due，不再从SM RECOVERING边生产start。已零预算的新fault只有failure→terminal，不凭空start。内部事件不等同socket EVENT；现有外部仅订阅DEVICE_STATE_CHANGED。

## 正常automatic序列
1. ProcessSupervisor.reap捕获PID+instance generation，SM接受exit并更新fault generation。
2. adapter发SERVICE_FAILED（heartbeat case改HEARTBEAT_TIMEOUT），dispatcher先让聚合记录fault产生ERROR/WARNING。
3. writer-local RM submit做admission/reserve，SM beginRecovery，adapter发带episode的RECOVERY_START；符合整体条件时ERROR→RECOVERING。
4. 到due且oldPID已回收、依赖可用，RM发一个launch primitive。
5. SM STARTING新generation；exec成功RUNNING回调包含captured context。SERVICE_STARTED保留candidate不清fault。
6. RM确认SUCCESS，先关闭active，发带完整binding的RECOVERY_SUCCESS；聚合只清本fault并重算，DSM提交和IPC通知。

failure→recovery_start之间不允许queued其它command插入；causal dispatcher FIFO不递归。下一failure可以把设备从RECOVERING拉回ERROR，但retry仍同episode时经记录active context重新计算，不重复发布start；设备仍有独立critical问题时保持ERROR。

## 并发矩阵
| 竞态 | 冻结规则 |
| --- | --- |
| duplicate failure同generation/type | coalesce、不reserve、不改deadline、不重复start |
| heartbeat timeout与exit同时到 | SM当前状态/instance验证；只有首次fault触发request，cleanup exit仅reap |
| duplicate SUCCESS/FAILED | closed episode拒绝，快照和notification不变 |
| late result A / failure B | episode+execution/current generation gate；见GENERATION，不靠时间 |
| START while BACKOFF | admission gate拒绝bypass；明确START不会reset预算 |
| explicit STOP / PID变化 | stop先撤销binding；new instance generation使旧exit/health/result失效 |
| manual restart/auto重叠 | RM单入口，manual supersede auto；同manual duplicate coalesce |
| prerequisite停止 | reverse closure；取消affected active；不得在恢复后自动revive |
| shutdown期间结果 | cancelAll后任何success不修改健康/启动；正常shutdown快照保留 |
|同checkpoint due+deadline| deadline优先，零新launch |
| process已reap但queued退出未处理 | writer先应用reap事实再timeout决策，不能因为自身排队误判 |
| observer throw | Runtime走现有exception cleanup，保留错误；非silent-success |
| 生产者时间倒序/相同 | queue order定序，仅token判新旧 |

## 内部输入门
新增EventType::restart_request（尾部追加，非IPC type）；可选typed RecoveryResult envelope与instance generation/context追加在现有Event字段后。RuntimeManager::post(RecoveryResult)若保留给内部异步producer，必须经过active binding gate，不直接publish RuntimeEvent::recovery_success。
RuntimeManager::post(RuntimeEvent)的具名recovery trio只接受RM规范化输出路径；外部缺context或不匹配active的结果拒绝。device-wide recovery留P3 standalone能力，但P4 Runtime没有device-wide自动协调器，producer默认不得绕过RM发布device-wide terminal。一般资源facts入口保留。
这属于内部契约收紧：不能单靠source=="recovery_manager"字符串信任。门依据active record+captured context，只有内部owner可产生canonical output；不设计公网安全协议。

## 单写者细节
EventQueue mutex解决投递；RM/dispatcher/aggregation只writer写。RM执行指令不从dispatcher订阅函数同步递归执行SM；先收集本轮accepted fact，再drain-local执行；每步后再排空因果facts。SM callback可以query但不可重入修改。查询不触发retry或聚合更新。回调不throw的既有约束保留。
计时checkpoint每轮都运行，不能只等timer health_check；queue pop_for正常上限200ms不意味着全负载最大200ms。每轮最多一个recovery launch，但不引入全局rate limiter队列或无界history；active数量上限为静态service数。
