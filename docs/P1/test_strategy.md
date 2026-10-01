# Phase1 Test Strategy


# 1. Test Goal


验证Runtime Core满足Phase1 Acceptance。


测试重点：

- lifecycle
- IPC
- heartbeat
- configuration
- process management



---

# 2. Unit Test


## 2.1 Config Manager


验证：


- JSON加载
- 字段解析
- 默认值



---

## 2.2 Service State Machine


验证状态迁移：


CREATED

↓

STARTING

↓

RUNNING


异常：


RUNNING

↓

FAILED

↓

RECOVERING



验证：

只有service_manager修改状态。



---

## 2.3 Process Supervisor


验证：


- process start
- process stop
- process exit event
- SIGTERM后超时的SIGKILL升级与进程回收



---

## 2.4 IPC Parser


验证：


- frame解析
- message type
- request id
- 拆包、合包、半关闭、过长帧与残缺帧
- Service Channel上的SERVICE_STOP EVENT



---

## 2.5 Monitor


验证：


- heartbeat receive
- timeout
- failure counter
- 旧进程的迟到健康事件不得使重启后的进程FAILED



---

# 3. Integration Test


## 3.1 Runtime Startup


步骤：


1.启动runtime_manager

2.加载配置

3.创建模块


Expected:


Runtime进入Event Loop。



---

# 3.2 Fake Service Start


流程：


test_client

↓

runtime_manager

↓

process_supervisor

↓

fake_service



验证：


state:

RUNNING



---

# 3.3 IPC Test


验证：


START

STOP

QUERY_STATUS



---

# 3.4 Heartbeat Test


流程：


fake_service发送heartbeat


周期：

5s



停止heartbeat。



Expected:


15s timeout


3次失败:


FAILED



---

# 3.5 Unexpected Exit Test


流程：


fake_service异常退出。



Expected:


process_supervisor

↓

service_manager

↓

FAILED

