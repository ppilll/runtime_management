# PHASE4 DESIGN FREEZE CANDIDATE

以下为原设计冻结时点记录。T1–T4 后续实现与静态审查的当前状态见
[validation_review.md](validation_review.md)；运行验证为空，未宣告 Phase4 完成。

## Frozen Decisions
1. 基于master@2ccbc733a1386c46917e6aee2fc7e0680e4321e9，延续现有Supervisor/多进程/C++17/CMake/Unix socket，仅新增RM core。
2. 推荐ownership方案B：RM唯一policy/retry/backoff/deadline/terminal owner；SM唯一lifecycle/PID/generation执行owner，DSM仅设备状态。
3. 删除SM自动retry loop、Runtime旧结果推导、IPC manual poll，所有操作复用同一writer；不增加recovery线程。
4. 保留SM生命周期generation；RM per-service recovery_generation为request ID；captured execution generation桥与candidate gate防stale。
5. 最小active BACKOFF/EXECUTING，无active即idle；内部SUCCESS/FAILED/TIMEOUT/CANCELLED，三种现有recovery事件不扩枚举。
6. 五次累计reservation、2/4/8/16/32、cap60、不reset；manual单事务不占自动预算，后续新fault独立判断自动策略。
7. recovery_timeout为唯一新增JSON策略字段，1..86400秒；默认63+5×(shutdown+startup+5)+1；admission绝对deadline，不延长。
8. 成功为exec+关联/期限验证，非业务或首心跳健康；SUCCESS重新聚合其它fault/resource。
9. 保留HIGH或任意heartbeat terminal OFFLINE、普通optional terminal WARNING和OFFLINE终止性。
10. 依赖只静态闭包stop及explicit START恢复，不自动revive/动态group；每turn最多一个恢复launch。
11. IPCwire/commands/type不变，不加GET_RECOVERY_STATUS；只type6内部路由迁移。
12. shutdown债务V04已实现修复，P4保留并真实回归；cancelAll先于stop/teardown。
13. 资源severity输入已存在，Phase5实际采集延后；业务/框架/reboot禁止范围明确。
14. T1→T2→T3→T4代码修改串行；测试matrix32项，完成验收20项，四种验证状态分开报告。

## Unresolved Decisions
无必须在开始编码前选择的架构方案。具体方法拼写、C++布局和测试fixture实现由后续线程在已冻结语义内决定，不构成新的架构决策。
部署是否将mcu_service设HIGH/required，以及目标设备是否要求严格硬实时恢复期限，需部署需求明确；当前包冻结现有默认和非硬实时边界。若需求改变，应重新审查，不能自动扩张实现。

## Blocking Issues
**空（针对当前设计进入Codex实现）。**
已获取全部指定P3文档及相关模块，未发现缺少事实源、未确定policy owner或未解决的接口矛盾。历史shutdown/V01–V03已修复，有代码证据，不作为重复设计阻塞。
这不意味着Phase4实现或运行验收完成。T2/T3迁移中间分支不可发布；Linux build/关键integration真实通过是Phase4完成验收gate，后续失败会成为实施阻塞。同步后端非硬实时限制已明确接受，不能在实现时隐去。

## Deferred Issues
实际资源采集/完整Phase5、GPU/NPU、业务健康probe、动态配置、预算稳定窗口/reset、自动依赖组恢复、async进程后端/硬实时SLA、全局背压/整包事务快照、socket peer/session认证、持久历史、Runtime restart、device reboot、RK3588实机验证。

## 进入下一阶段的建议
可在把完整docs/P4放入目标仓库并核对HEAD差异后，按codex_package串行启动实现。当前没有创建chat、修改C++、执行构建或写远端；本包就是可review设计候选。
