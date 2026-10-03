# Threshold / Hysteresis Policy

## 冻结阈值

| metric | activate | recover | severity |
| --- | --- | --- | --- |
| CPU | >=80% | <=75% | WARNING only |
| Memory warning | >=80% | <=75% | WARNING |
| Memory critical | >=95% | <=90%降为WARNING；<=75%直接NORMAL | CRITICAL |

全为默认值，可静态配置。冻结简单迟滞，无连续N次计数、moving average、复杂rule engine。第一个合法越界measurement即可activate，避免引入新迟延参数；迟滞区间解决79.9/80.1反复越过激活阈值的抖动。>=activate、<=recover包含相等；sample invalid 不改变policy state。

## 明确状态转换

CPU：
- NORMAL / unseen：>=cpu_warning → WARNING，否则NORMAL。
- WARNING：<=cpu_clear → NORMAL，否则维持WARNING。
- 没有CPU CRITICAL。100%只证明资源饱和，不能证明关键功能不可用。

Memory（先检查direct clear，再按当前状态）：

| 当前 | 输入条件 | 下一状态 |
| --- | --- | --- |
| unseen / NORMAL | >=memory_critical | CRITICAL |
| unseen / NORMAL | >=memory_warning且<critical | WARNING |
| unseen / NORMAL | <warning | NORMAL |
| WARNING | >=critical | CRITICAL |
| WARNING | <=memory_clear | NORMAL |
| WARNING | 其它 | WARNING |
| CRITICAL | <=memory_clear | NORMAL |
| CRITICAL | >memory_clear且<=memory_critical_clear | WARNING |
| CRITICAL | >memory_critical_clear | CRITICAL |

CRITICAL→WARNING不是clear后再activate，而是同source一条active warning替换severity，避免临时RUNNING。CRITICAL→NORMAL只一条active=false。WARNING↔CRITICAL不创建两份latch。

示例默认序列：79→80→95→94→90→76→75，对应NORMAL→WARNING→CRITICAL→CRITICAL→WARNING→WARNING→NORMAL。另测95→75直接clear、80→79.9维持warning、95→94.9保持critical。

## 发布与兼容

cpu_monitor / memory_monitor，service_name空，type=resource_warning。
NORMAL：active=false；WARNING：active=true,severity=warning；CRITICAL：active=true,severity=critical。
第一份valid metric发一条初始化fact（normal也发clear），其后仅state/severity改变发布。首次CPU warming_up不发CPUfact；memory可单独产生fact。
reason记录metric/value、activate/recover阈值、变更动作；at为measurement时间。severity在clear中无健康意义，规范发送warning。sink在policy锁外调用；构造校验及legacy两个输入校验须先于任何发布。

report_resources/reportResourceUsage名称保留；P5改变其抖动语义：94.9不再立即降级，稳定样本不重复发facts。必须迁移 tests/runtime_core_tests.cpp:test_resource_monitor_thresholds 的索引/数量/94.9断言，保留80/95相等、非法输入不部分发布、static overrides、FIFO adapter测试；不能仅删除旧测试。

CPU何时可导致ERROR：只有另有经过验证的deadline失效/关键功能故障事实时，按该事实进入ERROR；该能力不在P5。不从CPU百分比直接反推service或recovery目标。Memory critical延续已有95%定义，是资源压力ERROR事实，不表示已OOM，也不自动恢复任意service。
