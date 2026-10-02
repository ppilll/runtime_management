# Risk Analysis / Technical Debt

| 风险 | 等级 / 仓库依据 | 缓解与测试 | 决定 |
| --- | --- | --- | --- |
| 抽出RM后旧SM/Runtime/IPC owner残留 | 高，S02/S04/S05/S10/S11 | 同一集成gate删除三处；callsite review；P4-13/16 | Phase4必须处理 |
| fault token与launch token不同、SERVICE_STARTED提前清fault | 高，S03/S08 | captured bridge、candidate gate、P4-04/18 | Phase4必须处理 |
| PID复用/旧Monitor watch fact | 高，SM仅PID/时间现有校验 | instance generation tail，P4-19 | Phase4局部加强 |
| shutdown/recovery重启交错 | 高，S07已有cleanup | cancelAll先于stop、no launch after acceptance；P4-09/24 | Phase4必须处理 |
| synchronous start/rollback waitpid阻塞writer | 高，S12/S13 | 剩余startup cap、返回后deadline优先、一次launch/turn；P4-21 | 接受非硬实时边界；async后端另阶段 |
| 内核D-state无法SIGKILL/reap | 高，Linux进程语义 | 不伪清PID、不launch替代、显式terminal与shutdown错误 | 无通用用户空间修复；不得宣称硬实时 |
| lifetime五次预算在长期运行耗尽 | 中，P2冻结语义 | 保留无reset、真实诊断、manual动作不重置；P4-14/15 | 有意兼容；稳定窗口/reset另审 |
| 依赖方不自动revive造成设备仍ERROR | 中，P2已有规则 | 文档明确explicit START与critical stopped；P4-08/12 | 有意最小refactor；自动组恢复延后 |
| 主EventQueue/Logger无全局容量 | 中，P3明确限制 | 每service coalesce、active/last结果有界；每turndeadline检查 | 通用背压延后，不能承诺负载下延迟 |
| IPC并非atomic整包snapshot | 中，P3契约 | 保留单模块同步，查询只读，P4-26 | 全局snapshot revision延后 |
| 观察者throw/reentrant修改破坏dispatch | 中，P3契约 | writer-local work、禁止重入、现有错误cleanup；P4-21 | 不新增异常隔离框架 |
| 从内部post绕过RM结果gate | 高，P3开放producer入口 | bind active/receipt、拒绝future/missing/device-wide bypass；P4-32 | 内部API收紧必要，不改wire |
| 心跳socket仅service name标识、旧连接可能继续发送 | 中，IPC现有可信本机边界 | 保留receipt/state/time校验；Monitor延迟facts有instance token | session认证/peer credential绑定另审，P4不声称解决恶意或跨代旧连接注入 |
| mcu_service默认LOW不是业务想象的critical | 中，S09 | 部署显式AggregationOptions；不随意改默认 | 部署配置待具体设备确认 |
| 历史静态通过被当运行通过 | 高，P3 reports | 四状态报告；host Linux实际执行；P4-01..12 | 验收必须处理 |
| 多个implementation chat修改同文件 | 中，CHANGE_LIST | T1→T2→T3→T4严格串行，review可只读 | 禁止共享文件并发修改 |

## Shutdown Reliability Debt判定
旧fixed4s缺陷影响恢复可靠性，但当前基线已修复，因此不新增prerequisite代码fix。当前前提是维护max configured deadline+5s、完成reap/PID清除及失败报错，并真实执行独立六秒regression。若实现迁移使回归失败，这是进入下一gate的blocking implementation issue，不是把问题静默defer。

## Phase5/其它延后
实际CPU/Memory/Temperature/procfs采集、GPU/NPU、业务health、memory critical引起特定service动作、稳定窗口预算reset、自动dependent revive/group恢复、async process backend、全局背压/事务快照、IPC peer/session认证、runtime restart、device reboot、持久history均延后。
资源severity/阈值输入与source锁存已经存在，不能在Deferred表写“critical memory event字段尚缺”。
Level3/4不提供自动执行函数占位；未来需要独立审批边界/动作owner，当前RM遇到L1耗尽只terminal，不能调用system/reboot或自行退出Runtime当作恢复。

## 当前证据边界
本次静态阅读与设计产出，不运行Linux后端。无新C++源码，无Linux测试日志，无硬件结论。架构阻塞为空只允许推荐进入实现，不允许声称软件已满足上述风险缓解。
