# Phase1 Module Design


# 1. runtime_manager


## Responsibility


Runtime系统入口。


负责：

- 初始化模块
- 创建Event Loop
- 分发Runtime事件


不负责：

- Service状态修改


## Input


- system signal
- runtime event


## Output


- dispatched event


## Dependency


- service_manager
- ipc_manager
- monitor
- config_manager
- logger



---

# 2. service_manager


## Responsibility


Service生命周期唯一管理者。


负责：

- Service注册
- 状态机维护
- 生命周期迁移


## State Ownership


service_manager是唯一可以修改以下状态的模块：


CREATED

STARTING

RUNNING

STOPPING

STOPPED

FAILED

RECOVERING



## Input


来自：

- START command
- STOP command
- process event
- heartbeat event


## Output


- state update
- lifecycle event



---

# 3. process_supervisor


## Responsibility


管理Runtime启动的Service进程。


Phase1负责：


- 创建fake_service进程
- 启动
- 停止
- 检测退出
- 停止宽限期后强制结束仍未退出的子进程


## Input


service start/stop request


## Output


Process Event:


PROCESS_STARTED

PROCESS_EXITED



## Dependency


Linux process API



---

# 4. ipc_manager


## Responsibility


Runtime IPC通信。


负责：


- Unix socket创建
- connection管理
- message解析
- response发送


## Input


Socket message


## Output


Runtime Event


## Dependency


- epoll
- event loop



---

# 5. config_manager


## Responsibility


加载Runtime配置。


## Input


JSON configuration file


## Output


Service Configuration


## Flow


config_manager

↓

runtime_manager

↓

service_manager



## Phase1限制


支持：

- load
- validate


不支持：

- dependency scheduling



---

# 6. monitor


## Responsibility


Service健康检测。


## Input


Heartbeat information


## Output


Health Event


包括：


HEARTBEAT_RECEIVED

HEARTBEAT_TIMEOUT



## Rule


monitor不能修改Service状态。


---

# 7. logger


## Responsibility


Runtime日志。


输出：


- timestamp
- level
- module
- message



---

# 8. Module Dependency


Allowed:


runtime_manager

↓

service_manager


runtime_manager

↓

ipc_manager


runtime_manager

↓

monitor


runtime_manager

↓

config_manager


service_manager

↓

process_supervisor



禁止：


monitor

直接修改service状态。


ipc_manager

直接修改service状态。


fake_service

访问Runtime内部模块。


