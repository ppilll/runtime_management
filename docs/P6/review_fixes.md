# P6 T4 — Oracle Migration / Limited Fix Record

日期：2026-10-15（Asia/Shanghai）。本次仅静态检测，不提供编译测试报告。
原 T1/T3 生产实现不改；没有已复现且需要在本次扩大源码范围修复的生产缺陷。
新增测试源码用于补组合与 UART/PTY 源证据，所有改动限定 CHANGE_LIST 的 T4 路径。

## 逐旧 oracle → 批准语义 → 新 oracle

| 旧检查 / 冲突 | CHANGE_LIST 授权 | 迁移与保留的保护 |
| --- | --- | --- |
| P3 aggregation `EVENTS ==` 原八项/labels | 唯一 component_health_changed | 只追加一项；P6 另比较原 ordinal 顺序 + 末尾一项；原八 labels/FIFO/场景和 owner 检查不删 |
| P3 IPC `members == expected` 原1..10/255 | 尾部 HEALTH_REPORT=11 | 仅 expected 增加11；原每个编号/10-byte/64KiB/JSON样例/query/subscription/overflow 保留 |
| P4 `PROTECTED` frame.hpp/ipc_manager.hpp/原P3IPC checker 全文保护 | type11、peer/session/new query 构造尾部接线 | 不移除 PROTECTED 条目；批准文件必须匹配 P6 固定 SHA256，其余仍逐字等于原基线；P6 同时检查新语义 |
| P4 main 的 P5 启动逆向还原 | P6 adapter + committed component query 两参数 | 先要求扩展精确出现一次并移除，再运行原 single load/native/same services 与 P4 main 全文逆向比较 |
| P5 event/aggregation/IPC 全文 freeze | normalized model/event/aggregation/transport | 每文件固定审查 SHA256；dispatcher 除一个 case 完全还原；aggregation 移除明确组件增量后全文等于 f8507027；原资源/process/terminal 逻辑全部保留 |
| P5 检查 P4 checker 的历史迁移 | P6 checker 历史比较批准迁移 | `legacy_checker` 先核验当前 P4 checker 固定 SHA256，再取 f8507027 的原 checker 运行既有 P5 逆向迁移 oracle；不把任意工作树当 baseline |
| P5 SM 原两只读方法以外全文比较 | heartbeat 增加 launched_generation 谓词 | 剥掉原两只读方法后，仅删 heartbeat 处第一次精确谓词，再与4843061全文比较；原 health_missed 相同谓词保持 |
| P5 config 的旧返回式/连续 parser 片段 | health tail 与严格局部 schema | 要求新 health_config 返回式；class Parser 精确比较原文；旧 service/monitoring/parser/timeout函数与f8507027逐函数不变，原监控字段/数值规则继续检查 |
| P5 apply_change 全函数保护 | writer lifecycle 撤销component session | 只逆向移除一行 component_checkpoint，剩余与4843061完全相等 |
| P5 handle_event 全函数保护 | private wake / bound type4 防旁路 | 要求两分支原文出现一次，精确逆向还原后继续原 P5/4843061 event owner 比较；resource native gate 原样 |
| P5 其余 lifecycle/recovery 比较、grace/reap 后缀 | 无开放 | captured_recovery/cancel_closure/drain_work/reap_children、旧后缀继续原全文比较 |
| old tests/registrations/history | 只能增补，不能弱化 | P6 比较 f8507027 的全部旧 test_* 定义原文；原 tests/CMake 前缀逐字一致；原12/13、独立六秒/PID/ECHILD、441、P4/P5全部 COVERAGE/预算/owner 保留 |

固定 SHA256 的规范化仅移除 BOM/统一换行，由 read_text utf-8-sig 执行；不忽略空白、注释、表达式。
`APPROVED_SOURCE` / `APPROVED_CHECKERS` 是串行审查后写入的常量，不在运行时从工作树生成。
新增的 checker 自身不做自引用哈希；其 Python 语法、格式、矩阵/保护逻辑由本次 diff 审查。
以后修改批准文件必须重新审查具体差异、更新证据，不能仅自动刷新 hashes。

## 本次新增断言

Runtime EA 新组合先把 component HIGH loss 置 ERROR，再对 process 运行 RM success；保持 ERROR。
之后设置 CPU warning + memory critical，unfresh HEALTHY 不能清；fresh clear 只清 link；
memory clear 后仍 WARNING、CPU clear 后 RUNNING；最后真正 RM terminal 后 component clear 不改 OFFLINE 快照或通知。
所有旧 EA 函数和 main 调用保留。

VisionArm 三个 PTY 函数直接链接 canonical sources，不增加另一份 MCU codec/firmware。
fake 仅 HELLO_ACK、HEARTBEAT→STATUS、可选 PING→PONG，使用生产 FrameParser / EncodeMessage / DecodeMessage。
坏 CRC 通过改编码后的 CRC 字节注入，生产 parser counter 为 oracle；没有人为 CRC 降级 policy。
silence 与真实 master close/HUP 分开；重连新 PTY/symlink，保持 raw8N1/TIOCEXCL，不用 TIOCNXCL 绕过资源竞争。
五秒 observation deadline、一秒非阻塞写 deadline、20ms poll、join-before-close 与析构清理，CTEST声明40秒且串行。
没有基于固定 sleep 假定状态已改变；2ms sleep 仅轮询带 deadline 的实际 predicate。

## 静态检查开发诊断

P3/P4 迁移后首次通过。P5 首次报 `missing/duplicated P6 heartbeat instance predicate`：
新 checker 错误假定全文只有一个同形谓词；原 health_missed 本来就有相同谓词。
改为要求两处、仅删除第一处（heartbeat）再做旧全文比较后通过，没有修改 SM 或旧断言。
后续 source-only 专项准入/聚合/config 检查及 VisionArm 两个静态入口均通过。
最终状态和完整命令输出见 static_validation.log；没有隐藏失败或给旧入口加跳过开关。

## 保留的 Blocking / 未实施部分

ADR-P6-T2-01 是产品最终退出语义决定，不能由 review 越权改 `runtime_report.cpp` 或伪造 control_ok。
保留真正 camera/model/logger/report fatal；不在本任务决定正常 managed stop 与产品验收结果的分离方案。
尚缺 production client→真实 Runtime→production UART 的统一测试 driver、反向 EOF/exit gate、
真实早 bind gate、严格100ms UART瞬态 gate、持续背压延迟、占用/权限tty与 health1/8/32 overhead 场景。
这些在 validation_review.md 逐ID标 Partial，不以现有分层源码抵充完整覆盖。

本次没有 wire 变化需求，没有建立新 UART/MCU protocol ADR；产品退出 ADR 继续阻止完整 P6 验收声明。
