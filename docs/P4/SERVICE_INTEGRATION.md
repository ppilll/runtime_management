# Service Lifecycle / Dependency / Shutdown Integration

## 最小迁移清单
从SM移到RM：maximum_restarts、restart_delay、restart_at、retry reservation decision、enabled policy判定、automatic replacement scheduling、terminal exhaustion判定。从Runtime移到RM：service_recoveries_及依据RUNNING/FAILED推导结果的逻辑。从IPC移到RM：pending_restarts/cancel_restarts及STOPPED轮询发START。

SM仍保留registry/mutex/query/public startService/stopService/restartService、startup_order、dependency closure、PID/heartbeat/start_time、generation、termination_deadline、ProcessSupervisor调用与post-lock callback。restartService仍为请求routing facade，连接到同一个内部manual command；不能保留另一套stop/start实现。

## 拟增内部primitive（语义契约，无C++定义）
| 操作 | 输入 / 检查 | 输出 / 状态边 |
| --- | --- | --- |
| beginRecovery | expected fault/current gen + captured episode context + RM已reserve计数 | FAILED→RECOVERING；cleanup与dependency stop已由failure路径执行；无delay计算 |
| launchRecoveryAttempt | expected_current_gen、episode、剩余startup预算；oldPID<=0、所有prerequisite RUNNING、RM许可 | RECOVERING→STARTING→RUNNING或FAILED；captured launch gen与PID/失败原因回调 |
| finishRecoveryFailure | expected_current_gen、episode、terminal_reason | RECOVERING→FAILED必要新增边；RUNNING candidate若timeout需按明确fault cleanup进入FAILED；不自动创建另一episode |
| projectRestartCount | RM单调reservation total | 更新SM.status.restart_count；SM不计算策略 |
| cancelRecovery / stopService | expected context或显式stop | 清执行上下文，正常STOPPING/STOPPED；generation变化使旧结果失效 |

调用均writer-local，不能从RM worker/IPC线程直接调用SM。执行admission gate拒绝old context及active期间普通START bypass。生命周期状态回调保留锁外，尾部增加structured failure cause和optional operation context。

terminal finalization的FAILED事实标明“已有结果的执行终结”，供聚合保留故障，但不能被当成新自动request导致无限loop。重复finish不产生新generation/event。TIMEOUT针对一个已exec成功但尚未确认的candidate时先做finalize/cleanup再发terminal，不发布成功。

finalize若发新FAILED generation，使用该捕获值作为Result.latest_fault_generation/RuntimeEvent.generation；先撤销执行权限但保留关联直到finalization回调完成，随后commit结果并关闭active。未reap子进程launched_generation独立保留原launch token供exit/Monitor验证，不能把fault/stop generation误当旧进程身份。sealed canonical result在关闭active后仍可投递一次，外部迟到结果无此权限。

## Process exit分类：策略只能由RM读
当前SM.process_exit用restart_policy区别clean+always。迁移后由Runtime调用RM的纯判断classifyExit，给现有Event尾部附ExitDisposition（NORMAL_STOPPED或RECOVERABLE_FAILURE）再交SM执行状态转换。RM读取静态定义；SM只应用明确disposition，仍自己验证PID/instance/state/raw wait status。
显式STOP/已failed/recovering cleanup exit无需政策分类，直接reap清PID/完成stop。unknown status=-1视异常。standalone SM无coordinator时保留生命周期默认：clean→STOPPED，abnormal→FAILED，无自动恢复；需要automatic的既有测试fixture显式组合RM，不能另做compatibility retry owner。

SM.fail保留事实、monitor unwatch、stop_dependents、oldPID终止deadline；删除schedule/attempt/exhausted逻辑。clean+always由RM分类为recoverable failure以保留已有FAILED→RECOVERING轨迹。never首次fault仍FAILED，不终结为OFFLINE。

## Dependency Recovery冻结边界
静态graph已支持forward/duplicate edges、missing/self/cycle验证、拓扑tie按name；不新增DependencyManager。A依赖B：
1. B failure：SM逆拓扑停止所有transitive dependent（含A），取消它们active recovery，old child异步STOPPING直到reap。
2. B只恢复自己；其prerequisite必须RUNNING。blocked时等待absolute deadline，不新增attempt。
3. B成功后A仍STOPPED，不自动revive。显式START(A)沿既有prerequisite closure按拓扑启动；不能绕过任何prerequisite active recovery。
4. 不相关service不动；diamond每节点最多一次stop请求；clean prerequisite exit同样stop closure。
5. dependent被停止而无自身fault，不创建新的auto episode；否则会出现循环restart。
6. 同时多个真实fault：各service有独立预算/episode。closure停止取消affected active；不合并group episode。每turn最多一个launch、固定拓扑tie避免storm。
7. dependency停止的HIGH dependent仍是critical unavailable，B optional恢复时设备可ERROR；不因B SUCCESS强制RUNNING。这是P2保留行为，运维需要显式启动A。

L1只service/process recovery。L2必要能力就是现有停止闭包与显式恢复顺序；不自动restart group、不动态发现、不跨进程业务协调。Runtime restart/reboot只作为未来其他owner动作的边界描述，不提供可调用默认实现。

## Shutdown Reliability Decision
S07已按最大outstanding service graceful deadline+5s等待，全部PID清除才正常返回；超时报错，异常路径保留原错误传播。无需重复V04修复，保留S17六秒regression及多期限unit。

进入shutdown的writer入口第一步：标记shutting_down → RM.cancelAll(SHUTDOWN) → 撤销manual/automatic launch权限 → SM.stop_all → 按现有期限tick/force/reap → teardown。normal shutdown冻结最后设备快照，不发RECOVERY_FAILED/OFFLINE；CANCELLED仅内部记录/日志，不经健康dispatcher。不能在service_state_changed先抑制全部回调后再尝试给RM清理关联，RM取消必须更早。

queue中shutdown命令仅在被writer消费时生效；消费之前的FIFO工作可能已完成，不能声称入队瞬间生效。SIGTERM/INT标志在每次launch前检查，进入shutdown入口。同步start中到达signal/STOP不能打断所有内核操作；返回后checkpoint取消并cleanup，不接受late success。保证stop已被writer接受后不会启动任何replacement。

不可reap PID保留，不以析构强杀代替正常shutdown成功。不得禁用独立shutdown test、加WILL_FAIL、放宽其六秒/PID/assertion。
