# Phase1 架构审查

| 问题 | 风险 | 建议 |
| --- | --- | --- |
| 生命周期由 `runtime_manager` 和 `service_manager` 同时声明管理；`monitor` 输出健康状态，`service_manager` 又接收心跳事件，但未指定状态机的唯一写入者及事件流向（`module_design.md` 第 14–16、44–65、126–146 行）。 | 进程退出、心跳超时和人工停止并发时，可能重复或冲突地更新状态，状态查询结果不稳定。 | 明确 `service_manager`、`monitor`、主事件循环各自负责的状态和事件；规定状态迁移由一个模块串行执行。 |
| `service_manager` 依赖 `process manager`，但模块清单与 Thread 2 交付物均没有该模块（`module_design.md` 第 62–65 行；`thread_plan.md` 第 107–123 行）。 | fake_service 的创建、退出回收和异常退出上报无人负责，生命周期管理无法完整实现。 | 在现有模块分工中明确进程操作的归属，以及启动、停止、退出事件如何送达 `service_manager`。 |
| `config_manager` 已列为模块，但不在 `runtime_manager` 的依赖清单或 Thread 2/3 的交付与修改范围内（`module_design.md` 第 30–35、100–120 行；`thread_plan.md` 第 107–123、156–167 行）。 | “config 加载”是验收项，却可能没有实施负责人；服务启动所需配置的来源和校验时点不明确。 | 指定配置加载、校验和向 `service_manager` 传递配置的负责人，并在实施分工中覆盖该模块。 |
| Phase1 进程模型只列 `runtime_manager` 与 `fake_service`，IPC 却定义了 `START`、`STOP`、`QUERY_STATUS` 的请求/响应，未指定请求方、连接角色或 fake_service 是否承担控制客户端职责（`phase1_architecture.md` 第 99–111 行；`ipc_protocol.md` 第 18–39、69–139 行）。 | 各实现可能对同一消息的方向作不同假设；状态查询验收项也缺少明确的调用路径。 | 标明每类消息的发送方、接收方和 Phase1 查询入口，并说明它们是否复用同一个 socket。 |
| IPC 定义 Runtime→Service 的 `Event`，又把 `EVENT` 描述为 “Internal notification”；模块间还使用 Event Queue，三者的边界未区分（`ipc_protocol.md` 第 33–39、159–168 行；`phase1_architecture.md` 第 127–146 行）。 | 内部状态事件可能被误当作对外 IPC 消息，或漏发需要跨进程传递的通知。 | 区分进程内事件队列事件与 socket 协议消息；逐项标注 `EVENT` 的作用域和接收方。 |
| `ipc_manager` 同时依赖 `epoll` 与 event loop，而线程模型另外列出 IPC Handler、Monitor、Timer 线程，仅写明使用 Event Queue，未规定 socket、定时器和队列由谁驱动（`module_design.md` 第 71–94 行；`phase1_architecture.md` 第 127–146 行）。 | 多线程可能同时处理同一连接或定时事件，产生竞态；实现者也可能重复建立事件循环。 | 明确各线程的输入输出、队列生产者/消费者，以及 socket 和定时器的唯一处理线程。 |
| IPC 只列头字段和 JSON 载荷，未定义头字段编码、字节序、长度单位、消息边界、消息类型取值及响应与请求的关联规则（`ipc_protocol.md` 第 45–61、66–168 行）。 | Unix stream socket 上的粘包、拆包和跨进程解析可能不兼容；Thread 3 又被禁止更改协议，实施阶段难以消除分歧。 | 在 IPC 实施前固定最小可互操作的帧格式、类型取值、错误响应和 `request_id` 规则。 |
| 心跳规则同时使用 5 秒发送间隔、15 秒超时和“连续 3 次失败”，却未定义一次失败的判定、启动宽限期、迟到心跳及异常退出的优先级；配置中另有 `heartbeat_timeout`（`phase1_architecture.md` 第 175–195、260–276 行）。 | 不同模块可能在不同时间判定 `FAILED`，或在服务尚未启动完成时误报故障。 | 给出单一的失效判定、计时起点与配置覆盖关系，并规定异常退出时的处理顺序。 |
| 生命周期包含 `STARTING`、`STOPPING`、`FAILED`、`RECOVERING`，配置包含 `startup_timeout`，但未定义启动成功、停止完成和进程异常退出触发的状态迁移（`phase1_architecture.md` 第 151–169、260–276 行）。 | 进程已退出而状态仍为 `RUNNING`，或启动超时后进入无法恢复的中间状态。 | 列出 Phase1 必需的状态迁移触发条件、超时结果和进程退出处理；与状态查询字段保持一致。 |
| 重启上限与延迟序列已给定，同时写明“Phase1 只实现框架”；配置仍包含 `restart_policy`，但未界定框架与自动重启执行的界线，验收项也未覆盖重启（`phase1_architecture.md` 第 200–223、260–291 行）。 | Thread 2 可能把完整自动恢复和策略执行纳入 Phase1，造成范围扩张；也可能只保留空接口而无法验证框架。 | 明确 Phase1 对重启策略的最低交付行为与验收方式，并将完整策略执行标记为后续阶段（如确属后续范围）。 |
| Thread 3 要交付 `fake_service`，但允许修改的位置只有 `src/ipc` 和 `tests/`；Thread 2 的 `service_manager` 又依赖 Thread 3 才提供的 `ipc_manager`（`thread_plan.md` 第 107–123、139–172 行；`module_design.md` 第 62–65 行）。 | fake_service 的实现位置不明，线程间接口可能需要返工或越过各自修改范围。 | 补足 fake_service 的归属路径，并在 Thread 2/3 开工前约定 IPC 与生命周期的接口及集成顺序。 |
| 架构配置包含 `dependency`，但 Phase1 进程只要求一个 fake_service，未说明依赖字段在 Phase1 是否需要调度、循环检查或仅解析；未来服务列表已标为 `Future`（`phase1_architecture.md` 第 99–122、260–291 行）。 | 实施者可能提前实现多服务依赖编排，超出最小 Supervisor Core 和当前验收范围。 | 明确 `dependency` 在 Phase1 的处理边界；若仅保留字段，应避免把多服务编排计入本阶段交付。 |
