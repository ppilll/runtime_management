# ServiceAggregation / Device State Integration

复用现有 ServiceHealthPolicy{criticality,required} 类型用于静态component policy；registry分开是因为service进程与component不同事实对象，不能把mcu_link当虚构service。JSON/程序入口统一policy定义，不建MCUCriticality类型。

required回答“这个能力是否必须HEALTHY才就绪”，criticality回答“已确认不可用如何影响设备”。两者独立，可required+LOW或optional+HIGH。推荐机器人profile mcu_link required=true/HIGH；纯视觉不注册，若仍监控则optional/LOW；不按名字推断。

## 聚合规则

| Component观测 | required=true | required=false |
| --- | --- | --- |
| UNKNOWN初始，无曾就绪/既有故障 | 不满足readiness，其他健康时READY | 不阻止既有readiness |
| CONNECTING首次 | 同上 | 同上 |
| HEALTHY且fresh/current identity | 本component满足readiness；清本component可用性/故障后全量重算 | 全量重算 |
| DEGRADED | WARNING且不满足HEALTHY readiness | WARNING |
| UNHEALTHY + HIGH | ERROR | ERROR |
| UNHEALTHY + MEDIUM/LOW | WARNING；仍不满足readiness | WARNING |
| UNKNOWN/CONNECTING在同launch曾HEALTHY后 | 观测不可用；HIGH→ERROR，MEDIUM/LOW→WARNING；保持已确认adverse | 同criticality规则 |
| loss/reboot后的本地协商 | 保留episode最严重adverse，直到freshHEALTHY；不能短暂回READY/RUNNING | 同上 |
| producer故障/STOP/换代 | UNKNOWN；process事实独立聚合，新launchreadiness重新开始 | 同上但无requiredgate |

初始UNKNOWN不是已证明正常；开放线第一次open失败或5s managed等待失败是owner已确认不可用，可转UNHEALTHY，无需Runtime再加startup timer。
组件根本未配置时不存在gate，也没有虚假的UNKNOWN组件。

evaluate优先序：
1. 原P4 terminal latch OFFLINE。
2. 任意critical资源或HIGH component不可用/观测丢失 → ERROR。
3. 原critical process failure：只有原RM active recovery能决定RECOVERING，否则ERROR。
4. 资源warning、optional process故障、component DEGRADED或非HIGH不可用 → WARNING。
5. required service运行且required component freshHEALTHY且any service running → RUNNING，否则READY。

重要：HIGH component故障与process active recovery同时存在时仍ERROR；不能让“所有进程都在恢复”遮盖外部链路故障。component局部恢复不提供device_recovering flag，不产生RECOVERING。boot初始化仍由Runtime拥有BOOTING，init完成先READY再evaluate，正常query不修改它。

## reconcile 与状态表

新增component critical分支在当前RECOVERING转ERROR时选择既有critical_failure trigger，其他恢复/clear使用原issue_recovered等合法路径；不能用recovery_succeeded把链路恢复冒充RM成功。不增DSM状态/表；保留24allowed/417rejected，新增aggregation场景验证经过原允许路径。
service_started/RM success仍只清对应process latch；component健康独立 gate。componentHEALTHY只清这个component的adverse和observationavailability，不能清cpu_monitor/memory_monitor、别的service、P4terminal。reason与source来自最终触发事实，typed source使用component:mcu_link，不能借service_name覆写process owner。

例：
- 机器人进程启动、link CONNECTING：READY；firstREADY→RUNNING（资源和其它服务均正常）。
- cable lost：process row HEALTHY，link UNHEALTHY/HIGH，Device ERROR；pid/generation/restart_count不变。
- 重连后HEALTHY但CPUwarning：WARNING；control进程正在恢复：RECOVERING；memorycritical：ERROR；P4terminal：OFFLINE。
- optional/LOW linklost：WARNING，恢复后全部其他健康才回RUNNING。
- 进程重启exec成功、MCU还未READY：原process recovery SUCCESS可提交，但设备不得RUNNING；根据新component未知readiness与其他事实为READY/ERROR/WARNING。

## OFFLINE

物理MCU掉线应为ERROR（HIGH）或WARNING（lower），不用OFFLINE。原OFFLINE表示P4 terminal恢复结果且当前Runtime寿命不可退出；link再READY不会解除它。若产品未来把整机网络/电源掉线定义为OFFLINE，另ADR，不改本轮终结语义。
