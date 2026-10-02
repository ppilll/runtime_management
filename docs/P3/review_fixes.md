# Phase3 审查问题修复记录

日期：2026-10-02（Asia/Shanghai）。用户已授权修复 `validation_review.md` 中的问题。
沿用 Runtime Manager 监督职责、Unix Domain Socket、原有构建/测试目标；未引入业务
实现、硬件采集、外部消息系统、数据库或新依赖。保持已有未提交修改，不提交代码。

## 修复结果

| 原问题 | 当前实现 | 回归测试源码 |
| --- | --- | --- |
| V01：内存 95% 无法表达 ERROR | ResourceSeverity 区分 warning/critical；资源按来源保存严重程度；静态阈值默认为 CPU 80%、内存 80%/95%；支持降级/清除，服务恢复不能清除资源故障 | `test_resource_monitor_thresholds`、`test_runtime_resource_fact_adapter`、`test_resource_critical_downgrade_and_clear` |
| V02：迟到恢复成功覆盖新故障 | ServiceManager 发放单调 generation，开始/故障/停止更新；具名事实必须带 token，恢复结果必须匹配当前故障；缺失、旧/未来 token 拒绝，重复结果幂等 | `test_stale_recovery_generation_rejected`、`test_terminal_failure_metadata_and_generations` |
| V03：重试耗尽没有失败结果 | 最终 FAILED 回调带 recovery_exhausted；runtime 发当前失败事实后发布匹配 token 的 RECOVERY_FAILED；never/首次失败不升级终结结果，HIGH/心跳恢复失败 OFFLINE，可选普通失败 WARNING | `test_lifecycle_retry_exhaustion_reaches_device_state`、终结元数据测试、已有心跳恢复失败测试 |
| V04：固定四秒截断关机 | 查询全部未完成停止的最大配置期限，再加五秒强杀/回收预算；只有活动 PID 全部清除才正常返回；预算耗尽显式抛错 | `test_shutdown_deadline_uses_all_service_grace_periods`、保留原六秒独立 shutdown regression 的全部断言 |

其他审查建议的处理：

- READY 文档统一为 runtime 已初始化、等待必需服务；RUNNING 表示生命周期就绪，允许首心跳宽限。
  GET_HEALTH 的 RUNNING 标签必须由返回行确认：存在不健康服务为 UNHEALTHY，存在未知服务或空列表为 UNKNOWN，
  只有全部配置服务非空且 HEALTHY 才确认 HEALTHY。包括可选未启动服务时会保守返回 UNKNOWN。
  `test_running_health_requires_confirmed_heartbeats` 覆盖首心跳、有效心跳、过期及查询不改设备状态。
- STATE_TRANSITION.md 显式交叉引用聚合扩展规则，仍保留九条无目标边和十五条目标扩展边，
  没有引入新状态或放宽任意目标设置。441 项目标矩阵保留。
- Runtime 初始化或事件处理抛错时，先停止线程、关闭 timer、尝试停止/回收服务并恢复 signal handler，
  再传播原始错误。订阅者“不得抛异常”的约束仍有效，未把异常吞掉。
  `test_runtime_observer_exception_cleans_up` 覆盖错误传播、服务清理和信号恢复。
- 新增可控 snapshot gate 的 IPC 测试：生产者条数/字节溢出断开受影响订阅者，重新订阅后继续接收；
  未读响应突发超过每客户端输出上限时断开该客户端，其他连接仍可查询。
  测试解锁 guard 保证失败时不会把 IPC 析构卡在 gate。
- STOPPING 即作为服务不可用事实发布，与 STOPPED 共享停止代次；停止请求取消旧恢复 work，
  不必等待进程回收才更新关键健康。

## 接口与兼容性

`RuntimeEvent` 尾部新增 severity 和 optional generation，`ServiceStatus` 与
`ServiceStateChange` 尾部新增内部代次/终结元数据。既有 Event 字段顺序、服务生命周期
枚举、五次重试计数、2/4/8/16/32 秒退避及 Phase2 配置 JSON 未改变。
运行事实的具名生产者需要提供 token；这是用于阻止过期结果的内部 API 契约收紧，
时间戳仍作为元数据保留，FIFO 不按 timestamp 排序。

恢复生产者在发起工作时捕获 `query(name)->generation`，并在结果中回送这个值；
不能在完成时查询新值来冒充当前故障的恢复结果。具名服务开始/故障/停止由生命周期
所有者发新代次；未来恢复协调器不得自行发放代次。device-wide 成功只重新计算既有
服务事实，不能清除服务故障。device-wide 终结结果仍是可信协调器的显式声明。

资源接口示例（未在此次执行）：

```cpp
runtime::RuntimeManager manager(config_path, {}, {}, runtime::ResourceThresholds{80, 80, 95});
manager.reportResourceUsage(cpu_percent, memory_percent);
```

没有自动增加系统资源采集线程；部署侧需要提供测量值。Monitor 只发布事实，不写设备状态。
旧 IpcManager 构造默认参数、命令类型 1–7/255、新增类型 8–10、帧尺寸和字段均保持兼容；
GET_HEALTH 的既有状态标签变得更保守，不增加新的协议字段或标签。

正常关机保存最后设备快照的原约定保留，不人为映射到 OFFLINE。超过强杀/回收预算
会返回失败而不是假装所有服务已停止；不可回收进程的 PID 保留用于诊断/后续析构。
如果主循环和清理同时失败，优先传播主循环错误，进程状态仍可通过服务 query 检查。

## 验证

根据用户隔离环境限制，仅执行：

```text
python -B tests/phase3_validation_static_check.py
git diff --check
```

静态入口包含四个既有检查器及新增修复结构检查：资源等级/阈值与来源锁存、生命周期
代次捕获及结果匹配、自动重试终结元数据与 adapter、最大关机期限与错误传播、保守
健康投影，以及十一项新增测试的 main 接线。修改的生产文件、文档、新文件均扫描
冲突标记、尾随空白和文件结尾；所有 tests/*.py 用 Python AST 解析。

结果：全部静态检查通过；33 个 C++ 文件、72 个 test_* 函数有 main 调用、
8 个测试目标和 9 项 CTest 注册保留；24 项允许/417 项拒绝的目标声明矩阵与设计一致。
`git diff --check` 通过，仅有仓库 LF→CRLF 行尾提示。

没有调用编译器、交叉编译器、CMake、CTest、C++ 程序、服务进程或 socket 实验。
上述十一项新增函数及已有六秒关机回归是测试源码，未执行，不能据静态结果宣称
C++ 类型/链接、Linux 时序、信号/进程回收、实际 IPC 流量或并发行为已经验证。

## 保留的限制

- GET_HEALTH 各模块仍分别取快照，未新增全局事务或原子整包快照；这是已文档化接口限制。
- 运行主队列和日志队列仍沿用既有容量策略，未在本修复中引入通用背压框架。
  IPC 两类有界队列已有行为与新回归测试源码；持续生产者压力的性能需要目标环境测量。
- 普通观察者需遵守单写者、不可重入修改和不抛异常契约；独立并发 handle 的锁外通知
  排序没有放宽。新的 Runtime 退出清理使异常路径更明确。
- 六秒真实关机、真实崩溃/超时到 IPC 的端到端链路仍待 Linux 环境运行；没有弱化原测试断言。

## 修改范围

生产实现：event、service_manager、service_aggregation、monitor、runtime_manager 的
头文件/实现，以及 IPC 健康投影。测试：event_aggregation、runtime_core、
recovery_dependency、device_ipc 及旧关机回归注释。静态脚本：聚合检查器和统一检查入口。
文档：事件、聚合、资源、设备语义、状态表、IPC 契约，以及原审查报告的历史状态说明。
没有修改构建文件或进程后端的实现。
