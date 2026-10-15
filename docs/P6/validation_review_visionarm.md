# P6 T4 — VisionArm Static Review

日期：2026-10-15（Asia/Shanghai）。按用户要求仅静态检测，不列编译测试要求/报告。
本文件在 Runtime 与 VisionArm docs/P6 各保存一份；设计合同和 T1 fixture 不修改。

本地源码包：`F:\嵌入式Linux学习资料\Vision Arm\Visionram_win`。没有 Git 元数据，遵守
visionram/AGENTS.md，不尝试 Git、不伪造与上游 `05148303c5ec5bba0ee89e707ab340e94e8aa3e1` 的一致性。
Runtime 对应 HEAD 为 `f8507027c87a9ae9899f51da708f8d469552c473` + T1/T3/T4 工作树。
组合源指纹见 Runtime docs/P6/static_validation.log；T2 原始106文件/74函数清单仍原样保留。

## 改动范围

只新增 `tools/test_apps/runtime_supervision/tests/runtime_pty_tests.cpp`、
`tests/phase6_pty_static_check.py`，在同 harness CMakeLists.txt 尾部增加一个串行目标/注册，及本报告。
没有改 visionram 的任何生产源码、原八个 harness test 函数、T2 checker、基线清单、fixture、
产品 CMake/build.sh、UART protocol/transport、control/camera/RKNN/pipeline、业务 PASS 或 MCU 文件。

Runtime 新静态入口可通过 --visionarm-root 显式选择本地包；该选项只读源和执行 Python 审计，
不操作 UART/socket，也不启动产品。默认不要求本地存在另仓库。

## Statically Verified

| 入口 | exit | 证据 |
| --- | --- | --- |
| phase6_adapter_static_check.py | 0 | 100原文件、74保护函数；同版合同、词法/局部include/格式、owner/client/CLI/shutdown、八个断言函数 |
| phase6_pty_static_check.py | 0 | 新文件词法/括号/格式、三函数断言/main、生产codec引用、故障/有界等待/串行注册与禁止范围 |
| Runtime phase6_static_check.py --visionarm-root | 0 | 上述两个入口、两份fixture与Runtime同版合同逐文件哈希；Runtime旧保护及新合同共同审查 |

三个新源码函数均有可观察断言：

| 函数（省略test_） | 使用生产源的 oracle | 限制 |
| --- | --- | --- |
| pty_crc_silence_hup_and_reopen | HELLO/STATUS→HEALTHY、boot0；parser.crc_errors、wire静默link_losses、IsRunning；关闭master、新PTY/symlink、open_successes、recovered仍保留pending、AckThrough、bounded Stop | 不连接真实 Runtime，不提供实际电气结论 |
| pty_confirmed_boot_change_and_same_id_reconnect | 0→7确认peer_reboot；旧revision ACK不清新issue；HUP后同ID只确认重连、epoch前进 | 不保证同ID无缝reboot可检测；不是持久boot counter |
| pty_hello_reject_and_missing_path_are_nonfatal | HELLO reject pending；owner仍running；missing path多次open尝试与pending，Stop | 不覆盖权限/占用专项或产品最终退出 |

fake MCU 仅测试 responder，用生产 EncodeMessage/DecodeMessage/FrameParser，不复制协议常量/parser/CRC/固件。
没有控制命令或执行器行为。原 raw8N1/exclusive 默认不变，不用 TIOCNXCL，harness 直接引用原四个生产源。
Fake worker 析构 Stop；poll20ms/写1s/观察5s deadline；join后才关master，避免fd关闭读竞态；
UartLink 原析构join不改，测试目标声明40秒串行。source guards 不证明这些时序已满足。

## Blocking ADR-P6-T2-01 再核对

`runtime_components.cpp` 保留最终 control_ok 对 UART READY/DEGRADED、控制成功与错误计数的要求；
`EvaluateRuntimePass` 消费该值，`runtime_report.cpp::PublishRuntimeReport` 使用 product_passed/report_ok
选择退出码。managed ready timeout 已非fatal、运行期间独立process heartbeat不代表 duration 结束时必然成功退出。
这会使持续链路故障在最终产品退出时被上层当作process failure。

本任务不授权业务验收/report 重构；T2也已明确将此点留为 Blocking。本次不篡改该判断，
不伪造 control_ok，不吞掉真实 fatal。关闭ADR需要单独冻结允许文件和正常managed停止/验收诊断表达方式。
所以完整产品的 process/link 分离与 T2完成 gate 仍不满足，不能宣布 P6 DONE。

## 证据等级 / 缺口

| 等级 | 本次结论 |
| --- | --- |
| Implemented | 三个PTY故障场景源码、追加注册、静态检查器及本报告 |
| Statically Verified | 上述 Python guards，原文件/函数保护和同版fixture |
| Host Linux Runtime Verified | 本次仅静态，不提供该层证据 |
| RK3588 + MCU Hardware Verified | 不提供该层证据 |
| Not Verified | product退出ADR、双仓库统一driver、真实早bind/反向exit/严格瞬态gate、压力/性能、H01–H10/RS485与上游版本一致性 |

Runtime完整38场景/28验收/H01–H10逐ID表见其 validation_review.md；本报告不能替代整体验收或板端证据。
