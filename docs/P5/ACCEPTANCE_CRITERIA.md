# Phase5 Acceptance Criteria

本表是未来验收门，当前不标PASS。映射TEST_PLAN的P5-ID，逐项给实际证据。

| # | 必须满足 | 对应P5-ID |
| --- | --- | --- |
| 1 | 真实Linux aggregate CPU从/proc/stat采集 | 01,30 |
| 2 | 有效两sample delta与明确busy公式 | 02,04 |
| 3 | 首次CPU不产生虚假负载/clear | 03 |
| 4 | 真实LinuxMemory读取/proc/meminfo | 05,30 |
| 5 | 基于MemAvailable的稳定定义，坏输入absent | 05,06 |
| 6 | collector测量/Monitor policy/设备state分离 | 20,33 |
| 7 | resourceWARNING进入DeviceWARNING | 07,10,12 |
| 8 | resourceCRITICAL进入DeviceERROR | 08,13 |
| 9 | valid资源恢复clear同source latch | 09,10,34 |
| 10 | clear重聚合所有health | 14,29 |
| 11 | service fault不会被resource clear误清 | 14 |
| 12 | resources不无依据触发service recovery | 20 |
| 13 | RM唯一恢复policy owner | 20,32,33 |
| 14 | SM唯一PID/lifecycle owner | 16,23,32 |
| 15 | 无one-thread-per-service/新增采样thread | 19,24,33 |
| 16 | process PID reuse/launch mismatch明显竞态防护 | 15,16,21 |
| 17 | 单次collection失败不制造虚假ERROR或clear | 17,34 |
| 18 | shutdown正确停采样，join/lifetime，无teardown后发布 | 18,35 |
| 19 | fixture/fake reader可测，不修改真实/proc | 01–11,15–17,33 |
| 20 | P1–P4核心接口无无必要破坏 | 26,27,32,36 |
| 21 | UDS帧/type/旧health/subscription保持 | 31 |
| 22 | 无GPU/NPU/cloud/DB/history/web范围膨胀 | 33 |
| 23 | 文档/变更清单/线程任务/实现一致 | 33,36 + T4 review |
| 24 | static/host/RK/NotVerified明确，源码存在不等于passed | T4证据报告 |

附加明确门：process单核CPU可>100、RSS实际pagesize；SM锁忙skip；global配置向后兼容；native生产入口真实接线；模式互斥；stale/unknown诚实；P3/P4检查迁移有新oracle；原10executables/11CTest及六秒回归保留。

完成等级：
- Design Freeze Candidate：设计一致、事实可追踪、核心Unresolved与Design Blocking为空；不表示实现完成。
- Host阶段：Linux build/全部相关CTest/static通过，native procfs与shutdown、性能预算有host证据。
- RK3588阶段：真实板端核心采集、进程实例/关闭/恢复回归与budget证据；不能用x86或cross-build代替。
缺板端环境必须写Not Verified；不能把Host阶段宣布为完整RK3588 Phase5 DONE。
