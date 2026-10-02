# Thread2：Linux 进程后端实现

## 接口与边界

沿用 Phase1 的 `ProcessSupervisor` / `PosixProcessSupervisor`，不新增管理模块。
Service Manager 的现有 `start/stop/force_stop/reap` 调用继续有效。

`PosixProcessSupervisor` 另外提供设计文档要求的名称：

| 接口 | 行为 |
| --- | --- |
| `launchProcess(config)` | 等同于 `start(config)`，返回受管子进程 PID；启动失败抛出异常 |
| `terminateProcess(pid)` | 等同于 `stop(pid)`，向受管进程发送 SIGTERM |
| `checkProcessAlive(pid)` | 非消费式查询；无效、非受管、退出或已回收的进程返回 false |
| `getPid(service_name)` | 返回该服务的受管 PID；未知名称返回 -1 |
| `force_stop(pid)` | 向仍存活的受管进程发送 SIGKILL |
| `reap()` | 使用 waitpid(WNOHANG) 回收直接子进程，并返回原始退出状态 |

`getPid()` 在进程退出后、`reap()` 前仍返回已记录的 PID，以保留退出事件的关联。
非空服务名称在旧进程回收前不能重复启动；空名称兼容 Phase1 的匿名进程调用。

进程后端不修改 ServiceState，不决定重启，也不处理依赖。
现有 RuntimeManager 消费 `reap()` 的结果，按 PID 生成 `process_exited` 事件；
Service Manager 完成状态同步。Monitor 的心跳职责保持不变。

## 启动与停止

- 父进程准备 argv 和 envp，然后 fork/execve；不通过 shell 解析参数。
- 继承父进程环境，`environment` 中的 KEY=VALUE 覆盖同名变量；重复键以最后一项为准，允许空值及值中的等号。
- `working_directory` 为空时继承当前目录，否则在子进程执行 chdir。
  executable 按 execve 路径规则解析；相对路径相对于子进程工作目录，不搜索 PATH。
- 拒绝空 executable、嵌入 NUL、非法环境项和非正/超出时钟范围的 startup_timeout。
- 使用 CLOEXEC 错误管道和 epoll，在 startup_timeout 内完成启动握手。
  子进程报告信号设置、chdir 或 execve 的 errno；父进程在失败或超时时终止、回收子进程并撤销记录。
- 子进程只调用异步信号安全函数，不分配内存、不调用 setenv、不使用父进程锁。
  重置 SIGTERM、SIGINT、SIGCHLD 的处置并清空信号屏蔽字，使停止信号能够生效。
- Service Manager 先请求 SIGTERM，达到每个服务的 shutdown_timeout 后调用 force_stop。
  该流程保持异步，不在 terminateProcess 中等待；STOPPING 在收到退出事件后变为 STOPPED。

## PID 与资源管理

记录包含 PID、服务名称及可用的 pidfd，公共操作由互斥锁串行化。
在 fork 前分配记录，在回收前预留退出结果空间，避免分配异常丢失子进程或退出状态。

内核与头文件支持时通过 pidfd_open / pidfd_send_signal 使用稳定进程句柄；
pidfd 的非阻塞 poll 查询不会消费退出状态。旧内核返回 ENOSYS 或旧头文件缺少 syscall 定义时，
回退到 waitid(WEXITED | WNOHANG | WNOWAIT) 加 PID 信号操作。
WNOWAIT 保留僵尸直到后端回收，因此在独占回收约定下 PID 不会复用。
其他 pidfd_open 错误会使启动失败，不静默降级。

使用约定：后端独占这些直接子进程的回收；禁止其他线程/信号处理器回收它们，
禁止运行期间使用 SIGCHLD=SIG_IGN 或 SA_NOCLDWAIT；启动时会检查这两种自动回收设置。
父进程环境在启动快照期间不得被并发修改。服务必须以前台进程运行；不管理其自行派生的后代或进程组。

无效或非受管 PID 的信号请求不产生系统信号。发现 ECHILD 时存活查询返回 false，
回收返回 status=-1，避免将未知状态误报为正常退出。旧内核回退路径的 PID 安全依赖独占回收约定。
析构先终止所有剩余直接子进程，再回收和关闭 pidfd；调用方须先结束所有并发方法调用。

## 验证记录

按本次用户要求，仅执行静态检查，没有运行 CMake、编译器或 Linux 进程测试。
`git diff --check` 通过；源码静态核对覆盖接口声明/定义、子进程异步信号安全调用、
PID 信号边界、错误分支的 FD/子进程清理、非消费式退出查询及现有停止超时事件链。
环境中未找到 cppcheck/clang-tidy，静态检查不包含这些工具的分析，也不等同于编译通过。

新增 `tests/process_lifecycle_tests.cpp` 并登记 CTest，供后续 Linux ARM64 环境验证。
该测试通过重新执行自身作为 fake service，覆盖：

- 启动、SIGTERM 正常退出、服务 PID 查询、重复启动与无效/非受管 PID。
- 参数中的空格、环境继承/覆盖/空值、工作目录和非零退出码。
- 存活查询保留退出状态、回收后清理记录、同名服务再次启动。
- executable/chdir 失败和非法输入后的清理与再次启动。
- 忽略 SIGTERM 的进程在 shutdown_timeout 后收到 SIGKILL，退出事件同步服务状态。
- 析构回收子进程，以及外部回收后的未知状态处理。

这些测试源码本次仅静态检查，未执行；旧内核回退路径、启动超时和高并发行为仍需目标机验证。
