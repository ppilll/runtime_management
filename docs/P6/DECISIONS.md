# Architecture Decisions / A-B Comparisons

以下推荐是设计判断，非RK3588性能测试结论。通信owner详细九维比较见OWNERSHIP。

| ADR | A：优点 / 缺点 | B：优点 / 缺点 | 最终推荐 / 理由 |
| --- | --- | --- | --- |
| 01 owner | 独立mcu_service，隔离好/需抽库与commandIPC和部署 | 现有VisionArm owner，快且一致/业务crash同时失UART | B；现状成熟，不为展示搬迁业务 |
| 02 reuse | 抽生产canonicalUART库，长期干净/本轮跨repo成本高 | 原生产target保留，thinadapter；少改动/独立库仍欠缺 | B；A仅未来独立服务时必要，禁止copy |
| 03 health model | 复制全部LinkState，直观/耦协议且shutdown/fatal含义混 | 五normalizedstatuses，管理足够/需映射 | B；稳定、测试矩阵小、无额外device状态 |
| 04 snapshot delivery | 每100ms读取原snapshot，最快/漏瞬态loss与reboot | ownercoherentcurrent+pendingissue/ACK，可靠有限/少量seam | B；不依赖polling猜timeout，memoryO(1) |
| 05 reboot event | MCU_RESTARTED独立event，明显/enum膨胀仍不解决ID碰撞 | healthfact+peer_rebootreason/bootid，最小/不能保证所有重启 | B；保留合理unknown/reconnect语义 |
| 06 loss detector | Runtime按wireage再timeout，看似自主/dualowner误恢复 | 消费owner最终决策，正确/依赖owner实现 | B；既有UARTwatchdog唯一 |
| 07 recovery | 长期linkloss主动退出/restart，看似自愈/错误归属storm | UARTlocalrecovery与RMprocessrecovery分开/物理故障需维修 | B；不消耗processbudget |
| 08 internal event | MCUUP/DOWN多event，简单/硬编码且漏状态 | 一个component_health_changed，窄generic/一个轻抽象 | B；Supervisor通用，不能泛化成telemetry |
| 09 IPC | 扩展入站EVENT省编号/改方向、验证仍需设计 | type11 HEALTH_REPORT bind/report，方向明确/需升级client | B；旧EVENT和帧兼容 |
| 10 identity | 名称+timestamp或自报generation，快/可伪造及迟到 | SO_PEERCRED+SMlaunch+session/epoch+writer重验，成本小/限前台child | B；P6必须解决，非延期 |
| 11 required policy | 硬编码mcu HIGH，最快/纯视觉错误 | 复用ServiceHealthPolicy静态registry/有少量config | B；推荐robot HIGHrequired，部署明确选择 |
| 12 readiness | exec即RUNNING，原最简单/MCU尚NEGOTIATING也ready | requiredlinkfreshHEALTHY参与gate/processsuccess不变 | B；真实管理价值，不改变RMexec成功 |
| 13 report loss | 保留上次healthy永远有效，少timer/过时误报 | IPClease→UNKNOWN/availability降级，不算wiretimeout | B；1s/3.5s固定，明确观测面 |
| 14 query | 改GET_HEALTH所有reply，容易/旧消费者破坏 | opt-ininclude_components，旧{}保持/多一种payload | B；调试可观察但无metricsframework |
| 15 scheduling | 复用业务主loop，零thread/可能被camera和编码阻塞 | 一个managedclientloop，独立报告/增加join与线程资源 | B；heartbeat独立wire，禁止每componentthread |
| 16 test fixture | Runtime内重写fakefirmware，方便/重复协议和算法 | VisionArmtestharness链接原codec/cannedframes；Runtime只fakeJSON | B；主机可测，跨repo版本需固定 |
| 17 physical tests | PTY替代RS485，便宜/电气DE不可验证 | PTY userspace与RK实板分开，真实/需设备 | B；面试展示用独立证据层 |
| 18 persistence | DB/history，诊断多/IO维护范围过大 | current+pendingmemoryonly，轻/不保每次瞬态历史 | B；本phase足够 |

所有推荐考虑开发速度、稳定性、维护/跨repo成本、隔离、RK适配、测试与恢复；没有测量支持的资源/延迟值只作为目标。
