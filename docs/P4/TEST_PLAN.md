# Phase4 Testing Matrix / Acceptance

## 测试分层与状态报告
Unit：组合fake ProcessSupervisor、explicit fake writer clock、SM/RM/aggregator；测试不是照实现重复字符串，而是断言可观察launch次数/先后、预算、event/result数、健康快照、oldPID保持和generation拒绝。
Integration：Linux host上真实fork/exec/signals/reap、Runtime writer、Unix socket；沿用re-exec self child与ready pipe/gates避免固定sleep猜测。deadline可由可注入writer clock在unit精确推进，生产默认Clock::now；真实集成使用有界等待、单独长关机回归。
Static：结构/注册/协议边界/无双owner检查；不能代替编译或运行。ARM64编译不等于目标运行。

每次报告分Implemented / Statically Verified / Runtime Verified / Not Verified，附commit、环境、命令、exit status、失败日志和未执行原因。当前包全部测试均为计划；当前Runtime Verified为空，不写PASS。历史P3静态PASS仅属于当次历史检查。

## Required matrix
U=假时钟单元，I=Linux真实集成，S=结构审查。T4必须把下列核心12项做成真实assertions，不能仅保留test函数名称。
| ID | 场景 / 输入 | 必须观察的结果 | 层 / owner |
| --- | --- | --- | --- |
| P4-01 | HIGH service crash→restart正常 | failure→START一次→replacement→SUCCESS一次；ERROR→RECOVERING→RUNNING；正确request/launch gen；old child已reap | U+I，T1/T2/T3/T4 |
| P4-02 | initial launch失败+五次retry launch失败 | delays2/4/8/16/32边界，6次launch总数，count5；FAILED/RECOVERY_FAILED一次；无第7次/第6retry | U+I，T2/T4 |
| P4-03 | heartbeat timeout，old child忽略SIGTERM | 首accepted timeout创建一次任务；grace前无KILL、到期KILL，reap前无replacement；成功清heartbeat latch | U+I，T3/T4 |
| P4-04 | Failure A→Recovery A→Failure B→late Success A | 无论B是同episode新attempt故障还是独立r2，A不可清B；old/future/missing token拒绝；snapshot/notification保持 | U+I队列gate，T1/T3/T4 |
| P4-05 | deadline前−1ms、等于deadline、晚于deadline | <可成功；>=TIMEOUT且一次RECOVERY_FAILED；due相同时间零launch；最后PID不可伪清，无永久RECOVERING | U+I，T2/T4 |
| P4-06 | duplicate failure（相同generation/type） | 一episode、一reservation、一START，不延长due/deadline | U+I队列，T1/T4 |
| P4-07 | duplicate SUCCESS/FAILED/TIMEOUT | 只一terminal、一相应健康事件；重复不改snapshot或通知 | U+I队列，T1/T4 |
| P4-08 | A依赖B、diamond / duplicate edge | stop逆拓扑各节点一次；B只恢复自己；A保留STOPPED；显式START(A)正拓扑；无循环storm/unrelated影响 | U+I，T3/T4 |
| P4-09 | BACKOFF/EXECUTING/manual期间shutdown | CANCELLED，已接受shutdown后零launch；最后设备快照保留；合法grace+reap后PID−1，否则显式失败 | U+I，T3/T4 |
| P4-10 | HIGH automatic recovery最终失败 | 正确一次terminal来自真实执行链，OFFLINE不可自行退出 | U+I，T3/T4 |
| P4-11 | optional普通失败、optional heartbeat失败 | 普通terminal WARNING；heartbeat terminal OFFLINE，区别不可抹掉 | U+I，T3/T4 |
| P4-12 | 一个SUCCESS仍有另一FAILED/critical resource | 保持WARNING/ERROR，或其它critical active为RECOVERING；无中间RUNNING通知；source资源只由自身clear | U+I，T3/T4 |

## Additional mandatory matrix
| ID | 边界 | 验收 |
| --- | --- | --- |
| P4-13 | never / on-failure / always × clean/abnormal/unknown/start/timeout/explicit STOP | RM唯一policy读/决策；never首次无terminal，clean仅alwaysauto；explicitSTOP永不auto |
| P4-14 | 五次均成功但之后再次crash | count仍5，enabled新episode零attempt RETRY_EXHAUSTED一次，无START/reset |
| P4-15 | reserve后cancel / manual START/STOP/restart | 预算不退回不reset；manual不占auto，disconnect不cancel；duplicate manual coalesce |
| P4-16 | manual与auto重叠 / prerequisite STOP | single入口，supersede/cancel明确；无IPC poll第二次START；STOP后late success无作用 |
| P4-17 | manual launch失败 | manual一次FAILED内部；enabled新auto episode，never只首次fault；无重复RECOVERY_FAILED |
| P4-18 | exec成功新gen与fault不同 | SERVICE_STARTED仅candidate；通过bridge的SUCCESS才清fault；不可query新token给old结果 |
| P4-19 | service重启、PID数值复用、late exit/health_missed | instance generation验证；PID单独相等不足；old facts不失败replacement |
| P4-19b | fault/explicit STOP推进lifecycle token但oldPID尚在 | 保存的launched_generation不变，合法old child cleanup exit可以reap；不得误拒绝并留PID；replacement token不同 |
| P4-20 | deadline期间blocked dependency/unreaped PID | 不再次reserve、不early launch；TIMEOUT终结；cleanup仍保留PID/grace |
| P4-21 | synchronous launch耗尽剩余time / observer throw | cap startup；返回后timeout优先；signal/stop后不接受成功；异常cleanup恢复signals，不吞错 |
| P4-22 | 多服务due相同 / queue负载 | 每turn<=1 recovery launch；due+拓扑tie稳定；tick不只在health_check；timeout可推进时必终结 |
| P4-23 | invalid/new recovery_timeout、derived defaults、overflow | single/root services均解析；1/86400合法，0/86401/float/string/null非法；174/36089默认；programmatic一致 |
| P4-24 | shutdown_timeout2/6混合、six-second regression | max outstanding deadline；六秒后force/reap才返回；无WILL_FAIL/DISABLED |
| P4-25 | IPC type6/STOP与全部旧命令 | header/type/payload/error/pairing unchanged；ACK非success；SERVICE_STOP保留；query不推进RM |
| P4-26 | GET_HEALTH/subscription/slow client | grace UNKNOWN、nonempty allhealthy才能HEALTHY；原overflow/reconnect/weak sink不退化 |
| P4-27 | nonexistent/self/cycle graph | worker启动/任何lifecycle前拒绝，无side effect |
| P4-28 | generation接近uint64最大、同/倒序timestamp | overflow显式错误，不wrap；timestamp不参与freshness |
| P4-29 | timeout finalization callback | 新FAILED不创建auto loop；Result绑定final captured gen；cleanup STOPPED不解terminal |
| P4-30 | OFFLINE同时另一service pending | 其它auto取消，无继续auto launch；manual/START不解OFFLINE，不reset预算 |
| P4-31 | fake executor invariant failure | FAILED(EXECUTION_FAILURE)一次，无fake SUCCESS；正常launch失败仍重试而非直接terminal |
| P4-32 | 未匹配post(RuntimeEvent/RecoveryResult) / device-wide bypass | recovery gate拒绝，不允许source字符串或futuretoken绕过；资源fact合法入口保留 |

P4-04/05/06/07真实队列场景可用可控生产者/observer gate；不能要求真实内核产生可重复乱序，用Runtime入口及captured token验证链。P4-19用fake backend强制PID复用，比希望Linux恰好复用可靠；真实后端验证独占reap与pidfd安全。P4-22压力记录最大通知延迟不定义未经测量的硬实时SLA。

## Existing regression migration
runtime_core、recovery_dependency和phase2_integration目前直接实例SM并假设自主auto重启。需将这些fixture改为SM+RM组合，并保留delay/count/reap/顺序断言；“SM本身无auto”另加负向测试。event_aggregation手工recovery注入更新context，不删除stale/future/missing或partial recovery断言。
device_state_manager的63无target/441 target矩阵保留，24合法triples不改。保持8个旧executable与9项CTest（可新增），独立phase2_runtime_shutdown_regression必须实际运行。
现有phase2/phase3 static scripts有对旧adapter/recovery_exhausted的结构断言；更新对应ownership预期，不删整段校验绕过失败。phase4_static_check同时确保legacy delay/restart_at/IPC poll/Runtime service_recoveries_生产分支消失。字符串不存在不是完整single-owner证明，要人工审查所有policy read与launch callsites。

## Build and verification commands（后续Linux执行；本次未执行）
```text
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 -B tests/phase3_validation_static_check.py
python3 -B tests/phase4_static_check.py
git diff --check
```
根CMake要求>=3.16，C++17及Threads，无额外依赖。CTest进程测试不能随意并行：部分Runtime测试修改process-global signal handlers，保持默认串行；每test使用唯一tmp/socket路径。必要repeated race执行限定新增关键用例，记录次数/失败，不泛化无限重复。
ARM64：使用部署已有toolchain file另建build-arm64；编译与目标执行分别报告。没有仓库提供的toolchain路径，不能虚构。RK3588部署实测可随后执行，不阻塞设计冻结；Phase4完成验收至少要Linux host构建和关键集成真实通过。

## 20项完成验收与证据映射
1. RM职责明确且有实现→ADR01、模块审查。
2. Single policy owner→全部policy read/launch callsites人工审查+P4-13。
3. 无两个独立retry loop→legacy移除检查+P4-16/22。
4. Failure形成Request→P4-01/03/13。
5. Execution形成Result→P4-01/02/31。
6. Result关联具体generation→P4-04/18。
7. 旧result不清新fault→P4-04/19/32。
8. exhaustion产生terminal→P4-02/14。
9. RECOVERY_FAILED来自真实链→P4-02/10/11。
10. 无逻辑永久RECOVERING→P4-05/20/21；内核阻塞限制如实记录。
11. SUCCESS重算全部health→P4-12。
12. dependency边界明确且落实→P4-08/27。
13. 无无限auto retry→P4-02/14/29。
14. 无明显storm→P4-06/08/22。
15. P1–P3公开接口最小兼容→旧回归+CHANGE_LIST逐项。
16. Unix socket架构保持→P4-25/26+frame unchanged。
17. 无业务实现→scope静态+人工review。
18. 无Phase5采集→Monitor diff仅instance token。
19. 关键恢复路径测试覆盖→P4-01..12实际运行记录。
20. 文档/任务/最终代码一致→T4收敛报告，无假PASS。

只有设计包完整不等于Phase4 Implemented/Runtime Verified。T4必须给出逐项证据和remaining failures，不能用“代码存在”替代运行验收。
