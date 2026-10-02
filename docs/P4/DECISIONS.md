# Architecture Decisions
以下均为设计冻结建议，依据 REPOSITORY_ASSESSMENT 的当前代码事实。速度/稳定/维护/并发/ARM64/测试/扩展/展示价值按项目规模判断，不冒充基准测试。

## ADR-01：Recovery Policy Single Owner
方案A：SM 保留 retry/backoff，RM 协调结果。
优点：代码移动少、早期开发快、已有 fixture 少变。
缺点：RM deadline/取消/终结与 SM timer 各有 active 状态，容易重复 launch/terminal；manual IPC 第三处协调仍需移走；新策略必须穿过 SM。

方案B：RM 拥有 retry/backoff/deadline/terminal，SM 只执行生命周期。
优点：请求—执行—结果有同一记录；可独立用假时钟验证；符合 P0/P2 文档职责。
缺点：需要移除 SM.restart_at/fail/tick 策略分支、Runtime 推导结果和 IPC manual poll；旧 fixture 要接 RM。

| 维度 | A | B |
| --- | --- | --- |
| 开发速度 | 初期最快，集成取消/期限仍需额外协调 | 中等，局部提取即可 |
| 稳定性 | 两个 active state 容易漂移 | 单一 episode 不变量更可审查 |
| 维护成本 | SM 混合执行与策略 | 策略集中、SM 限界明确 |
| 并发风险 | SM timer 与 RM cancel/result 交叉 | 同 writer 上一次决策 |
| ARM64适配 | 原平台可用 | 只加 C++17 内存对象，同样可用 |
| 测试难度 | 必须模拟跨 owner 状态 | RM fake clock + fake executor 可隔离 |
| 后续扩展 | deadline/监控来源继续侵入 SM | 新 failure type 走统一 ingress |
| 面试展示价值 | 能解释有限重构但 owner 不鲜明 | 能清晰展示 ownership、generation 和确定性 |

推荐B。改动必须在同一集成 gate 完成切换，不保留 fallback 自动 retry loop。restart_count 可由 SM 存储以保持 public status，值只接受 RM 发出的 reservation projection；SM 不能决定是否加一。

## ADR-02：线程
A：独立 recovery worker。优点：可避开 writer 上同步等待；缺点：多线程结果关联、SM 线程调用和 shutdown join 更复杂；依赖/状态仍需串行。
B：复用 writer/tick。优点：实现快、少锁、ARM64内存开销小、假时钟容易；缺点：同步 exec/内核阻塞可延迟 timeout。
推荐B，接受 ARCHITECTURE 中非硬实时期限，并用剩余预算约束 launch。若部署以后要求严格响应期限，应另审异步进程后端，不能偷偷增加 worker。

## ADR-03：关联模型
A：timestamp 或单一 lifecycle generation。优点：字段少；缺点：timestamp 不能区分 episode，fault token 会在 replacement STARTING 改变，单 token 不能关联完整恢复。
B：原 lifecycle generation + per-service recovery_generation + execution generation binding。优点：复用既有 token，精确区分旧 attempt/result；缺点：需显式请求/launch bridge。
推荐B，不再增加随机 UUID/request_id；请求 ID 就是(service_name,recovery_generation)。不以完成时查询的新 generation 为旧结果“补标签”。

## ADR-04：退避与reset
A：固定2s或线性2/4/6/8/10。优点：易算、低延迟；缺点：持续失败时高频 fork，改变 P2 测试期望。
B：既有确定性指数2/4/8/16/32，ceiling60，累计最多5。优点：兼容、限制 storm、测试已有基础；缺点：成功不 reset 会使长期设备耗尽，运维需有意处理。
推荐B。reset 仅重建 Runtime/静态注册表；不新增稳定窗口字段。手动 restart 不花费/重置自动预算。成功即 exec完成而非业务健康；仍由 Monitor 发现后续故障。

| 延迟候选 | 五次序列 / 总延迟 | 稳定性与测试判断 |
| --- | --- | --- |
| 固定 | 2/2/2/2/2；10s | 最快但持续fault时launch最密集；边界简单，改变旧行为 |
| 线性 | 2/4/6/8/10；30s | 中间频率，仍需新期望；没有仓库需求支持改变 |
| 指数 | 2/4/8/16/32；63s | 延后持续fault的fork负载，完全保留P2；推荐 |

reset的独立取舍：方案A在SUCCESS后reset，优点是长期运行可持续恢复，缺点是反复exec成功后立刻crash可无限重启；方案B保留累计预算，优点是P2兼容且总fork数有界，缺点是长期可能耗尽。推荐B；未来稳定窗口需新需求和测试，不隐含纳入本阶段。

## ADR-05：状态和结果
A：IDLE/PENDING/EXECUTING/WAITING/SUCCEEDED/FAILED 全部长期状态。优点：命名详细；缺点：同步 launch 的 EXECUTING/WAITING 区分价值很小，终结状态形成额外状态机。
B：无 active 即 idle；active仅 BACKOFF/EXECUTING；终结保存单个 Result。优点：状态少、可用 deadline+PID表达等待；缺点：诊断需读取字段。
推荐B。保留 SUCCESS/FAILED/TIMEOUT/CANCELLED 四个内部 outcome；TIMEOUT 单独测试，CANCELLED 不误判 OFFLINE。只三种既有 recovery event，TIMEOUT映射 RECOVERY_FAILED，cancel不加事件。

## ADR-06：依赖范围/升级
A：故障后自动恢复整个依赖组、自动 revive 所有 dependent。优点：可能恢复整机可用性；缺点：改变 P2 契约、重启 storm、双故障归属和预算复杂。
B：沿用静态图、逆序停止 transitive dependent、只恢复故障节点；explicit START 恢复被停止闭包。优点：最小改动、边界可测；缺点：B恢复后A仍STOPPED，设备可保持ERROR。
推荐B。L0仅再确认快照，无新动作；L1 service/process restart 实现；L2只既有依赖停止/显式启动，不新增自动组restart；L3 runtime restart/L4 reboot只保留未来边界，无实现/默认动作。

## ADR-07：timeout
A：每次attempt重置deadline。优点：小deadline易配置；缺点：可以一直BACKOFF/依赖等待，无法限制episode总时长。
B：admission时创建episode绝对deadline，包含backoff、reap、dependency wait和launch；每个checkpoint先到期后执行。优点：不会逻辑永久RECOVERING；缺点：deadline可能在同步后端期间迟到。
推荐B。默认总预算由合法静态service timeouts推导；可配置更短以提前终结。内核卡死不是新增线程可完整解决的问题，禁止硬实时承诺。

## ADR-08：配置
A：增加max_restart_attempts/restart_delay/max_restart_delay/recovery_timeout以及reset窗口。优点：灵活；缺点：矩阵膨胀、易破坏P0五次约束。
B：冻结max=5、base=2s、cap=60s，只增加optional recovery_timeout。优点：校验简单、现有JSON兼容、少文档/测试分支；缺点：无法部署调retry形状。
推荐B。未知JSON字段既有解析行为不顺带改变；明确上述三个名字不是支持的可配置策略字段。

## ADR-09：IPC
A：GET_RECOVERY_STATUS和新的恢复事件订阅。优点：客户端诊断详尽；缺点：当前无真实调用方，P3只订阅DEVICE_STATE_CHANGED，无必要扩协议。
B：现有GET_HEALTH/QUERY_STATUS/GET_DEVICE_STATE/DEVICE_STATE_CHANGED加进程日志，内部保留Result。优点：帧/类型/客户端兼容、开发/测试快；缺点：外部看不到每次retry细节。
推荐B；不把内部recovery events描述为已经在socket广播。只迁移RESTART_SERVICE内部路由，保留类型6及ACK语义。

## ADR-10：历史shutdown债务
A：按旧审查重新实现固定预算修复。优点：若旧代码存在可解决；缺点：当前基线已修复，重复改动风险高。
B：接受当前max deadline+5s实现，强制回归并在recovery迁移中取消全部work。优点：保留已解决问题、低维护；缺点：真实Linux验证仍欠缺。
推荐B。它是已实现但运行未验证的可靠性前提，不是新的设计阻塞；若回归失败必须先修再进入最终验收。
