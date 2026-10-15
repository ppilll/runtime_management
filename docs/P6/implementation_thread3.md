# P6 Thread 3 — Authenticated Health IPC / Static Configuration

日期：2026-10-15（Asia/Shanghai）。执行 `codex_package/thread3_prompt.md` /
`thread3_task.md`。按用户要求，本次仅进行静态检测，不要求、不展开编译测试报告。
下文 PASS 仅指所列 Python 文本/结构检查，不表示测试源码已执行。

## 基线与范围

HEAD：`f8507027c87a9ae9899f51da708f8d469552c473`。保留进入时已有的 T1 工作树改动、
根 AGENTS.md、docs/P6、component_health/health_policy 头、component_health_tests.cpp 和 temp2.log。
T1 交接报告声明模型/private admission 已冻结，本任务在其基础上接线。
已读取根/P6规则、README、CHANGE_LIST、freeze、T3任务、T1报告/fixture、P6 IPC/config/event/
aggregation/recovery 合同，以及指定 P4/P5 owner 文档和 P1/P3 IPC 合同及当前代码。

只改 T3 Allowed Files：config 头源、frame.hpp、IPC 头源/main、runtime_manager.cpp 的配置合并接线、
新 component_ipc_tests.cpp、tests/CMakeLists.txt 的追加注册及本报告。
Runtime 头不需再修改；根 CMake、T1 模型/fixture/aggregation、SM、RM、后端、DSM、Collector、
Monitor、Logger、旧测试/静态检查器与 P0–P5 历史文档保持原任务边界。
未改 VisionArm/MCU，未安装依赖、创建其他 chat/subagent 或提交代码。

## Implemented

| 文件 | 交付 |
| --- | --- |
| include/runtime/config_manager.hpp / src/config/config_manager.cpp | RuntimeConfig 尾部 HealthConfig；显式 policy/producer；严格 health 子 schema；JSON/程序入口同校验；load_file 仍验证完整配置 |
| src/runtime/runtime_manager.cpp | 配置 policy 与 AggregationOptions 同值合并、冲突拒绝；保持原 programmatic 入口；使用同一 component registry 构造 T1 核心 |
| src/ipc/frame.hpp | 仅追加 HEALTH_REPORT=11；原编号/帧尺寸保留 |
| src/ipc/ipc_manager.hpp / .cpp | 私有 adapter、真实 peer、type11 parser/weak completion、bound type4、输入/输出边界、公平配额、opt-in components 查询 |
| src/ipc/main.cpp | 一次完整 load；同一 core/registry、服务列表、组件 committed cache 接线；CLI/socket/device sink 保留 |
| tests/component_ipc_tests.cpp | 13 个含断言、main 接线的测试函数；真实前台 child/UDS 与配置测试源码 |
| tests/CMakeLists.txt | 追加 phase6_component_ipc；保留原 12 executables / 13 CTest 和 T1 新注册，当前合计 14 / 15 |

HealthConfig 没有 UART 参数、report/lease 可配 timer 或按服务名称推断 component policy。
health 不存在时 registry 为空；single-service root、services root、monitoring 和 1MiB 限制保留。
services policy 最多32项，component 0..1 且只允许 mcu_link；producer 必须已注册、autostart、
heartbeat_timeout>=4s。required 必须 bool，criticality 只接受 HIGH/MEDIUM/LOW；重复、未知字段、
缺字段、错误类型/null、未知 service/producer、非法 key/UTF-8 都拒绝。
legacy vision_required 的 programmatic 行为保留；新 JSON 显式 vision policy 与该开关冲突时拒绝。

## Wire / identity / lifecycle 审查

| 边界 | 当前接线 |
| --- | --- |
| framing | 原10-byte LE、64KiB legacy cap、types1..10/255；frame.cpp 未改；EVENT5 仍仅出站 |
| type11 | service channel 的 bind/report；非零 request_id；payload<=4096；严格 decoded duplicate/unknown key、版本/类型/null/UTF-8、owner reason/status/kind 校验 |
| 数值 | epoch 为32个小写hex；session/seq/revision/link_epoch 为 canonical uint64 字符串，逐位溢出检查；零只允许初始 revision；boot_id 为 null 或 uint32整数，0有效 |
| peer | accept4 后 SO_PEERCRED 读取 PID/UID/GID、server steady accepted_at、单调不复用 token；service socket 限 owner 读写；UID相同不代替PID认证 |
| bind | T1 writer 核验 exact SM foreground PID、RUNNING、start_time、launched_generation；server 发 epoch/session，早连接或旧身份1004要求重连；同conn重复bind不续租 |
| report | T1 在应用时重新核验 peer/session/epoch/当前实例；旧序号、revision、issue/current、重连确认、lease 下界仍归 T1 核心；不引第二套状态判断 |
| ACK | submit=true 仅表示入队；writer 完成事实派发、DSM 与 committed cache 后才回调；epoll 才生成 OK/APPLIED；成功type4继续无wire ACK |
| reply lifetime | 每conn一个 typed completion slot、core仅持weak sink；writer callback noexcept，无socket I/O；新conn新对象，旧回复不按fd寻找新连接 |
| bounds | T1 private outstanding<=32/writer batch<=8；每conn一pending，并暂存最多一份最新单向heartbeat；service输入逐header/body读取；每epoll轮整个service入口最多16帧/16次accept；原4*(10+65536)输出cap保留 |
| disconnect | EOF/HUP/overflow/removal/transport teardown 原子撤销peer；不要求入队空位；T1 checkpoint 派生UNKNOWN，不提交 RecoveryRequest |
| type4 | 原 service_name/timestamp JSON；P6 producer 必须同bound socket；附 server peer/epoch/session/received_at 走private writer；不走name-only post；不续component lease |
| type9 | {} 保持原字段/语义；仅 {"include_components":1} 追加 components；复制writer cache，不访问SM或推进lease；整体仍非事务快照 |
| shutdown | T1 shutdown gate/weak mailbox继续拒绝late ingress；IPC线程退出时撤销/关闭peer，stop执行join；不等待ACK退出；原RM cancel/grace/reap未改 |

1001：未知 service/component 或未启用producer；1002：格式/方向/版本/字段/非法数值和内容；
1003：不可用、满队列/一pending、关闭/过期；1004：身份/session/旧producer，随后关闭该连接。
错误仍为type255，保留request_id，只给安全诊断，不序列化内部身份字段。
同轮heartbeat+report先处理报告，再提交一份coalesced heartbeat；保留heartbeat原接收时间，满mailbox时
不生成新时间。已提交heartbeat尚未完成时重叠type11仍可收到1003；调用方保留issue并重试，
不能将1003或send成功当成APPLIED。五种组件状态仍为 UNKNOWN/CONNECTING/HEALTHY/DEGRADED/UNHEALTHY。

该配额仅覆盖service入口；未改旧control客户端全局背压，不声称全局DoS或硬实时调度问题已解决。
socket目录及部署UID由部署保证；连接fd继承/转交、daemon/wrapper与外部reaper仍不受支持。
与原Query/QueryDevice一样，QueryComponents回调的被捕获core须覆盖IPC运行寿命；main的析构顺序满足此约束。

## 测试源码映射

下列函数均位于 tests/component_ipc_tests.cpp，省略 test_ 前缀。只表示源码交付与静态核对。

| P6 IDs | 函数 / 可观察断言 |
| --- | --- |
| 27 | strict_health_parser_and_direction：版本/duplicate/unknown/type/null/UTF-8/数值/方向，格式错误不进入mailbox |
| 27 | integer_null_utf8_and_exact_payload_boundaries：uint64高位/最大值、boot null/最大值、256-byte reason、4096/4097边界 |
| 28 / 10 | async_ack_queue_cap_tombstone_and_control_fairness：停writer时无ACK，32满队列、一pending、断连后新child/control可用 |
| 23 / 33 / 27 | real_peer_bound_heartbeat_and_committed_query：同UID真实非child PID拒绝、未绑定type4拒绝、同次write的heartbeat+report、单向心跳无成功ACK、APPLIED后cache、opt-in及legacy字段、独占reap/ECHILD |
| 10 / 19 / 25 | reconnect_replay_revision_and_epoch_isolation：重复不续租、lost-ACK issue重连确认、issue/current、旧revision/seq/session/epoch拒绝 |
| 32 / 26 | unknown_registry_duplicate_bind_and_normalized_owner_failures：未知注册、重复bind、normalized open_failed无process action、future query不推进expiry |
| 19 | restart_runtime_epoch_and_old_instance_report：重建Runtime后旧epoch/session不使新实例fresh |
| 09 | new_launch_rejects_previous_session_and_resets_owner_bounds：真实STOP/reap/START后旧session拒绝，新launch revision下界重建 |
| 04 / 27 | subscription_and_link_loss_preserve_process_identity：原type10/type5次序，component loss/recovery通知，PID/token/预算不变 |
| 10 / 18 | disconnect_and_process_exit_are_independent：EOF先UNKNOWN而process仍RUNNING；真实异常退出走原SM生命周期 |
| 34 | health_config_legacy_strict_schema_and_wrapper：旧root/monitoring、新policy/registry、wrapper同验证、非法schema |
| 34 | config_policy_merges_and_conflicts：同值合并、service/component冲突、未知producer |
| 34 | config_registry_limits_and_legacy_vision_option：32/33 policy上限、未知service、legacy开关与新显式policy边界 |

P6-24的精确早bind/登记竞态、P6-09的相同数值PID换launch、P6-26的3500ms等号与stale queued report
继续由T1的 bind_exact_pid_launch_start_and_early_retry、launch_change_same_pid_and_shutdown_gate、
lease_exact_boundary_query_and_stale_queue fake-clock 源码覆盖；本任务没有伪造真实socket结果替代这些门。
P6-32此处仅接收normalized owner失败，不实现或模拟串口open/HELLO；owner产品路径属T2/T4。

## Statically Verified

Windows / PowerShell，现有 Python -B、标准库。旧checkers未迁移、未删除保护项。

| 命令 | exit | 结果 |
| --- | --- | --- |
| python -B tests/phase2_static_check.py | 0 | 47 C++文件local include/词法/括号；189个main接线函数；14 targets / 15 CTest |
| python -B tests/phase3_validation_static_check.py | 1 | P2/state部分通过；旧8 internal events与旧types1..10枚举冻结失败 |
| python -B tests/phase4_static_check.py | 1 | frozen interface/backend changed: src/ipc/frame.hpp |
| python -B tests/phase5_static_check.py | 1 | P5 protected file changed；首项随set遍历可变化 |
| 下附T3专项audit，python -B - | 0 | 143个非T1/T3 tracked文件不变；旧源码保护/fixture/新路径/测试接线 |
| git -c core.safecrlf=false diff --check | 0 | tracked diff格式；untracked源码由专项audit另检查 |

实际旧checker诊断包括：

```text
FAIL: required internal event kinds differ
FAIL: wire types differ from Phase1/2/3 contract
FAIL: Phase3 aggregation; Phase3 IPC
FAIL (static): frozen interface/backend changed: src/ipc/frame.hpp
FAIL (P5 static): P5 protected file changed: include/runtime/event.hpp
```

前三类冻结冲突已由 CHANGE_LIST 列为T4迁移项；本任务不把它们改写为PASS。
phase6_static_check.py 属T4 Allowed Files，目前未创建，不宣称P6全静态通过。

路径审查补充了同轮heartbeat/report的暂存接线与真实child组合write断言，避免心跳系统性抢占报告槽。
这只改变传输调度；没有修改T1模型、私有32-operation上限或writer身份/lease语义。

专项audit初次exit1：将handler注释里的APPLIED也当作直接生成ACK。改成只匹配字符串字面量后
exit0；未因此修改被测源码或旧checker。一次报告提取命令遗漏UTF-8编码，使用Windows默认GBK读取失败；
显式encoding后恢复，审查脚本本身未执行失败的提取步骤。最终输出：

```text
PASS (T3 audit): 143 non-T1/T3 tracked files unchanged; legacy config functions/structs, SM instance-only extension, old test registration prefix retained
PASS (T3 audit): 47 C++ files; 189 main-wired tests; 14 targets / 15 registrations; 13 new assertion-bearing tests; DSM declaration contract retained
PASS (T3 audit): frozen T1 fixture SHA256; original wire types +11, 10-byte/64KiB; peer capture, bounded private replies/inputs, writer-only APPLIED, bound type4, single-load query wiring
LIMIT: text/structure checks only; no behavioral or timing result
```

## 可复现T3专项audit

在仓库根将以下正文通过 PowerShell here-string 管道交给 `python -B -`。
它只读取源码/HEAD，不写历史checker，也不执行C++。

```python
from pathlib import Path
import sys, re, subprocess, hashlib
sys.path.insert(0, 'tests')
from phase5_static_check import function
from phase2_static_check import check_source, check_registration, masked_source
from phase3_aggregation_static_check import check_aggregate_contract

def read(p): return Path(p).read_text(encoding='utf-8-sig')
def old(p): return subprocess.check_output(['git', 'show', 'HEAD:' + p]).decode('utf-8-sig').replace('\r\n', '\n')
t1 = {'include/runtime/event.hpp', 'include/runtime/runtime_manager.hpp', 'include/runtime/service_aggregation.hpp', 'src/runtime/event.cpp', 'src/runtime/runtime_manager.cpp', 'src/runtime/service_aggregation.cpp', 'src/service/service_manager.cpp', 'tests/CMakeLists.txt'}
t3 = {'include/runtime/config_manager.hpp', 'src/config/config_manager.cpp', 'src/ipc/frame.hpp', 'src/ipc/ipc_manager.hpp', 'src/ipc/ipc_manager.cpp', 'src/ipc/main.cpp', 'src/runtime/runtime_manager.cpp', 'tests/CMakeLists.txt'}
changed = set(subprocess.check_output(['git', '-c', 'core.safecrlf=false', 'diff', '--name-only']).decode().splitlines())
assert changed <= t1 | t3, changed - (t1 | t3)
frozen = [p for p in subprocess.check_output(['git', 'ls-files']).decode().splitlines() if p not in t1 | t3]
for p in frozen: assert read(p) == old(p), p
config = read('src/config/config_manager.cpp')
base = old('src/config/config_manager.cpp')
for pattern in [r'class Parser', r'ServiceConfig service\(const Json& value\)', r'MonitoringConfig monitoring\(const Object& root\)', r'void ConfigManager::validate\(const ServiceConfig& config\)', r'void ConfigManager::validate\(const MonitoringConfig& config\)', r'std::chrono::seconds ConfigManager::recoveryTimeout\(const ServiceConfig& config\)', r'std::vector<ServiceConfig> ConfigManager::load_file\(const std::string& path\)']:
    assert function(config, pattern) == function(base, pattern), pattern
for name in ['ServiceConfig', 'MonitoringConfig']:
    pattern = r'struct ' + name + r'\s*\{.*?\n\};'
    assert re.search(pattern, read('include/runtime/config_manager.hpp'), re.S).group() == re.search(pattern, old('include/runtime/config_manager.hpp'), re.S).group()
predicate = '                (!event.instance_generation || *event.instance_generation == service.status.launched_generation) &&\n'
assert read('src/service/service_manager.cpp').replace(predicate, '', 1) == old('src/service/service_manager.cpp')
assert read('tests/CMakeLists.txt').startswith(old('tests/CMakeLists.txt'))
assert check_registration() == (14, 15)
assert check_aggregate_contract() == (9, 24)
files = sorted(p for d in ('include', 'src', 'tools', 'tests') for p in Path(d).rglob('*') if p.suffix in ('.hpp', '.cpp'))
scenarios = sum(check_source(p.resolve()) for p in files)
assert hashlib.sha256(Path('docs/P6/fixtures/health_contract_v1.json').read_bytes()).hexdigest() == '9c3a7d1543d259a281eb8003f80c7fe09254e029719194b6453c00629dac3680'
header = read('src/ipc/frame.hpp')
current = dict((k, int(v)) for k, v in re.findall(r'(\w+)\s*=\s*(\d+)', header))
previous = dict((k, int(v)) for k, v in re.findall(r'(\w+)\s*=\s*(\d+)', old('src/ipc/frame.hpp')))
assert current == {**previous, 'health_report': 11}
assert 'header_size = 10' in header and 'max_payload = 64 * 1024' in header
ipc = read('src/ipc/ipc_manager.cpp')
for token in ['SO_PEERCRED', 'connection.peer->pid = credentials.pid', 'connection.peer->accepted_at = accepted_at', 'connection.peer->connection_token = next_connection_token_', 'service_frames < 16', 'length > component_payload_limit', 'client.peer->revoked = true', 'std::weak_ptr<ComponentPeer> peer', 'std::optional<ComponentReply> ready', 'completed(const ComponentReply& reply) noexcept', 'client.health_inflight', 'client.binding.runtime_epoch', 'client.binding.session_id', 'component_json(view)', 'events->pending.size() > 1024', 'client.output.size() + encoded.size() > max_queued_output']:
    assert token in ipc, token
handler = function(ipc, r'auto handle = \[&\]\(Client& client, const ipc::Frame& frame\)')
assert not re.search(r'"[^\n]*APPLIED', handler)
callback = function(ipc, r'void completed\(const ComponentReply& reply\) noexcept override')
assert not re.search(r'\b(?:send|recv|write)\s*\(', masked_source(callback))
assert 'std::weak_ptr<ComponentReplySink> reply' in read('include/runtime/component_health.hpp')
assert ipc.count('ingress.received_at = Clock::now()') == 1
assert 'client.heartbeat_received_at = Clock::now()' in ipc and 'ingress.received_at = client.heartbeat_received_at' in ipc
assert 'bool heartbeat_waiting = false' in ipc and 'if (components_->submit_(std::move(ingress)))' in ipc
main = read('src/ipc/main.cpp')
assert main.count('load_runtime_file(') == 1 and 'ComponentTransportAdapter>(core)' in main and 'core.queryComponentHealth()' in main
for key in ['session_id', 'report_seq', 'projection_revision', 'link_epoch']: assert 'decimal("' + key + '",' in ipc
assert '(std::numeric_limits<std::uint64_t>::max() - n) / 10' in ipc
source = read('tests/component_ipc_tests.cpp')
names = re.findall(r'\bvoid\s+(test_\w+)\s*\(', masked_source(source))
assert len(names) == 13
for name in names:
    body = function(source, r'\bvoid\s+' + name + r'\([^)]*\)')
    assert 'require(' in body or 'expect_error(' in body, name
    assert name + '();' in source[source.index('int main('):], name
for p in sorted(t3 | {'tests/component_ipc_tests.cpp', 'docs/P6/implementation_thread3.md'}):
    text = read(p)
    assert text.endswith('\n') and all(line == line.rstrip() for line in text.splitlines()), p
new = '\n'.join(read(p) for p in t3 if p.endswith(('.hpp', '.cpp')))
assert not re.search(r'#\s*include[^\n]*(?:uart|serial)|\b(?:tcsetattr|TIOCSRS485|UartLink|SerialPort)\b|/dev/tty', new, re.I)
print('PASS (T3 audit):', len(frozen), 'non-T1/T3 tracked files unchanged; legacy config functions/structs, SM instance-only extension, old test registration prefix retained')
print('PASS (T3 audit):', len(files), 'C++ files;', scenarios, 'main-wired tests; 14 targets / 15 registrations; 13 new assertion-bearing tests; DSM declaration contract retained')
print('PASS (T3 audit): frozen T1 fixture SHA256; original wire types +11, 10-byte/64KiB; peer capture, bounded private replies/inputs, writer-only APPLIED, bound type4, single-load query wiring')
print('LIMIT: text/structure checks only; no behavioral or timing result')
```

## Not Verified / 后续交接

静态文本/结构检查不能证明真实peer、fd关闭/复用、writer/epoll竞态、半关闭/压力、lease时序、
调度公平性或性能。测试函数含真实进程/socket操作，但函数存在不构成行为证据。
T2仍须核对同版fixture与上述heartbeat pending语义；没有在当前checkout核对VisionArm实际改动或做联调。
T4继续完成旧checker的批准迁移、统一phase6静态入口和联合场景审查；本报告保留已知静态失败，
不宣布整体Phase6验收完成。
