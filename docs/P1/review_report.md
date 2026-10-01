# Phase1 最终 Review 报告

日期：2026-10-01  
范围：`docs/P1/` 全部现有文档、`include/`、`src/`、`tests/`、`tools/fake_service/` 和 CMake 配置。

## 结论

**静态审查发现的代码与文档问题已修复。** 模块边界、Service 状态所有权、IPC 方向和 Phase1 范围现已一致。Linux 实机编译与运行验证由用户后续执行，本报告不以本环境缺少构建工具作为缺陷。

## 五项检查

| 检查项 | 结果 | 依据 |
| --- | --- | --- |
| 模块职责一致性 | 静态核对符合 | `RuntimeManager` 负责配置、事件队列与分发；`ProcessSupervisor` 负责进程操作；`IpcManager` 负责 socket；Timer Thread 使用 `timerfd`，Monitor Thread 检查健康，Logger Thread 输出日志。 |
| `service_manager` 状态所有权 | 符合静态约束 | `ServiceState`、PID、最近心跳时间、重启计数的写入集中在 `src/service/service_manager.cpp`；IPC 和 Monitor 通过事件传递变化。 |
| IPC 协议实现一致 | 静态核对符合 | 10 字节小端帧头、长度为 Payload 字节数、类型值、`request_id` 回显、控制/服务通道方向均已核对；STOP 向已通过心跳标识的 Service 连接发送 `EVENT`。协议文档已补足这些约定。 |
| Phase 范围扩展 | 已收回 | 去掉依赖拓扑排序、跨服务引用/环检查及启动门控；`dependency` 在 Phase1 仅解析。删去与模块设计和验收项不一致的“配置保存”承诺。未新增模块或设备业务。 |
| 测试覆盖 | 关键路径已有用例，留待实机运行 | 单元测试覆盖配置、状态迁移、进程启动/退出、心跳、恢复上限、停止升级、启动失败状态、旧健康事件隔离和异步日志排空；集成测试覆盖 START、STOP、QUERY_STATUS、心跳失效、合包、半关闭、无效长度/残缺帧、`SERVICE_STOP` EVENT 以及拒绝 SIGTERM 的进程。 |

## 本次修正

1. 将 `phase1_ipc` 和 `fake_service` 的构建接入根 CMake，使交付的 `runtime_manager` 本身提供控制与服务 socket；删除不带 IPC 的旧入口。集成测试改用该主程序。
2. 删除超出 Phase1 范围的依赖图排序和启动门控，更新核心契约与单元测试。配置仍校验字段类型和重复 Service 名称。
3. 在服务连接首次有效心跳后记录其 Service 名称；收到 STOP 时，向对应连接发送 `EVENT`，载荷为 `{"event":"SERVICE_STOP","service_name":"..."}`，`request_id=0`。内部事件队列仍与 socket EVENT 分离。
4. IPC 接收端在对端关闭写方向后继续处理完整请求并发送应答；对残缺帧关闭连接。加入半关闭与合包回归用例。
5. 修正 `ipc_protocol.md` 的通道方向、帧长度定义、响应类型、心跳时间含义和 JSON 示例排版。

## 修复跟进

1. **线程模型：已修复。** 现有模块内增加 `timerfd` 驱动的 Timer Thread、独立 Monitor Thread 和异步 Logger Thread。Monitor 的 watch 数据加锁，健康事件在锁外送入 Runtime 队列；Service Manager 拒绝来自旧进程周期的迟到健康事件。未新增模块。
2. **STOP 卡住：已修复。** STOP 和失败清理先发送 SIGTERM；两秒后进程仍在则由 Service Manager 请求 Process Supervisor 发送 SIGKILL。运行中的服务在进程被回收后才从 STOPPING 迁移到 STOPPED；运行时退出路径也执行该超时检查。单元测试模拟拒绝退出，集成测试用 SIGSTOP 暂停 fake_service 后验证最终 STOPPED。
3. **覆盖缺口：已补主要边界。** 增加异常启动后的 FAILED 状态、IPC 超长/残缺帧、异步日志排空和旧健康事件测试。`startup_timeout` 的内核/调度极端路径及长期连接重连行为仍应在 Linux 实机验证时观察；当前测试不声称覆盖所有平台故障。
4. **Service 通知处理：已修复。** fake_service 现在按协议帧接收 `SERVICE_STOP` 并退出；对端断开时清除残帧，后续心跳周期可重新连接。

## 验证记录

- 已通读 `docs/P1/` 原有八份文档；对实现进行静态路径核对，并检查状态字段写入位置。
- 按用户要求，本轮不把隔离环境中缺少 CMake、交叉编译器及 Linux 运行环境纳入审查，也不在本机尝试编译或执行测试。
- 本轮改动尚无运行通过的证据。用户在 Linux 实机运行 `cmake -S . -B build`、`cmake --build build`、`ctest --test-dir build --output-on-failure` 后，可依据失败日志继续修复。
