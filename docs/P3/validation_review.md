# Phase3 Thread4 静态验证与架构审查报告

> 本文件保留首次审查记录。用户随后授权修复，V01–V04 及健康语义、状态表交叉引用、
> 异常清理和 IPC 溢出测试已处理；当前实现和检查结果见 [review_fixes.md](review_fixes.md)。
> 下文旧计数和“与 HEAD 无差异”仅对应审查时点，不适用于修复后的工作区。

审查日期：2026-10-02（Asia/Shanghai）。依据 `codex/thread4_test_prompt.md`，
已读取 `docs/P3/` 全部文档，包括三个实现报告及 Phase4 恢复接口约定。
审查对象是当前工作区中的 Thread1、Thread2、Thread3 实现，而非仅 Git HEAD。

## 结论与验证边界

**源结构、状态表声明、IPC 协议声明及测试注册静态检查通过；设计能力缺口和既有问题仍然存在。**
按用户要求，没有运行编译器、交叉编译器、CMake、CTest、C++ 测试程序、
服务进程或 socket 实验，也没有安装工具。没有实际执行失败的 C++ 用例可供统计；
下文问题属于源码推导、设计对照及集成风险，不冒充运行测试结果。

此次新增内容仅为测试源代码、静态检查入口和本报告。保留前三个线程已有的
未提交修改，不修改生产实现、服务业务或构建框架，不提交代码。

完成的静态命令：

```text
python -B tests/phase3_validation_static_check.py
git diff --check
git diff --exit-code -- src/service/service_manager.cpp src/service/process_supervisor.cpp src/monitor/monitor.cpp src/config/config_manager.cpp src/ipc/frame.cpp
```

第一个入口调用既有 Phase2、Phase3 state、aggregation、IPC 四个静态检查器，
再检查新增测试的目标矩阵、场景声明、Python 语法及本次产物的空白/冲突标记。
也可分别运行：

```text
python -B tests/phase2_static_check.py
python -B tests/phase3_state_static_check.py
python -B tests/phase3_aggregation_static_check.py
python -B tests/phase3_ipc_static_check.py
```

| 静态检查项目 | 结果及含义 |
| --- | --- |
| C++ 源结构 | 33 个 `.cpp`/`.hpp` 文件：本地 include、注释/字面量、括号和冲突标记检查通过；不是 C++ 语法/类型检查 |
| 测试注册 | 61 个 `test_*` 函数在相应 `main` 中有调用声明；8 个测试目标、9 项 CTest 注册保留，无失败屏蔽 |
| 原始状态表 | 7 个状态、9 条原始边；无目标的 63 种状态/事件组合声明为 9 项允许、54 项拒绝 |
| 聚合扩展表 | 15 条额外边与 `AGGREGATION_RULE.md` 一致；合计 24 条允许的状态/事件/目标三元组 |
| 新增目标矩阵 | 独立测试期望表与两份设计表一致；声明覆盖 441 个组合，其中 24 项允许、417 项拒绝；未执行 `handle()` |
| 表路径 | 原始检查器的 7 条路径、聚合检查器的 9 条路径均可在抽取的声明表中遍历 |
| IPC | 旧类型 1–7/255、10 字节帧头、64 KiB 上限保留；3 个新请求与文档相符，4 个 JSON 示例合法且计数一致 |
| Phase2 核心实现对照 | 服务生命周期、进程后端、Monitor、配置解析、帧编解码五个文件与 HEAD 无差异；不能据此断言运行时回归全部通过 |
| 空白与 Python | `git diff --check` 通过；新产物额外扫描通过，所有 `tests/*.py` 可由 Python AST 解析 |

Git 输出的 LF→CRLF 提示属于行尾规范化提示，不是检查失败。
入口返回 0 表示上述结构检查通过，不表示下列设计问题已解决。

## 五项任务用例

| 用例 | 源码证据与已准备测试 | 静态判断 / 未验证部分 |
| --- | --- | --- |
| Test1 正常启动 | `run()` 在处理队列前显式提交 `runtime_initialized`；聚合在必需服务运行后提交 `required_services_ready`。`test_normal_boot`、新增 `test_normal_startup_and_complete_critical_recovery` 检查 BOOTING→READY→RUNNING 及通知链 | 状态边和接线一致。真实自启动、exec、首个心跳及通知到达未验证；READY/健康语义差异见后文 |
| Test2 关键服务崩溃 | `ServiceManager::process_exit/fail` 的 FAILED 回调被转换为 SERVICE_FAILED；默认 control 为 HIGH，聚合得出 ERROR。`test_control_failure_case2` 及新增完整恢复链覆盖该事实 | RUNNING→ERROR 边成立。真实进程退出到 RuntimeManager 再到 IPC 的整条链未执行；启用自动重启时紧接着还会产生 RECOVERING，查询不保证停留在 ERROR |
| Test3 恢复流程 | RECOVERY_START→`health.recovering`→RECOVERING；具名 RECOVERY_SUCCESS 清除该服务故障，并重新计算所有服务。新增完整链验证 ERROR→RECOVERING→RUNNING；已有部分恢复测试验证落到 WARNING 时没有临时 RUNNING | 显式事件成功链一致。真实重启及耗尽结果集成未验证；耗尽事实缺口见 V03 |
| Test4 多服务聚合 | 默认 control HIGH/required、vision MEDIUM/optional、OTA LOW/optional；六种失败顺序、部分恢复、必需 vision、静态覆盖、资源来源分别有测试声明 | control 故障优先 ERROR；vision/OTA 普通故障 WARNING；单个恢复不能清掉其他服务问题。源码分支和表路径一致，未执行聚合算法 |
| Test5 心跳超时 | Monitor 发 health_missed；ServiceManager 校验状态、PID、时间和 missed_count 后才产生 FAILED，adapter 才发布 HEARTBEAT_TIMEOUT。新增三种默认服务的超时/恢复链测试 | 接线支持任何等级超时进入 ERROR，且故障标记不被 STOP 清掉。真实超时、旧心跳/旧 PID 拒绝及监控线程调度未执行 |

现有主要覆盖位置：`tests/device_state_manager_tests.cpp`、
`tests/event_aggregation_tests.cpp`、`tests/runtime_core_tests.cpp`、
`tests/device_ipc_tests.cpp`、Phase2 生命周期/恢复/IPC 集成测试。

新增的四个测试函数已挂到原有 `main`，沿用原有测试目标：

- `test_explicit_target_matrix`：遍历全部 7×9×7 组合，检查允许边的完整元数据及单次通知、
  拒绝边的快照不变及零通知；覆盖全部 15 条新增边和 OFFLINE 终止性。
- `test_invalid_explicit_target_and_metadata`：聚合目标路径的缺 source/reason、未知目标和未知事件拒绝。
- `test_normal_startup_and_complete_critical_recovery`：完整启动/关键故障/成功恢复链，检查每条通知的 previous/current。
- `test_heartbeat_timeout_and_successful_recovery`：control、vision、OTA 各自超时后的 ERROR→RECOVERING→RUNNING，
  以及重复恢复成功不额外通知。

这些是为后续 Linux 执行准备的测试，静态工具只核对声明、结构及契约期望。

## 失败场景、能力缺口与修复建议

### V01 / P2：资源事实不能表达内存 95% 时的 ERROR

分类：Phase3 设计能力缺口；不是观察到的 C++ 运行失败。

`MONITOR_INTEGRATION.md` 规定内存 80%→WARNING、95%→ERROR，且阈值可配置。
但 `RuntimeEvent`（`include/runtime/event.hpp:24`）仅有事件类型、来源及 active，
没有资源严重程度；聚合只将 RESOURCE_WARNING 来源存入集合
（`src/runtime/service_aggregation.cpp:31`），随后仅计算 `warning`
（同文件 `:93`、`:113`）。当前 Monitor 也仅实现心跳观察。

推导场景：所有服务正常，内存生产者将严重压力作为 RESOURCE_WARNING 发布；
当前接口只能导出 WARNING，没有承载 95%→ERROR 的资源事实路径。
直接发 `DeviceStateEvent::critical_failure` 也无法同步聚合内部资源记录，
之后重新计算可能把状态清掉，不宜用作替代方案。

建议：保持“Monitor 报事实、聚合/状态所有者决策”的边界，为资源事件增加显式严重程度，
按来源保留 warning/critical 状态，提供静态可配置阈值并明确清除/降级规则。
增加 80%、95%、从 critical 降级、清除以及服务恢复不能清除资源故障的用例。
这不要求把硬件采集或业务实现放进此次验证任务；Thread2 本身也禁止新增硬件监控，
但不能据现有实现宣称完整 `MONITOR_INTEGRATION.md` 已完成。

### V02 / P2：过期恢复成功可以覆盖后一次关键故障

分类：已有实现报告明确披露的架构风险；当前公共事实 API 可以产生该问题，
真实并发发生概率未验证。

`RuntimeEvent` 没有 PID/运行代次/恢复请求编号。`ServiceAggregation::handle`
（`src/runtime/service_aggregation.cpp:48`）无条件将具名 RECOVERY_SUCCESS
视为 running，并清除 failed、stopped、heartbeat_lost、recovering。
生产者时间作为元数据保留，既不排序也不校验新旧。

推导场景：control 的恢复请求 A 尚未反馈，又发生一次 control 故障 B；
延迟到达的 A 成功事件会清掉 B，在没有其他故障时导出 RUNNING。
当前同一运行线程的生命周期 adapter 有序，不是该风险的主要来源；
未来恢复协调器或外部 `post(RuntimeEvent)` 生产者需要额外防护。

建议：为服务运行及恢复请求增加 generation/token；事件携带 token，
只有匹配当前待恢复故障的结果才可清除记录。不要简单拒绝较小 timestamp，
那会破坏现有 FIFO/时间元数据契约，也不足以识别旧请求。
补充 A 故障→A 恢复→B 故障→A 迟到成功的回归用例。

### V03 / P2：Phase2 自动重试耗尽没有 RECOVERY_FAILED 生产者

分类：恢复结果集成缺口；Phase4 边界相关，不等同于 Phase2 生命周期回归。

`ServiceManager::fail`（`src/service/service_manager.cpp:267`）仅在重试次数小于 5 时
安排 RECOVERING；耗尽后保留 FAILED。RuntimeManager adapter
（`src/runtime/runtime_manager.cpp:79`）对任何 FAILED 只发布
SERVICE_FAILED 或 HEARTBEAT_TIMEOUT，没有发布 RECOVERY_FAILED。
因此实际自动重试全部失败时，关键服务设备状态停留在 ERROR；
OFFLINE 需要另外有生产者显式提交失败事实。

预期需要区分：首次故障、`restart_policy=never` 和重试耗尽不能混为一类。
原始 Phase2 的“第六次故障保持 FAILED”仍被保留；现有外部恢复失败事件
到 OFFLINE 的状态机接口也存在，Phase4 文档把恢复策略执行留给后续阶段。
当前不能宣称实际自动重试耗尽已接通 OFFLINE。

建议：明确 Phase2 自动重启与 Phase4 协调器谁拥有终结结果，扩展生命周期事实携带
失败类别/重试是否终结，再由唯一生产者发一次 RECOVERY_FAILED。
不要把所有 FAILED 都升级为 OFFLINE。补充关键重试耗尽、可选重试耗尽、
心跳恢复耗尽、never 策略及重复终结结果的跨模块测试。

### V04 / P1：既有固定四秒关机期限仍短于合法 shutdown_timeout

分类：Phase2 已报告、此次确认仍存在的源码支持缺陷；不是 Phase3 新引入的问题。

`src/runtime/runtime_manager.cpp:217` 仍采用固定四秒退出预算；
ServiceManager 在 `now + config.shutdown_timeout` 才安排强制停止
（`src/service/service_manager.cpp:245`、`:336`）。配置允许比四秒更长的期限。

现有独立回归 `phase2_runtime_shutdown_regression` 构造忽略 SIGTERM、
shutdown_timeout=6 秒的进程。源码推导 `run()` 可在四秒结束后返回，
进程尚为 STOPPING 且 PID 未清除；后续析构强杀不等同于正常完成关机。
本次没有执行该回归，也没有改变它的期望、禁用它或设置 WILL_FAIL。

建议：由所有活动服务的实际关机期限决定等待预算，强制停止后继续回收退出并完成
服务状态更新；明确超时失败结果。保留已有独立回归，覆盖多个不同期限及回收失败。
详细原报告见 `tests/phase2_review.md`。

## 其他架构与覆盖关注项

1. **READY 和健康语义需要统一。** `DEVICE_STATE_MODEL.md` 把 READY 描述为必需服务
   已启动，原始转换表却在 runtime 初始化完成时进入 READY；当前实现遵循后者。
   聚合以服务 exec 后进入 RUNNING 为就绪依据，不记录首个有效心跳；IPC 在首心跳
   前把该服务标为 UNKNOWN，却把设备 RUNNING 标为 HEALTHY。
   这是启动宽限期的稳定策略差异，不能仅归因于跨模块快照时差。
   应明确设备 HEALTHY 是否要求必需服务首心跳，再统一内部聚合和 IPC 文档/测试。
2. **表完整性不等于事实正确。** 原始九条边和十五条聚合扩展均为显式检查，
   没有任意目标 setter；但 `STATE_TRANSITION.md` 仍只列原始九条，读者必须结合
   `AGGREGATION_RULE.md` 才能获得完整契约。建议交叉引用或统一列出带目标/无目标的规则。
3. **单写者约束明确。** Runtime 队列有 mutex；dispatcher、aggregator 仅在运行线程修改；
   DeviceStateManager 的读写共享 mutex，通知在释放状态锁后调用。
   ServiceManager 回调也在注册表锁外调用，IPC socket/output 由 epoll 线程独占，
   通知生产者队列有 mutex，weak_ptr 防止 IPC 析构后悬空访问。
   这些结构没有暴露明显的锁循环；尚未通过实际线程压力测试证明。
   独立并发调用 DeviceStateManager::handle 会使锁外回调可能乱序，已有头文件对此作了约束。
4. **异常/背压契约需要后续落实。** 订阅回调要求不抛异常；dispatcher 异常会中止余下订阅者，
   设备状态已经提交也不会回滚。Runtime 主事件队列/日志队列无容量限制。
   后续恢复/资源生产者接入前应明确异常隔离、退出清理及输入限流，不改变业务边界。
5. **GET_HEALTH 非原子快照是已文档化限制。** 设备和各服务分别加锁读取，因果事件尚未排空时
   可以暂时不一致。若调用方需一致读，建议增加快照 revision 或由运行线程生成整包投影。
   当前 response 的服务计数来自同一次列表遍历，未发现声明与计数规则矛盾。
6. **IPC 溢出覆盖不足。** 源码包含 1024 条/字节数限制、每客户端输出上限和丢失序列断开逻辑；
   当前 IPC 测试主要覆盖超大单事件断开、突发顺序、重复订阅、连接关闭与 weak sink 生命周期。
   尚无专门用例在可控阻塞下触发正常大小事件的生产者队列溢出、慢客户端输出溢出和溢出后重订阅。
7. **跨模块执行缺口。** 单独状态、聚合及 socket 测试已有源码覆盖，Runtime 新集成案例主要是
   启动失败和直接设备事件。真实关键服务崩溃→自动重启→设备通知、验证后心跳超时→设备通知、
   多服务并发故障链及代次校验仍需专门的 Linux 集成测试。此次没有运行这些链路。

架构边界审查未发现摄像头、推理、MCU、驱动或业务代码进入新增模块；
没有数据库、MQTT、云 SDK、HTTP/TCP 服务或外部消息系统，Unix Domain Socket 和
既有 CMake 目标沿用，设备诊断快照仍只在内存中保存。

## 本次产物与后续顺序

- 修改 `tests/device_state_manager_tests.cpp`：目标矩阵及目标元数据/枚举拒绝测试。
- 修改 `tests/event_aggregation_tests.cpp`：完整关键恢复链、三种服务心跳恢复链。
- 新增 `tests/phase3_validation_static_check.py`：一个命令执行全部源检查并核对新增覆盖。
- 新增 `docs/P3/validation_review.md`：本报告、失败场景、架构关注项和建议修复。

优先处理既有 P1 关机期限缺陷；明确资源 critical 语义、恢复代次及终结事实责任，
然后补全相应测试。未来有 Linux 工具链时再执行已有注册测试及上述跨模块场景。
此次验证任务已按静态范围完成，不以编译/运行作为交付前置条件。
