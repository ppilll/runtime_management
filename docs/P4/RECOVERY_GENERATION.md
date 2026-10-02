# Recovery Generation and Stale Result Protection

## 三种标识与唯一发放者
1. lifecycle generation：SM已有uint64。STARTING、新FAILED、explicit STOP增加；RUNNING保留launch token；STOPPING→STOPPED同stop token。P4不改变这套发放语义。
2. recovery_generation：RM per-service单调非零，admitted episode增加；成功/取消后不归零。request ID=(service, recovery_generation)。
3. execution_generation：SM开始本次replacement时发放的新lifecycle generation；RM通过该操作的捕获回调绑定。initial_fault_generation不是它。

不增加独立PID generation，不用timestamp判断正确性。PID可复用，所以process_exited/health_missed内部envelope增加捕获instance_generation：reap在owner映射时连同generation投递；Monitor watch保存当时instance generation并回送，SM同时验证。heartbeat仍由IPC receipt time和当前注册snapshot解析，保留既有协议；未认证服务socket的身份限制见RISK，不顺带新增session认证。

instance_generation复用该受管子进程在STARTING时的launch generation值，不另设计数器；SM必须将它保存为ServiceStatus尾部launched_generation（或等价私有字段并由捕获回调提供）。FAILED/STOPPING推进lifecycle generation时，这个仍未reap子进程的launched_generation不变。Monitor.watch及reap映射捕获launched_generation，SM验证它而不是直接拿最新status.generation比较，否则合法failure cleanup的exit会被误拒绝。PID清除后清launched_generation；新launch取得新值。RecoveryResult的execution_generation也是该值；成功另外要求当前lifecycle generation未被新failure/stop推进。

## fault→launch桥：不能直接沿用P3结果token
例如fault=10，RM episode=1，replacement STARTING=11。P3 aggregator要求recovery_success.generation等于当前health fault；直接回送10会被新的service_started(11)覆盖，直接查询11给旧A补标签又会允许迟到结果。两个做法都禁止。

冻结方案：
- RM在beginRecovery指令中携带episode+expected_current_generation。SM在实际变迁时将该captured context放进ServiceStateChange，不在完成时查询补标签。
- Runtime writer保存/使用RM的active binding校验每个回调；RM确认launch token11属于episode1，并记录expected_generation=11。
- 聚合在RECOVERY_START记录episode与故障；recovery相关SERVICE_STARTED附相同context，记录running candidate及新observed_generation=11，但保留unavailable/recovering标记，不能先清故障。
- 仅校验过的SUCCESS以generation=11且episode1、initial_fault10、execution11进入聚合；清除该服务故障后recalculate。
- 期间若合法attempt failure=12，聚合更新observed generation和latched fault，RM可保留episode1且转下一attempt；旧execution11结果不能被接受。重试BACKOFF仍属active episode，聚合保持其recovering标记。
- 新独立failure B或explicit start/stop使旧binding撤销；新failure事实清原recovering，新admission绑定episode2。旧episode1结果全部拒绝。

RuntimeEvent尾部增加可选RecoveryContext（episode、initial_fault_generation、execution_generation、origin）；保留generation字段为该事件所表示的当前生命周期事实。ServiceAggregation保存active recovery context、observed generation和running candidate，不自行查SM补状态。没有context的旧具名recovery事实不再可绕过Runtime的result gate，旧standalone测试相应按明确context准备。

terminal finalization若推进FAILED generation，RM先撤销launch/due权限，调用SM取得该操作的捕获finalization generation，更新latest_fault_generation，再commit Result并关闭active；不得用调用前token发terminal。该回调标明finalization，不做新request。规范化输出在关闭active前已完成全部校验，关闭后由RM的sealed last_result receipt验证投递；外部post结果仍必须匹配active，不能借用已关闭记录再次发布。整个finish过程在同一writer内无reentrant生命周期修改，不增加长期第三状态。

## 接受结果谓词（全部条件必须满足）
- service存在、metadata有效，RM有active record，result.recovery_generation==active.recovery_generation；
- origin/initial_fault_generation与admission一致；
- execution_generation等于active当前绑定，SM当前generation等于该capt获值；
- SUCCESS要求对应PID匹配、状态RUNNING、launch已成功，完成checkpoint now<deadline，未收到stop/shutdown；
- failure/timeout针对latest expected/fault generation；stop generation不能拿来发布旧terminal；
- aggregator观察到相同context及当前generation；重复terminal不得再通知。
无launch的TIMEOUT/EXHAUSTED可使用当前已捕获latest fault generation，不虚构execution token。

## 典型轨迹
| 步骤 | lifecycle | episode / execution | 决策 |
| --- | --- | --- | --- |
| Failure A | 10 | admit r1 | ERROR、BACKOFF |
| replacement A launch | 11 | r1/e11 | candidate；不能先清故障 |
| Failure B（A的绑定attempt失败） | 12 | r1/e11失效 | health故障保留；下一attempt |
| late Success A | 11 | r1/e11 | generation不匹配，拒绝 |
| 下一launch | 13 | r1/e13 | 正确关联可SUCCESS |
| independent Failure C | 14 | r2，r1关闭 | late r1一律拒绝 |
| explicit stop | 15 | cancel r2 | no recovery result可复活 |
producer_time相等、倒序都不影响上述判定。同故障重复event不得增episode/attempt。

## 溢出与生命周期
uint64即将溢出时显式报错并进入Runtime错误清理，不能wrap成0或复用旧token。Runtime重建后内存token重新开始，内部结果不能跨Runtime实例投递；跨boot token持久化不是本阶段功能。callback/producer lifetime必须早于RM销毁结束；外部保留回调不得引用已销毁RM。
