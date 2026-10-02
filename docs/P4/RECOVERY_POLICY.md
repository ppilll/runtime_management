# Retry / Backoff / Timeout / Configuration

## 策略与计数
RM唯一读restart_policy并选择自动动作。never：首次crash/start/heartbeat故障记录健康但不admit恢复；on-failure：异常/未知退出、启动失败、心跳超时恢复；always：另包含clean spontaneous exit。显式STOP不是clean spontaneous exit，禁止auto retry。

自动预算为每注册service在当前Runtime实例内累计最多5次reservation，包含失败launch和取消的已reserve attempt；原初始启动不计。原restart_count的wire字段继续显示该累计数。SM仅存RM提交的计数投影，无计算策略。
第n次reservation延迟=min(2×2^(n−1),60)s，n=1..5，即2/4/8/16/32。60是公式上限，不意味着第六次retry。
due=reserve_at+delay，用writer时刻；每个attempt失败后下一reserve_at更新；episode.deadline永不延长。due已到但oldPID未reap/依赖未RUNNING继续等待，不重复reserve。显式START不能绕过active BACKOFF。
第5次成功后可以输出SUCCESS，预算仍5；以后第6个可恢复fault立即FAILED(RETRY_EXHAUSTED)。第5次launch失败同episode产生一次FAILED，不再schedule。
没有success/heartbeat/manual START/STOP/reset。重建Runtime/注册表是唯一reset；不新增RESET RPC。

## 成功条件
exec handshake完成，SM进入RUNNING，匹配captured operation及PID/generation，RM尚active且实际now<deadline。不等待首心跳、不实施业务readiness probe，维持P2/P3生命周期就绪与IPC确认健康的区别。
故障后仍有其他service FAILED/STOPPED或critical resource时，SUCCESS仅清本service正确fault，设备可能WARNING/ERROR/READY，绝不强制RUNNING。

## 总期限
automatic episode从writer admission accepted_at开始，包括全部backoff、grace/reap、dependency wait、launch以及回调检查；manual同样从admission开始，stop/reap/start都包含。不用failure producer timestamp减预算。
到now>=deadline时timeout胜过同checkpoint尚未确认的success或due launch。已经在更早checkpoint完成的SUCCESS不撤销。

唯一新增JSON字段：recovery_timeout（整秒，optional，1..86400）。
缺省固定推导：63 + 5×(shutdown_timeout + startup_timeout + 5) + 1 秒。
63为五次可达延迟之和；每次5秒为reap margin；1秒为默认checkpoint margin。这是静态保守默认而非硬实时保证。默认shutdown2/startup15对应174s；合法各3600对应36089s，仍小于86400。明确配置更短合法，可在第一个attempt前TIMEOUT；不强制等所有五次用完。manual复用相同timeout字段，不增加manual timeout。
使用checked arithmetic与steady timepoint范围检查；programmatic ServiceConfig和JSON必须一致验证，不能仅验证parser。

| 字段 | Phase4处理 |
| --- | --- |
| restart_policy | 保留never/on-failure/always及默认never |
| max_restart_attempts | 不新增JSON；冻结常量5 |
| restart_delay | 不新增JSON；冻结常量2s |
| max_restart_delay | 不新增JSON；冻结常量60s |
| recovery_timeout | 新增optional整数秒；默认静态推导 |
| startup/shutdown/heartbeat_timeout | 保留各1..3600s与原默认 |

现有parser未拒绝所有unknown keys；不在P4扩大为strict schema。上述不支持字段若出现在旧配置中不能声称已生效，文档/测试须明确。不支持动态配置更新。

## timeout后的执行/状态
先撤销active launch权限和due，保存TIMEOUT一次。请求SM finishRecoveryFailure：未reap旧PID保留；SERVICE FAILED为最终故障状态，监控unwatch，仍有PID则继续既有SIGTERM→合法shutdown_timeout→SIGKILL/reap cleanup。不为recovery_timeout缩短已承诺的shutdown grace，不把PID=-1当作清理命令。若原状态RECOVERING，增加显式RECOVERING→FAILED执行终结边；不增加状态。
timeout后的cleanup可以晚于结果。SM仍可tick停止升级，绝不能启动replacement。aggregator清recovering，依据criticality/heartbeat latch决定OFFLINE或WARNING。不能因cleanup STOPPED清除terminal fault。
不可回收PID：显式保留诊断，禁止replacement；逻辑恢复已终结。Runtime正常shutdown仍使用最大grace+5s并报无法回收错误。

## 终结细分
| 情形 | admission / 结果 | RECOVERY_FAILED |
| --- | --- | --- |
| 首次fault，有预算 | active→BACKOFF | 不立即发 |
| never自动fault | SUPPRESSED_POLICY | 不发 |
| clean on-failure/never exit | STOPPED lifecycle | 不发 |
| 单次launch失败且可retry | active回BACKOFF | 不发 |
| used=5或最后retry失败 | FAILED(RETRY_EXHAUSTED) | 一次 |
| absolute deadline到期 | TIMEOUT | 一次，reason=RECOVERY_TIMEOUT |
| executor invariant / 不可继续执行错误 | FAILED(EXECUTION_FAILURE) | 一次 |
| explicit stop/shutdown/supersede | CANCELLED | 不发 |

process stop/force_stop失败沿用日志+cleanup，等待deadline而非每次tick发terminal；程序级异常/observer throw由Runtime错误清理，不包装成业务恢复成功。

## 手动restart协调
类型6 ACK仍是入队接受，不是执行成功。single manual transaction：当前automatic active被CANCELLED(SUPERSEDED_MANUAL)，取消其due；SM stop目标及dependents；等待目标PID reap后按P2 prerequisite closure尝试启动目标；不自动revive其dependents。已有同service manual active则coalesce，不随requester disconnect取消。STOP目标或任一prerequisite取消受影响manual/auto active。
手动事务本身只做一次stop/start，不使用/重置自动预算。其新launch失败形成独立failure fact，由RM按restart_policy判断自动episode；manual事务不发第二份自动RECOVERY_FAILED。受dependency active/不可用阻塞时不能强行START，等待manual deadline。manual prerequisite启动仍需通过统一active gate。
