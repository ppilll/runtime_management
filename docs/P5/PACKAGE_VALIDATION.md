# Design Package Validation

完成日期：2026-10-03，Asia/Shanghai。对象为本次纯文档overlay，基线master@4843061fce2a329832105d34aaea380a35560c0e；完成前再次通过已连接GitHub确认master未变化。

## 实际检查范围

本次执行文档校验，检查所需文件、非空内容、Markdown本地链接、merge marker、无C++代码块/源文件、36条唯一测试ID、24条验收编号、四任务各八个必需字段、短提示词长度、JSON整数/7字段/默认阈值次序、120份源文件缓存/commit及冻结四栏目。
首轮发现README指向本报告而报告尚未生成；补齐后重跑。完整数字以随包design_package_validation.json为准。此检查不是C++编译器、仓库静态检查器、CTest或Linux运行测试。

## 人工一致性复核

- CPU aggregate busy公式、首次/失效baseline与Memory MemAvailable定义一致。
- CPU80/75 onlyWARNING；Memory80/75/95/90；critical降级一条active warning，clear仅本source全量重聚合。
- Process使用launched_generation/starttime、实际HZ/pagesize；starttime锚点不随CPU previous清除丢失；try快照避免等SM启动锁。
- native/external互斥；旧构造/API保留，新main一次full config；不新增wire/type。
- 采样复用Monitor worker，unknown不伪pressure或clear；原RM/SM/DSM/关闭ownership保持。
- T1→T2→T3→T4写入串行，Allowed/Forbidden/shared文件/测试门一致。
- P4 static历史记录与当前runtime NotVerified分开；没有把取回文件或测试源码说成已运行通过。

## 交付与限制

Overlay含根AGENTS.md与docs/P5完整文档、source metadata、codex_package及本报告，无C++源码/头文件。完整设计与任务也汇总为独立阅读稿。
本轮未向GitHub提交、创建分支/PR或启动实施chat；未运行C++、Linux procfs、socket、RK3588或性能测量。原120个仓库文件只作事实审查缓存，不进入文档压缩包。

Implemented：设计文档/任务包已生成。
Statically Verified：仅本次文档校验与人工设计一致性检查。
Host Linux Runtime Verified：Not Verified。
RK3588 Runtime Verified：Not Verified。
P5 C++实现：未实现，本轮禁止生成。

Design Blocking为空意味着可开始下一步实现；不意味着P4/P5运行验收完成。后续验证使用TEST_PLAN，实际失败须形成实施阻塞并修复。
