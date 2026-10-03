# PHASE5 DESIGN FREEZE CANDIDATE

完成日期：2026-10-03（Asia/Shanghai）；仓库核查始于2026-10-02。
事实基线：[ppilll/runtime_management master@4843061](https://github.com/ppilll/runtime_management/tree/4843061fce2a329832105d34aaea380a35560c0e)，完成前再次通过GitHub确认master未变。

## Frozen Decisions

1. P5 = Resource Monitoring & Health Facts；实际Linux procfs→Collector→typed snapshot→Monitor→FIFO→Aggregation→DSM。
2. Collector轻量独立，测量/策略/state分离；RM/SM/DSM原ownership全部保留。
3. system CPU用aggregate八counter delta，idle+iowait非忙，guest不重复；first/regression/zero/read错误不生成虚假0/100。
4. Memory使用MemTotal/MemAvailable；非法/缺字段unknown，无MemFree fallback。
5. Process为受管前台直接子进程、CPU/RSS/观测；name/PID/launched_generation/starttime和批量try重验证；CPU单核等价可>100。
6. 复用已有Monitor worker与1s timer，默认2s静态1..60s；heartbeat优先、tick coalesce、锁外I/O，无每service/new resource thread。
7. CPU80 activate/75clear，仅WARNING；Memory80/75与95/90，critical可直接clear或一条active warning降级；纯迟滞，无N次计数。
8. 复用resource_warning、ResourceSeverity、active、稳定cpu_monitor/memory_monitor source；first valid初始化、其后change-only。
9. clear擦本source并全量evaluate；service fault/其它资源/READY/RECOVERING/OFFLINE保留，不能直接RUNNING。
10. 资源事件不形成RecoveryRequest，不执行任意service restart；process读失败不伪service_failed。
11. RuntimeConfig+global7字段monitoring，整数JSON；旧load_file/ResourceThresholds/构造保留；旧external与新native入口互斥，新main一次加载。
12. current+previous memory-only、invalidity/age/error节流；invalid不clear确认latch，也不立即ERROR；unexpected worker异常走既有shutdown。
13. 不增加IPC，UDS/health/DEVICE_STATE_CHANGED保持；Temperature/Disk延期，collector typed seam留扩展点。
14. 四线程按T1系统collector→T2 process/config→T3 policy/runtime→T4review/validation串行，明确共享文件冲突。
15. 36项测试计划、24项验收映射；静态/Host/RK证据独立。原P4运行验收债务与新采集预算在最终实施门完成。

## Unresolved Decisions

核心架构选择：**无**。
部署可在已冻结schema内选择不同阈值/interval；实际RK3588 BSP是否具MemAvailable、权限/HZ/pagesize与预算仍需运行取证，是验收事项，不以虚构路径或参数补齐。
方法拼写/等价C++字段布局/test fixture可由实施任务决定，不允许改变冻结语义。

## Blocking Issues

**空：仅针对设计进入后续Codex实现。**
事实源、owner、collector/policy、身份、迟滞、config/IPC/scope与线程文件边界已明确，未发现必须先决定的设计缺口。

这不表示P4/P5实现已验收。当前P4只有历史静态验证记录；本轮不生成C++、不编译、不运行Linux/板端测试。实际HEAD差异、P4 baseline build/test失败、身份契约不成立、性能/关闭失败会形成新的实施阻塞，必须修复后再验收。

## Deferred Issues

Temperature generic zone-type discovery、Disk指定mount capacity、GET_RESOURCE_USAGE及resource streaming、process资源阈值、资源驱动恢复、GPU/NPU/AI profiling、云/DB/历史趋势、dynamic config、异步backend/硬实时SLA、全面global backpressure、IPC peer/session身份安全。RK3588 runtime evidence是待验收工作，不是被本包证明的能力。

## Implementation entry gate

先将本包根AGENTS.md与完整docs/P5复制到实际仓库，核对HEAD仍匹配或审查差异；然后按codex_package短提示词依次执行。设计Blocking为空后可进入实现。最终宣告P5完成仍要求实际运行证据，不把设计冻结称为Phase5 DONE。
