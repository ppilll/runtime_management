# P6 Thread 1 — Contract / Admission / Aggregation Implementation

日期：2026-10-03（Asia/Shanghai）。任务：`codex_package/thread1_prompt.md` /
`thread1_task.md`。本次用户明确授权执行 T1，并限定只进行静态检测；编译测试不作为要求，
不生成编译测试报告。本报告的 PASS 仅指明确列出的静态检查。

## 基线与范围

Runtime 本地 HEAD 为 `f8507027c87a9ae9899f51da708f8d469552c473`，与 P6 SOURCE_MANIFEST 一致。
开始时 tracked 工作树无修改；根 AGENTS.md、docs/P6/、temp2.log 已是用户提供的 untracked 内容，
本次保留它们。当前父目录只有 Runtime checkout，没有 VisionArm / MCU checkout，未声称复核其实际 HEAD
或本地差异；合同引用其固定基线 `05148303c5ec5bba0ee89e707ab340e94e8aa3e1` /
`92d8f45bc9c7df55f6e2abea4b4abfeb090a3e3d`，后续跨仓库执行仍须核对。

已读根/P6规则、README、CHANGE_LIST、T1任务、freeze、P6健康/事件/聚合/IPC/owner合同，
以及 P3事件/聚合、P4架构/恢复策略/服务接线/设备状态/validation、P5架构/设备状态/validation
和当前 Runtime/SM/event/aggregation 头源。配置章用于核对显式 registry 边界。

仅修改 T1 Allowed Files。没有改 IPC/config/main、VisionArm/MCU、RM、ProcessSupervisor、
DSM表、Collector/Monitor/Logger、旧测试、旧checker或P0–P5文档。没有创建其他 chat/subagent、
提交或安装依赖。

## Implemented

| 文件 | 交付 |
| --- | --- |
| include/runtime/health_policy.hpp（新） | 原 ServiceCriticality / ServiceHealthPolicy 名称和默认值移入轻头；统一 policy 合法性 |
| include/runtime/component_health.hpp（新） | 五状态、reason、sample/current/issue、identity/fact/snapshot、显式 policy、typed ingress / peer tombstone / weak reply；1000/3500ms、4096/32边界常量 |
| include/runtime/event.hpp / src/runtime/event.cpp | 原八 RuntimeEventType 保持编号顺序，尾部新增唯一 component_health_changed 和 typed fact；Event 尾部私有 coalesced wake batch |
| include/runtime/service_aggregation.hpp / src/runtime/service_aggregation.cpp | 单 mcu_link registry、同 launch ever-healthy/adverse、required readiness、HIGH component 优先于 process recovery、局部 clear 后全量 evaluate |
| include/runtime/runtime_manager.hpp / src/runtime/runtime_manager.cpp | ComponentAdmission writer核心、私有弱 sink、有界 mailbox、会话/序号/revision/lease、lifecycle撤销、writer提交缓存、只读组件query、shutdown gate |
| src/service/service_manager.cpp | heartbeat 仅追加 instance_generation 对 launched_generation 的谓词；其余全文保持 |
| tests/component_health_tests.cpp（新） / tests/CMakeLists.txt | 17 个 unit/fake clock 测试源码，新增 phase6_component_health_unit；原注册前缀完整保留 |
| docs/P6/fixtures/health_contract_v1.json（新） | v1 request schema、reply样例、语义映射、数值/boot0、九组 issue/ACK/reconnect/lease/shutdown 顺序 fixture |
| docs/P6/implementation_thread1.md | 当前证据、冻结接口及后续交接 |

根 CMakeLists.txt 不需要变动；新增模型函数与 admission 核心放在已注册的 runtime_manager.cpp。
service_manager.hpp 不需要新增 identity API，复用现有 query() 的 SM 状态副本。

## 冻结接口 / T2–T3 交接

合同 fixture version=1，frozen_by=P6-T1。该文件 SHA256：
`9c3a7d1543d259a281eb8003f80c7fe09254e029719194b6453c00629dac3680`。
T2/T3 使用同版只读 docs/P6 与 fixture；不复制业务、UART 或 Runtime C++ 源到另一仓库。

- `ComponentPolicyEntry(component, producer_service, ServiceHealthPolicy)` 三参数必须显式提供；
  Runtime programmatic 入口使用 `AggregationOptions::components`，为空保持旧行为。只能启用一个 mcu_link；
  producer 必须在 registry、autostart=true、heartbeat_timeout>=4s。T3 将 JSON HealthConfig 合并到同一静态配置，
  并处理原 service policy 同值合并/冲突拒绝；T1 没有 JSON 接线。
- `ComponentSample` 的 revision/epoch 为 uint64，boot 为 optional uint32（0有效）；只允许 owner reason，
  reason按 UTF-8 encoded bytes 校验<=256。新 JSON canonical decimal strings、duplicate/unknown key、
  message size/direction/version 等由 T3 parser 校验；fixture 的 regex 不能代替 uint64上界和语义校验。
- `ComponentAdmission::apply(ingress, writer_now)` / `checkpoint(now)` / `snapshot(now)` /
  `stop()` 是无 I/O、无 process policy 的 writer 核心。Lookup生产只读SM当前状态，unit注入捕获peer/状态/时钟；
  不把 fake PID 测试当真实 SO_PEERCRED 证据。
- Runtime 的 `component_sink()` 是 private，只允许 `runtime::ComponentTransportAdapter`（T3可在
  ipc_manager.hpp定义）和受限测试access获取。T3在构造时用 adapter 获取此 sink，再注入 IPC；
  不能新增 public trusted flag、公开 component post 或绕回按名称 heartbeat。
- `ComponentPeer` 必须由 service socket accept 的 server SO_PEERCRED / monotonic accepted_at /
  唯一 connection_token 创建，禁止从 JSON 构造。每个连接一份 shared peer；断开立即原子写
  `revoked=true`，不依赖 queue 空位。新连接使用新 peer，不能重新激活旧记录或向新 session 复用旧 tombstone。
- bind/report request_id非零；heartbeat内部兼容0。BIND返回 server派生 PID/launched_generation/epoch/session。
  epoch启用健康时用 getrandom 生成128位；熵失败抛管理错误，不用时间替代。session正uint64全 Runtime寿命单调，
  最大值后拒绝并抛 fatal 管理错误，禁止 wrap。
- submit返回 bool仅表示有限入队，false由T3返回1003，不发送 APPLIED。queued+writer batch的 outstanding<=32；
  每wake最多8个操作，最多一个coalesced wake。wake在原FIFO中捕获batch，后来操作重新排wake，不能跨越已提交STOP。
  每个writer turn/lifecycle checkpoint检查revocation/lease；保留旧全局queue的调度限制。
- `ComponentReplySink::completed` 接受 writer 应用结果，仅向有限weak连接输出队列提交，不能 socket I/O或无限等待；
  callback不得throw。identity错误255回复仅序列化error/code/request_id与安全诊断，不输出reply内部身份字段。
  T3保留原输出cap，并实现type11每epoll轮16帧输入配额及4096-byte上限。
- HEALTH_REPORT应用后，经 fact dispatcher / aggregation / DSM 与 committed cache更新，才调用weak reply。
  type4原JSON保留，IPC通过绑定连接附上内部 epoch/session/peer 和 server received_at；
  Runtime再验证后直接交SM并附 launched_generation。process heartbeat不续 component lease。
- `queryComponentHealth(now)` 只复制writer committed cache并派生age，不访问SM、不推进lease/state；
  T3用于type9 opt-in投影；原 {} GET_HEALTH、types1..10/255、10-byte/64KiB、EVENT方向保持。
  query的fresh字段表示最后writer checkpoint已提交的观测，不保证查询时推进expiry或整包事务一致。
- core分别保存last_owner与有效观测：断连/过期派生UNKNOWN不改revision/epoch/kind；
  same seq identical只重ACK不续租；higher seq stable current续租不重复fact。
  同revision changed sample仅有已应用issue→current的窄例外，epoch和boot evidence必须相同。
  issue APPLIED但ACK丢失后同launch重连，可原样重发确认而不重复发布issue；
  current已应用则关闭例外、拒绝旧issue回滚。新issue更高revision使旧缓存HEALTHY失败。
- T2 owner负责coherent read、pending issue最高severity摘要、AckThrough和ACK后重新读取；Runtime只消费normalized facts。
  新launch重建owner下界/readiness/adverse；同boot ID绝不表示未reboot。wire watchdog/reopen/metrics留owner。
- lifecycle离开RUNNING或实例变化立即撤销；public post(Event)/post(RuntimeEvent)无权提交component事实；
  P6producer name-only heartbeat拒绝。shutdown gate关闭mailbox并停止core，不再提交component状态，
  原RM cancel/grace/reap、线程join后缀保持。

T1 model/private接口及fixture已锁定，可以交接T2与T3；T1没有实现service-channel传输或生产产品接线。
shared header / CMake / checker 后续修改继续串行，T4等双方完成。

## 测试源码映射

下列只表示 assertion-bearing 测试源码已交付和静态结构核对。

| P6 ID | 新函数（均在component_health_tests.cpp，省略test_前缀） |
| --- | --- |
| 01 / 11 | required_readiness_high_loss_and_local_latch |
| 04 | session_overflow_never_wraps_and_link_loss_has_no_process_action |
| 09 | bind_exact_pid_launch_start_and_early_retry；launch_change_same_pid_and_shutdown_gate |
| 12 | optional_low_required_low_and_optional_high |
| 13 / 14 | clear_reaggregates_resources_process_recovery_and_terminal；component_clear_preserves_unrecovered_process_and_invalid_registry |
| 25 | sequence_duplicate_no_renewal_conflict_and_lower_bound；issue_current_same_revision_and_new_issue_priority；ack_lost_reconnect_preserves_owner_and_confirmation |
| 26 | lease_exact_boundary_query_and_stale_queue |
| 35 | clear_reaggregates_resources_process_recovery_and_terminal；required_readiness_high_loss_and_local_latch |
| 身份/关闭/边界补充 | normalized_status_reason_and_utf8_bounds；stop_committed_rejects_health_and_bound_heartbeat；epoch_and_replaced_session_identity；service_manager_bound_heartbeat_instance_gate；private_queue_bound_pending_revocation_and_weak_lifetime；public_component_facts_and_name_heartbeat_cannot_bypass |

新源码17个test函数，仓库共176个main接线test函数；13 executable / 14 CTest注册，
原12 / 13完整保留。DSM源码/旧测试完整保留，原24 allowed / 417 rejected声明空间保持。
没有 DISABLED / WILL_FAIL、预算/PID/ECHILD弱化或六秒回归修改。

## Statically Verified / 实际命令

Windows / PowerShell，使用现有 python 的 -B（python3同等替代），标准库，不安装依赖。

| 检查 | 修改前exit | 最终exit | 证据 |
| --- | --- | --- | --- |
| python -B tests/phase2_static_check.py | 经P3入口0 | 0 | 46个C++文件local include/词法/括号、176main接线函数、13/14注册 |
| python -B tests/phase3_validation_static_check.py | 0 | 1 | state/IPC/源码结构部分通过；新增事实触发旧八枚举冻结断言 |
| python -B tests/phase4_static_check.py | 0 | 1 | 旧八枚举检查由P4入口复用，保留未迁移失败 |
| python -B tests/phase5_static_check.py | 0 | 1 | 旧event/aggregation整文件保护遇合法T1修改；首次报event.cpp，末次报service_aggregation.cpp |
| 下附T1专项audit，python -B - | — | 0 | Allowed Files、146 frozen files、SM精确逆向还原、旧注册前缀、DSM/fixture/新断言与UTF-8边界结构 |
| git -c core.safecrlf=false diff --check | — | 0 | tracked修改空白/冲突；新源码/fixture另由专项audit覆盖 |

最终旧checker的实际错误：

```text
P3: FAIL: required internal event kinds differ
    FAIL: Phase3 aggregation
P4: FAIL (static): required internal event kinds differ
P5: FAIL (P5 static): P5 protected file changed: src/runtime/event.cpp
    末次：FAIL (P5 static): P5 protected file changed: src/runtime/service_aggregation.cpp
```

P5 protected使用set迭代，首先报告的文件可随Python hash顺序变化；两项都保留为需T4迁移的合法T1冻结冲突。

先前将critical_resource与critical_component写在同一个谓词时，还触发P3/P4旧critical_resource文本oracle。
改为保留原critical_resource独立分支再检查component后，该冲突已消除；没有修改checker。
专项audit第一次只识别直接require，漏认expect wrapper而退出1；确认wrapper包含
states.query().current==target断言后，仅修正audit识别，未弱化测试。

T4仍须按CHANGE_LIST记录并迁移：八枚举→原八保留加唯一事实；event/aggregation整文件freeze→指定新增
组件机制及原resource/terminal保护；SM全文还原→只额外开放本heartbeat谓词；新13/14注册→保留原12/13。
没有删除PROTECTED、旧oracle或旧check。phase6_static_check.py是T4 Allowed File，本次不创建，
也不声称P6全静态PASS。

专项audit最终输出：

```text
PASS (T1 source audit): 46 C++ files; 176 main-wired tests; 13 targets / 14 registrations; 17 new assertion-bearing tests
PASS (T1 source audit): 146 non-T1 tracked files preserved; SM restored exactly by removing only heartbeat instance predicate
PASS (T1 source audit): original eight facts + one component fact; original 24 DSM edges and 441 declaration cases retained
PASS (T1 fixture audit): strict request shapes, semantic reason mapping, canonical numeric examples, nine ordered scenarios and payload bounds
fixture SHA256: 9c3a7d1543d259a281eb8003f80c7fe09254e029719194b6453c00629dac3680
LIMIT: static source/schema checks only; no behavioral verification
```

## Not Verified

静态结构与人工路径审查不证明身份/断连/队列/ACK时序、真实进程与socket行为、性能或RK3588/MCU电气结果。
真实SO_PEERCRED、IPC/配置/main接线与严格parser留T3；owner投影/managed client留T2；
联合验收、旧checker迁移和新的phase6 checker留T4。源码存在不表示这些行为已通过。

## 可复现T1静态audit

在仓库根把以下正文保存到PowerShell here-string `$p6Audit`，执行
`$p6Audit | python -B -`。这只是本报告内的静态审查脚本，不替代或修改旧checker。

```python
import sys, json, re, subprocess, hashlib
from pathlib import Path
sys.path.insert(0, 'tests')
from phase2_static_check import check_source, check_registration, masked_source
from phase3_state_static_check import enum_members
from phase3_aggregation_static_check import check_aggregate_contract
from phase3_validation_static_check import check_explicit_target_coverage
from phase5_static_check import function
root = Path.cwd()
base = 'f8507027c87a9ae9899f51da708f8d469552c473'
def read(p): return (root / p).read_text(encoding='utf-8-sig')
def old(p): return subprocess.check_output(['git', 'show', base + ':' + p]).decode('utf-8-sig').replace('\r\n', '\n')
allowed = {'include/runtime/event.hpp','src/runtime/event.cpp','include/runtime/service_aggregation.hpp','src/runtime/service_aggregation.cpp','include/runtime/runtime_manager.hpp','src/runtime/runtime_manager.cpp','src/service/service_manager.cpp','tests/CMakeLists.txt'}
changed = set(subprocess.check_output(['git','-c','core.safecrlf=false','diff','--name-only']).decode().splitlines())
assert changed <= allowed, changed - allowed
tracked = subprocess.check_output(['git','ls-files']).decode().splitlines()
frozen = [p for p in tracked if (p.startswith(('include/','src/','tests/','docs/P0/','docs/P1/','docs/P2/','docs/P3/','docs/P4/','docs/P5/')) or p == 'CMakeLists.txt') and p not in allowed]
for p in frozen: assert read(p) == old(p), 'unexpected frozen change: ' + p
predicate = '                (!event.instance_generation || *event.instance_generation == service.status.launched_generation) &&\n'
assert read('src/service/service_manager.cpp').replace(predicate, '', 1) == old('src/service/service_manager.cpp')
assert read('tests/CMakeLists.txt').startswith(old('tests/CMakeLists.txt'))
files = sorted(p for d in ('include','src','tools','tests') for p in (root/d).rglob('*') if p.suffix in ('.hpp','.cpp'))
counts = sum(check_source(p) for p in files)
assert check_registration() == (13,14)
assert check_aggregate_contract() == (9,24)
check_explicit_target_coverage()
assert enum_members(read('include/runtime/event.hpp'), 'RuntimeEventType') == enum_members(old('include/runtime/event.hpp'), 'RuntimeEventType') | {'component_health_changed'}
assert 'std::optional<ComponentHealthFact> component_health;' in read('include/runtime/event.hpp')
tests = read('tests/component_health_tests.cpp')
functions = re.findall(r'\bvoid\s+(test_\w+)\s*\(', masked_source(tests))
assert len(functions) == 17
assert 'states.query().current == target' in tests
for name in functions:
    body = function(tests, r'\bvoid\s+' + name + r'\([^)]*\)')
    assert 'require(' in body or '.expect(' in body, name
    assert re.search(r'\b' + name + r'\s*\(', tests[tests.index('int main()'):]), name
fixture_path = 'docs/P6/fixtures/health_contract_v1.json'
data = json.loads(read(fixture_path))
assert data['version'] == 1 and data['frozen_by'] == 'P6-T1'
assert data['transport']['health_payload_max'] == 4096 and data['transport']['private_operations_max'] == 32
messages = data['messages']
for name in ('bind_request','issue_request','current_request'):
    message = messages[name]
    schema = data['schema']['$defs']['bind_request' if message['op'] == 'bind' else 'report_request']
    assert set(message) == set(schema['required']) == set(schema['properties']), name
    assert type(message['version']) is int and message['version'] == 1
    assert len(json.dumps(message,ensure_ascii=False).encode('utf-8')) <= 4096
    if message['op'] == 'report':
        assert message['status'] in data['semantic_validation'][message['reason_code']]
        for field in ('session_id','report_seq','projection_revision','link_epoch'):
            value = message[field]
            assert re.fullmatch(r'(0|[1-9][0-9]*)',value) and int(value) <= 2**64-1
        assert len(message['reason'].encode('utf-8')) <= 256
        assert type(message['peer_boot_id']) is int and 0 <= message['peer_boot_id'] <= 2**32-1
assert messages['issue_request']['projection_revision'] == messages['current_request']['projection_revision']
assert messages['issue_request']['peer_boot_id'] == 0
assert len({s['id'] for s in data['sequences']}) == len(data['sequences']) == 9
for value in data['numeric_fixtures']['accepted_positive_u64']:
    assert re.fullmatch(r'[1-9][0-9]*',value) and int(value) <= 2**64-1
for value in data['numeric_fixtures']['rejected_positive_u64']:
    assert not re.fullmatch(r'[1-9][0-9]*',value) or int(value) > 2**64-1
for p in ['include/runtime/health_policy.hpp','include/runtime/component_health.hpp','tests/component_health_tests.cpp',fixture_path] + sorted(allowed):
    source = read(p)
    assert source.endswith('\n') and all(line == line.rstrip() for line in source.splitlines()), p
new_source = '\n'.join(read(p) for p in allowed if p.endswith(('.hpp','.cpp')))
assert not re.search(r'#\s*include.*(?:uart|serial)|\b(?:tcsetattr|TIOCSRS485|UartLink|SerialPort)\b|/dev/tty',new_source,re.I)
print('PASS (T1 source audit):',len(files),'C++ files;',counts,'main-wired tests; 13 targets / 14 registrations; 17 new assertion-bearing tests')
print('PASS (T1 source audit):',len(frozen),'non-T1 tracked files preserved; SM restored exactly by removing only heartbeat instance predicate')
print('PASS (T1 source audit): original eight facts + one component fact; original 24 DSM edges and 441 declaration cases retained')
print('PASS (T1 fixture audit): strict request shapes, semantic reason mapping, canonical numeric examples, nine ordered scenarios and payload bounds')
print('fixture SHA256:',hashlib.sha256((root/fixture_path).read_bytes()).hexdigest())
print('LIMIT: static source/schema checks only; no behavioral verification')
```

