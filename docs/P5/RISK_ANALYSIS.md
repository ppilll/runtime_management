# Risk Analysis

| 风险 | 影响/事实 | 冻结处理与验证 |
| --- | --- | --- |
| P4尚无运行验收 | 历史validation_review明确未编译/运行，不能假设基线C++可通过 | P5最终集成前运行原全部CTest；失败需先定位，不改记录成PASS |
| 同worker慢proc I/O | 可能延迟heartbeat；read/join非硬实时 | heartbeat优先、限文件size、tick coalesce、overrun节流、预算实测；未来隔离thread另ADR |
| SM同步exec持锁 | blocking all_statuses可能等startup_timeout | 两个批量try锁接口skip；不重写backend；P5-23 gate |
| 初次process身份锚点 | PID数字不是绝对证明；身份快照非原子 | 独占reap+launch token+starttime、前后重验证；外部reaper/daemonizing不支持 |
| counters / iowait regression | Linux可能出现回退、hotplug或异常输入 | invalid/rebaseline，不猜wrap或假0/100；P5-04 |
| Memory字段/BSP差异 | 缺MemAvailable无法应用公式 | unavailable，无MemFree fallback；host/RK实际取样 |
| RSS并非全归属 | direct child的共享页可重复，stat RSS可能近似，不含后代 | 仅diagnostic，不自动求和当system used，不赋服务pressure fault |
| 未知期间保留latch | 已确认WARNING/ERROR可能保守滞留；无latch时state不能反映资源未知 | snapshot validity/age与日志明确；不伪clear，不引availability-state机 |
| native/external竞争 | 两生产者可覆盖同source、旧测试被host压力影响 | 构造时模式互斥，native public source guard，单一Monitor policy |
| 旧测试/static冻结迁移 | 94.9/每次2fact断言及main字节保护会冲突 | 记录准确批准改动，替换语义oracle；保留所有旧核心保护 |
| 阈值适用性 | 80/95不是目标板调优证据，CPU100不证明功能坏 | CPU onlyWARNING，默认clear95→90/80→75；static configurable，板端load验证 |
| O(N)规模 | 每周期2+N读，SM快照/比对若实现成N次list会O(N²) | 批量hash/name lookup、32服务budget、无per-service thread |
| cache/queue时间差 | current snapshot可先于writer health提交，文件间非原子 | 文档承认采样时间/FIFO，无事务快照承诺 |
| 已有global queue/IPC安全债务 | 全局queue未有总容量、旧连接heartbeat身份等P4已列 | change-only/coalesce仅缓解本次输入，不宣称全部背压/认证问题解决 |
| shutdown/exception | worker在read中、gate析构、callback throw可能清理迟到 | 停止检查、join前owner存活、unexpected异常请求既有shutdown，gate release guard |
| 性能未测 | p95<20ms/p99<50ms/CPU<1%均目标，非事实 | host与RK分列结果；不伪硬实时或板端PASS |

Memory critical只延续已存在device resource pressure事实，不等于实际OOM，更不指示应重启哪一个service。资源severity的安全边界比“自动自愈”展示价值优先。

设计阻塞与验收债务分开：本包没有未决核心架构选择；Host/RK运行与性能仍是实施/发布门。若当前HEAD变化、独占reap契约不成立或基线build失败，新的证据可以形成实施阻塞，不能继续声称空。
