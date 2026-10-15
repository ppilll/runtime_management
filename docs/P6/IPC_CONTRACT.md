# UDS Health Contract v1

## 现状、选择与兼容

现有两个UDS SOCK_STREAM端点：control channel命令/query/subscription；service channel HEARTBEAT和Runtime向service的SERVICE_STOP。
10-byte LE header及64KiB最大payload不变，types1..10/255编号不变。EVENT(5)入站本来拒绝，出站携SERVICE_STOP/DEVICE_STATE_CHANGED。
A扩展入站EVENT：能省type，但改变原方向、混命令和事实，身份/ACK仍须新合同。
B新增HEALTH_REPORT(11)只在service channel：推荐B，bind/report共用一个type，原EVENT语义冻结。不传UART rawSTATUS，不为每state增type。

新producer只有明确health配置启用时需要升级client/server；旧server收到type11报invalid，client不得fallback停心跳或伪资源。未启用P6时旧配置/clients保持原wire。
type9 GET_HEALTH可选请求 {"include_components":1}；{} 原格式保持逐字段原语义。type7 process rows不新增link字段；type8/10/5保持原device合同。

## 方向与形状

所有新JSON字段均严格校验duplicate keys/unknown keys/type/UTF-8/null/range；旧parser行为仅新命令局部扩展，不引JSON SDK。整数64位身份/seq用canonical decimal string（无符号、无前导零，零仅revision初始允许），避免既有signed parser与JSON精度限制。request_id非零uint32，connection内至多一个new command outstanding，reply原request_id。0仍留给原出站通知。

Service→Runtime type11 BIND：
```json
{"version":1,"op":"bind","service_name":"control_service"}
```
Runtime→Service成功同type11：
```json
{"version":1,"op":"bind","result":"OK","runtime_epoch":"00112233445566778899aabbccddeeff","session_id":"1","service_name":"control_service","instance_generation":"3","report_interval_ms":1000,"lease_ms":3500}
```
runtime_epoch例为格式，实际启动时OS entropy生成128bit；失败启动报错，禁止时间戳充当认证。session_id writer发放单调正uint64，不复用；溢出拒绝并报fatal管理错误，不wrap。
bind不发布HEALTHY；只将当前观测UNKNOWN、等待fresh报告。重复同conn bind只返回同session，不清已有health或renewlease。

REPORT请求：
```json
{"version":1,"op":"report","runtime_epoch":"00112233445566778899aabbccddeeff","session_id":"1","report_seq":"1","component":"mcu_link","status":"UNHEALTHY","reason_code":"link_lost","reason":"owner watchdog declared loss","projection_revision":"12","link_epoch":"2","peer_boot_id":1234,"sample_kind":"issue"}
```
sample_kind=current或issue；current是当前projection，issue是owner待确认摘要。只有issue被writer应用并ACK后client才确认owner的issue revision；再发current。issue内容是同样五状态+原因，无UART字节。
成功report ACK同type11：
```json
{"version":1,"op":"report","result":"APPLIED","session_id":"1","report_seq":"1","projection_revision":"12"}
```
ACK在writer完成identity gate/事实提交后才产生，不是IPC“入队成功”。同seq同canonical payload重复返回同ACK不续lease；client用ACK确认issue，不能用send成功清issue。不同seq可沿用projection_revision以renew当前状态；issue->current允许same revision（从同一coherent snapshot取出），其他same revision changedstatus非法。issue只允许DEGRADED/UNHEALTHY；不接受issueHEALTHY。currentHEALTHY后旧issue revision不得再次恢复旧fault，除非owner仍有新的未ACKissue。
所有monotonic字段只在同producer launch内比较；bind重连同launch不能把旧seq作为fresh，重新sessionseq从1但保留ownerrevision/epoch和writer已应用revision下界。新launch归零/重建；旧session永远不可复用。

## 身份合同：server决定

1. accept捕获SO_PEERCRED(pid/uid/gid)、connection token和accepted_at；socket目录/文件限制为部署UID可连接。UID相同不足以身份认证。
2. BIND经writer：service_name必须存在并被component配置指定为producer；SM当前state RUNNING、PID>0、launched_generation>0；peer PID必须精确等于当前前台直接child PID。拒绝shell wrapper、daemon后代、自报PID/generation、任意其他service。
3. accepted_at必须不早于SM当前launch start_time；防旧socket peerPID数字复用后被误绑定。child太早连接（exec登记未完成）收到1004 RETRY_BIND，应关闭重连，不能等到新实例后把旧socket重新归属。依据同一Runtime monotonic域，非producertimestamp。
4. writer从SM复制launch token与runtime_epoch并发session；新合法conn bind撤销同producer旧session。任意report/heartbeat在提交时再次检查PID/token/currentRUNNING/session未revoked及epoch；不能只在IPC读取时检查。
5. SO_PEERCRED在connect时固定，不证明每次write的实际进程；本契约禁止delegating/继承已连接UDS fd、daemonizing或外部reaper。root/被授权owner自身被攻陷不在此认证保证内。任意别的service新建socket自报名称无法通过。
6. process lifecycle改变立即revoke；fault/STOP后的launched_generation虽仍供reap，不允许report。Runtime重启epoch变化，新Runtime不adopt遗留orphan；旧child需部署清理后由SM重新launch。

启用component producer的type4 HEARTBEAT必须使用同已绑定service socket：原{"service_name":...,"timestamp":整数}格式保持，timestamp只metadata；IPC附加server PID/instance token/session到内部envelope，writer重验证后才交SM/Monitor。禁止它绕回旧按名称入口；SM处理新增instance_generation gate，旧合法内部事件仍按原语义。
未启用component的legacyservice保留旧兼容路径；此包不宣称全面解决旧服务认证，P6producer路径必须安全。legacy client不能替componentproducer发type4以躲过绑定。绑成功前无acceptedheartbeat，既有启动grace内client重试。

## freshness / disconnect / errors

received_at由server读取完整合法report时捕获；writer若now-received_at>=3500ms则1003过期拒绝，不允许迟到缓存健康重新续租。producer重试必须重新读取projection、用新seq/current观察，不复用旧received时间。
stable current每1000ms续租；issue及时stop-and-wait。reportdeadline1s，无ACK关闭重连；UDS重试1s，有限buffer，无tight loop，wire仍独立运行。
disconnect/half-close/overflow立即revokesession，writer应用UNKNOWN；retainadverse，neverfreshhealthy。接受前disconnect可拒绝；已提交事实不撤销已发生history，但观测即失效。同launchreconnect不能让旧disconnect tombstone撤销新session。

ERROR255沿用1001 unknown service/component、1002 malformed/方向/版本/非法seq内容、1003 unavailable/full/shutdown/过期、新增1004 identity/session/stale producer或RETRY_BIND。reason不泄漏系统路径/凭据。1004关闭该producerconn，newbind恢复；配置没有授权不是可忽略错误。成功回type11；controlchanneltype11拒绝1002。pending reply不得写复用fd；用connectiontoken/weaklifetime关联。

## bounds / query

health命令payload<=4096 bytes；reason<=256；componentkey静态；每conn一inflight；private ingress<=32，拒满不伪ACK；原output cap4*(10+65536)保持。新commands受每轮16帧公平配额及输入上限，断连/STOP/lease不能被持续renew饿死。bounded故障摘要保留在owner，Runtime不保存全metrics历史。
GET_HEALTH opt-in在原device_state/服务process rows之外追加 components 数组：component、producer、instance_generation、status、reason_code、peer_boot_id、link_epoch、projection_revision、report_age_ms/null、observation_fresh、required、criticality。只读writer已提交snapshot副本，不访问tty/重新计算wirehealth；整体device+process+component读取仍非事务原子，字段携identity/time，不声称atomic。
type10仍只DEVICE_STATE_CHANGED，不新增component原始stream。健康改变而device不变通过opt-inquery可观察，issue不保证每次设备通知。

## 投递顺序与重连的补充冻结规则

- writer 分开保存“最后接受的 owner sample 元数据”和“当前有效观测”；断连生成 UNKNOWN 不改写 owner revision 下界、最后 sample_kind 或已应用 issue 的确认记录。
- issue 摘要的 projection_revision 是该摘要覆盖到的最新故障高水位；current 的 revision 是当前语义版本。二者取自一次 coherent read，可不同；两者相同而状态不同，仅允许已 APPLIED 的 issue 紧接同次快照 current。接受 current 后关闭该例外。高 seq 不单独授予修改同 revision 内容的权力。
- 同 launch 重连保留上述确认记录。若 issue 已应用而 ACK 丢失，允许新 session 原样重发该 issue，以 APPLIED ACK 完成确认；这是重复事实，不发第二次 event。此时有效观测仍可由合法新报告重新建立，但单纯重复旧 seq 仍不续租。若 current 已应用，旧 issue 不得回滚它。
- client 每次 ACK 后重新读取 owner，若出现更高 revision 的 pending issue，先处理新 issue，不能用此前缓存 HEALTHY 越过它。AckHealthIssueThrough 在同一 owner 锁内只清其覆盖 revision，不清更新的故障。
- link_epoch 在同 launch 内不回退；首次 protocol session 与重新建立 session 递增，boot_id 不是排序键。新 launch 不继承旧 epoch 下界。
- report 的 reason_code 仅接受 owner 的 initial/opening/negotiating/ready/response_timeout/link_lost/open_failed/hello_rejected/startup_not_ready/peer_reboot/recovered/owner_stopped；observation_unavailable/report_expired 是 Runtime 本地派生原因，不允许 producer 伪造。校验 status 与 reason 的映射符合 MCU_HEALTH_MODEL；issue 只能为已确认 DEGRADED/UNHEALTHY。
- runtime_epoch/session 是隔离重启和重放的身份上下文；真实 producer 授权依赖 SO_PEERCRED 与 SM 当前实例重验，随机 epoch 本身不证明发送者身份。
