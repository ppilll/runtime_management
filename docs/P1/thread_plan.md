# Phase1 Thread Execution Plan


# Execution Order


Phase1必须按照以下顺序执行：


Session0

↓

Session1

↓

Session2

↓

Session3

↓

Session4



禁止并行修改同一代码区域。


---

# Session0

## Architecture Review


## Goal

确认P1设计文档一致。


## Input

All documents under:

docs/P1/


## Output

Architecture review report。


## Modify Range

docs/


## Forbidden

- 修改代码
- 创建源码
- 修改架构


## Validation

确认：

- module ownership明确
- IPC角色明确
- lifecycle ownership明确



---

# Session1

# Foundation


## Goal

创建C++工程基础。


## Input


Read:

- phase1_architecture.md
- module_design.md
- coding_rules.md


## Output


创建：

- CMake工程
- src结构
- include结构
- tests结构


## Modify Range


Allowed:


CMakeLists.txt

src/

include/

tests/


## Forbidden


禁止实现：


- Service状态机
- IPC协议
- Heartbeat
- Recovery逻辑



## Validation


必须：

- CMake configure成功
- 基础工程编译成功



---

# Session2

# Runtime Core


## Goal


实现Runtime核心框架。


## Input


Read:


- module_design.md
- thread_plan.md
- phase1_architecture.md



## Output


实现：


runtime_manager

service_manager

process_supervisor

config_manager

monitor

logger



## Modify Range


Allowed:


src/runtime/

src/service/

src/process/

src/config/

src/monitor/

src/logger/


include对应目录



## Forbidden


禁止：


- 修改IPC协议
- 实现业务Service
- 接入VisionArm
- MCU逻辑
- OTA逻辑



## Required Validation


验证：


- runtime启动
- config加载
- service状态迁移
- process事件处理



---

# Session3

# IPC and Fake Service


## Goal


实现跨进程通信和测试Service。


## Input


Read:


- ipc_protocol.md
- test_strategy.md
- module_design.md



## Output


实现：


- ipc_manager
- fake_service
- integration test



## Modify Range


Allowed:


src/ipc/

tests/

tools/fake_service/



## Forbidden


禁止：


- 修改IPC协议
- 修改Service状态设计
- 添加业务逻辑



## Required Validation


验证：


- START
- STOP
- QUERY_STATUS
- HEARTBEAT



---

# Session4

# Review and Stabilization


## Goal


验证Phase1实现符合设计。


## Input


Read:


All docs/P1



## Output


review_report.md



## Modify Range


Allowed:


bug fix

test fix

documentation fix



## Forbidden


禁止：

- 架构修改
- 新增模块
- 扩大Phase范围



## Validation


执行：

- build
- unit test
- integration test

