# RK3588 Embedded Device Runtime Management Framework

# Phase 0 Architecture Design Document

Version: P0-Final\
Status: Architecture Frozen

## Document Purpose

本文档用于冻结 RK3588 Embedded Device Runtime Management Framework Phase
0 架构设计结果，作为后续 Phase 1 Runtime Core Framework 开发输入。

------------------------------------------------------------------------

# 1. System Architecture Overview

## System Position

系统定位：

Hardware → Linux Kernel → Runtime Management Layer → Business Services

Runtime Management Framework 是设备运行监督层（Device Runtime
Supervisor）。

负责： - Service 生命周期管理 - Service 启停控制 - 状态维护 - 健康检测 -
异常恢复 - IPC 管理 - 配置管理 - 资源监控

不负责： - Camera pipeline - V4L2 - DMA-BUF - RGA - RKNN - MPP -
MCU业务控制逻辑 - 机器人业务流程

核心原则：

Runtime 管理业务服务，Service 实现业务能力。

------------------------------------------------------------------------

# 2. Module Responsibility Definition

  Module             Responsibility   Non Responsibility
  ------------------ ---------------- --------------------
  Runtime Manager    系统协调入口     不实现业务
  Service Manager    生命周期管理     不处理业务逻辑
  Monitor            状态采集         不执行恢复策略
  Recovery Manager   故障恢复执行     不分析业务
  IPC Manager        服务通信         不定义业务协议
  Config Manager     配置加载         不生成业务逻辑
  Logger             系统日志         不替代监控系统

------------------------------------------------------------------------

# 3. Process/Thread Model

采用：

Multi Process + Internal Thread Model

进程：

runtime_manager - vision_service - control_service - mcu_service -
ota_service

原因： - 故障隔离 - 独立恢复 - 资源限制

线程用于： - Runtime内部事件循环 - Monitor任务 - IPC处理 - Timer任务 -
Logger任务

------------------------------------------------------------------------

# 4. Service Lifecycle Model

冻结状态：

CREATED\
STARTING\
RUNNING\
STOPPING\
STOPPED\
FAILED\
RECOVERING

状态转换：

CREATED → STARTING → RUNNING

RUNNING → STOPPING → STOPPED

RUNNING → FAILED → RECOVERING → STARTING

Heartbeat:

-   周期：5s
-   Timeout：15s
-   连续失败：3次

Restart:

-   最大重试：5次
-   初始延迟：2s
-   最大延迟：60s

------------------------------------------------------------------------

# 5. IPC Design

通信：

Unix Domain Socket

模型：

Request/Response + Event Notification

消息类型：

  Message        方向
  -------------- -------------------
  START          Runtime → Service
  STOP           Runtime → Service
  QUERY_STATUS   Runtime → Service
  HEARTBEAT      Service → Runtime
  EVENT          Service → Runtime

Timeout：

START 30s\
STOP 10s\
QUERY_STATUS 3s\
HEARTBEAT 15s

IPC只负责运行管理信息，不传输业务数据。

------------------------------------------------------------------------

# 6. Configuration Design

格式：

JSON

原因： - C++生态成熟 - 稳定 - 适合嵌入式

冻结字段：

-   service_name
-   executable
-   arguments
-   autostart
-   dependency
-   startup_timeout
-   heartbeat_timeout
-   restart_policy

Dependency：

Phase 1仅支持简单单向依赖。

------------------------------------------------------------------------

# 7. Failure Recovery Design

支持：

## Service Crash

检测： - PID退出 - exit code

恢复：

FAILED → RECOVERING → restart

## IPC Failure

检测： - socket断开 - heartbeat timeout

处理： - reconnect - restart service

## 不自动恢复

-   Kernel failure
-   Hardware damage
-   Firmware corruption

------------------------------------------------------------------------

# 8. Resource Monitoring Scope

Phase 1：

CPU: - /proc/stat

Memory: - /proc/meminfo - Process RSS

Process: - PID - alive - exit status

Heartbeat: - Service健康状态

Phase 1不包含：

-   GPU监控
-   NPU监控
-   AI性能分析
-   云端监控
-   历史数据库

------------------------------------------------------------------------

# 9. Phase 1 Development Input

Phase 1必须遵守：

1.  Runtime负责管理，Service负责业务。
2.  使用7状态生命周期模型。
3.  使用Unix Domain Socket IPC。
4.  使用JSON配置。
5.  实现CPU、Memory、Process、Heartbeat监控。
6.  实现Crash Recovery、Timeout Recovery、Retry Limit。

禁止修改：

-   架构边界
-   模块职责
-   IPC方向
-   Service模型

------------------------------------------------------------------------

# 10. 未解决问题和技术风险

## OTA升级机制

未定义： - 版本管理 - rollback - compatibility

## 动态配置更新

未定义： - runtime修改配置 - service热更新

## Recovery复杂策略

当前仅支持restart。

未来可能需要： - 多服务恢复策略 - 故障等级

## Dependency管理

当前仅支持简单依赖。

未来可能需要： - DAG依赖 - 循环检测

## Logger能力

未定义： - log rotation - persistent storage - remote upload

## MCU管理边界

未定义： - MCU reset - firmware update

## 安全模型

未定义： - IPC权限 - service隔离 - secure boot关联

------------------------------------------------------------------------

# Phase 0 Final Status

Architecture Freeze: PASS

Phase 1进入： Runtime Core Framework Design & Implementation
