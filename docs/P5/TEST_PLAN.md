# Testing Matrix

当前全表为待实施计划，不是test passed。U=fixture unit，I=真实Linux/runtime integration，S=结构检查，H=Host Linux，R=RK3588。

| ID | 场景与oracle | 层 | Owner |
| --- | --- | --- | --- |
| P5-01 | aggregate cpu八字段/optional字段/guest不重复计数 | U | T1 |
| P5-02 | total1000→1200、idle400→450=75%，0/100边界 | U | T1 |
| P5-03 | 首次CPU absent/warming，无warning/clear；memory独立可用 | U+pipeline | T1/T3 |
| P5-04 | regression/wrap/overflow/delta0/idle>total/字段变化、rebaseline | U | T1 |
| P5-05 | meminfo两字段、重复/单位/负值/overflow/total0 | U | T1 |
| P5-06 | MemAvailable计算1000/200=80%，与MemFree无关 | U | T1 |
| P5-07 | memory恰80→WARNING，source/type/active/time准确 | U | T3 |
| P5-08 | 恰95→CRITICAL，一条同source替换 | U | T3 |
| P5-09 | 95→94.9保持→90降级→76保持→75clear，95→75直接clear | U+I | T3/T4 |
| P5-10 | CPU80 activate、79.9保持、75clear、100只WARNING | U | T3 |
| P5-11 | 79.9/80.1/79.8/80.2仅一次activate，stable不重复发 | U | T3 |
| P5-12 | healthy required running+资源warning→Device WARNING | U+I | T4 |
| P5-13 | memorycritical→ERROR，service recovery不能盖住 | U+I | T4 |
| P5-14 | clear时control失败/optional故障/另一source仍在，不能虚假RUNNING | U+I | T4 |
| P5-15 | sample前/中PID消失→not_present，无SM/RM动作或stale RSS | U+I | T2/T4 |
| P5-16 | 相同PID新launch、相同launch新starttime、pre/post身份改变、迟到结果丢弃 | U+I gate | T2/T4 |
| P5-17 | stat/meminfo read/permission/parse/size失败，部分validity独立且invalid不clear | U+I | T1/T3/T4 |
| P5-18 | sample前/中shutdown、signal/observer throw、join/lifetime、teardown后无publish | U+I gate | T3/T4 |
| P5-19 | N=1/8/32，排序/partialfailure隔离、2+N reads、O(N)身份查询 | U+H/R | T2/T4 |
| P5-20 | 资源不产生RecoveryRequest/launch/reservation，具名故障原恢复仍有效 | U+I | T4 |
| P5-21 | stat comm空格/右括号、fields14/15/22/24、Z、PID mismatch、rss负/溢出 | U | T2 |
| P5-22 | process首次CPUabsent但RSSvalid、多线程CPU>100、fake HZ/pagesize、长间隔reset | U+H/R | T2/T4 |
| P5-23 | SM try快照/revalidate锁忙skip，不等exec、不提交未验证previous | U gate | T2/T4 |
| P5-24 | due边界、fake时间倒退/elapsed0、slowcycle不catch-up、timer coalesce | U+I gate | T3 |
| P5-25 | single/services旧root、默认、7字段合法/反序/范围/type/null/unknown/duplicate | U | T2 |
| P5-26 | 旧Thresholds三值及derivedclear、load_file wrapper、新native config无override歧义 | U | T2/T3 |
| P5-27 | legacy run前report可FIFO排队；native拒绝外部report/reserved source及post(Event)封装绕过，内部正常发布 | U+I | T3 |
| P5-28 | query不读proc/推进policy、并发copy、scan unavailable/empty与age>3interval stale | U | T3 |
| P5-29 | clear到READY、保持OFFLINE、返回RECOVERING，多source只清一个 | U | T4 |
| P5-30 | runtime_manager executable真正native读Linux CPU/memory/process，不是fake-only库 | I+H/R | T4 |
| P5-31 | UDS types1..10/255、10bytes/64KiB、GET_HEALTH首心跳UNKNOWN、既有subscription保持 | S+I | T4 |
| P5-32 | 原6s/PID/ECHILD、RM五次budget/token/cancel/manual/OFFLINE全保留 | S+I | T4 |
| P5-33 | 纯文本parser/seam，无proc写入、新worker、DB、GPU/NPU或资源隐式恢复 | S+review | T4 |
| P5-34 | 连续失效保留已有latch，首次失效不声称normal，有效样本恢复 | U+I | T3/T4 |
| P5-35 | unexpected worker exception请求shutdown，不terminate/destroy在途对象 | U+I gate | T3/T4 |
| P5-36 | 原graph/heartbeat/child cleanup/sink lifetime/IPC overflow及main一次加载 | S+I | T4 |

## 实际落点与既有证据

新resource_collector_tests：T1系统与T2进程；resource_monitoring_tests：T2配置/身份、T3policy/runtime、T4补充。T3迁移runtime_core资源测试，T4增补现有integration目标。
复用当前函数：
- runtime_core：test_resource_monitor_thresholds、test_runtime_resource_fact_adapter、test_monitor_instance_reuse_and_cleanup_generation。
- event_aggregation：test_resource_critical_downgrade_and_clear、test_optional_recovery_failure_and_resource_sources。
- recovery_coordination：test_untrusted_recovery_ingress_cannot_clear_resource、test_real_optional_terminal_and_partial_resource_recovery、test_offline_cancels_other_task_and_leaves_no_recovering_service。
- phase2_runtime_shutdown_regression与原test_runtime_shutdown_respects_long_grace_period。

测试名称存在不代表执行。native真实host压力可干扰旧synthetic expected，因此旧合成测试使用external，不对真实CPU断言固定80%。精确时间用fake clock；身份/关闭使用有release guard的gate及有限测试timeout，不用sleep猜测。

## 后续Linux命令

```text
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 -B tests/phase3_validation_static_check.py
python3 -B tests/phase4_static_check.py
python3 -B tests/phase5_static_check.py
git diff --check
```

P5 checker新增后才能运行。P3 Monitor表达式、P4 main字节保护按CHANGE_LIST迁移，不能删owner/terminal/状态/协议/6s检查。P4 git-history检查需要实际仓库历史，不能在无.git的文档缓存中伪造通过。CTest默认串行，signal handler场景不随意-j。

H：记录kernel/architecture/HZ/pagesize、命令/退出码/stdout/stderr、proc样本、周期与overhead；真实资源只断言合法性/公式/接线。
R：记录板型/BSP/kernel/ARM64与权限、同样采集和shutdown/recovery smoke、N=1/8/32预算。用部署已有toolchain，仓库无路径不得虚构；交叉编译不能代替板端运行。

## 验证状态

逐ID分开记录Implemented、Statically Verified、Host Linux Runtime Verified、RK3588 Runtime Verified、Not Verified。Static只证明结构/人工路径，不证明类型链接/运行。T4报告实际函数/命令/结果/证据与未验证项。
P5完成需要host build/相关完整CTest及RK3588核心采集/生命周期回归完成；若无目标环境，只能报告host阶段已验证/板端待验收。本设计包检查仅是文档校验，不是P5实现通过。
