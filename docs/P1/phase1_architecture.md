# RK3588 Device Runtime Management Framework

# Phase1 Architecture


## 1. Purpose

Phase1实现最小 Device Runtime Supervisor Core。

目标：

建立运行于RK3588 Linux ARM64用户空间的Runtime Management Layer。


Phase1验证：

- Runtime启动
- Service生命周期管理
- IPC通信
- Heartbeat检测
- 状态维护
- 基础恢复框架


Phase1不实现任何设备业务。


---

# 2. System Position


System:


Hardware

↓

Linux Kernel

↓

runtime_manager

↓

--------------------

vision_service

control_service

mcu_service

ota_service



Phase1:

实现：

runtime_manager

fake_service



Future service:

- vision_service
- control_service
- mcu_service
- ota_service


---

# 3. Runtime Responsibility


Runtime负责：


## Lifecycle Management

- Service注册
- Service启动
- Service停止
- 状态维护


## Health Monitoring

- Heartbeat接收
- Timeout检测
- Failure判断


## IPC Management

- Runtime控制接口
- Service通信接口


## Configuration

- JSON配置加载


## Logging

- Runtime运行日志


---

# 4. Runtime Non Responsibility


禁止：


## Vision Business

- Camera
- V4L2
- DMA-BUF
- RGA
- RKNN
- MPP


## MCU Business

- UART
- RS485
- MCU协议
- 实时控制


## Other Business

- OTA业务
- Cloud Management
- Database
- Web UI


Runtime只管理业务进程。

不实现业务。


---

# 5. Process Model


Phase1 Process:


runtime_manager


负责：

- Runtime Event Loop
- Service Manager
- Process Supervisor
- IPC Manager
- Monitor
- Config Manager
- Logger



fake_service


负责：

- 模拟业务Service
- 注册
- Heartbeat
- 状态响应



---

# 6. Module Ownership


Runtime模块：


runtime_manager

负责：

- Event Loop
- Event Dispatch


service_manager

负责：

- Service状态机
- 生命周期状态唯一写入


process_supervisor

负责：

- 子进程创建
- 子进程停止
- 子进程退出检测


ipc_manager

负责：

- Socket通信


monitor

负责：

- 健康检测
- 产生健康事件


config_manager

负责：

- 配置加载


logger

负责：

- 日志输出



---

# 7. Event Flow


所有Runtime事件：


External Input

↓

IPC Manager

或

Monitor

或

Process Supervisor


↓

Runtime Event Queue


↓

Service Manager


↓

State Transition



规则：

只有service_manager可以修改Service状态。


---

# 8. Thread Model


runtime_manager内部线程：


## Event Loop Thread

负责：

- epoll
- event dispatch


## IPC Handler Thread

负责：

- socket处理


## Monitor Thread

负责：

- heartbeat检测


## Timer Thread

负责：

- timerfd事件
- 每秒向Monitor Thread投递一次健康检查请求


## Logger Thread

负责：

- 异步日志
- 退出时清空待写日志队列


---

# 9. Service Lifecycle


State:


CREATED

↓

STARTING

↓

RUNNING

↓

STOPPING

↓

STOPPED



异常：


RUNNING

↓

FAILED

↓

RECOVERING

↓

STARTING



状态修改：

只有service_manager。

STOP先由process_supervisor发送SIGTERM；两秒内仍未退出则发送SIGKILL。service_manager在进程回收事件后将STOPPING改为STOPPED。


---

# 10. Heartbeat


配置：

heartbeat_timeout


默认：

heartbeat周期：

5秒


timeout：

15秒



失败：

连续3次timeout。



判定：

monitor产生HEARTBEAT_TIMEOUT事件。


service_manager决定状态变化。


---

# 11. Recovery


Phase1实现：

Recovery Framework。


包含：

- Failure detection
- Restart counter
- Restart delay计算


Restart:

最大5次


Delay:


2s

5s

10s

30s

60s



Phase1不实现复杂策略。


---

# 12. Configuration


格式：

JSON


字段：


service_name

executable

arguments

autostart

dependency

startup_timeout

heartbeat_timeout

restart_policy



Phase1:


支持：

- 加载
- 校验


不实现：

- dependency graph
- dependency scheduling


---

# 13. IPC


Phase1使用：

Unix Domain Socket



两个通信方向：


## Control Channel


test_client

↓

runtime_manager


用于：

START

STOP

QUERY_STATUS



## Service Channel


runtime_manager

↓

fake_service


用于：

HEARTBEAT

EVENT



---

# 14. Phase1 Acceptance


必须完成：


- runtime_manager启动
- JSON配置加载
- fake_service启动
- Service状态维护
- Unix Domain Socket通信
- heartbeat检测
- 状态查询
- 基础日志

