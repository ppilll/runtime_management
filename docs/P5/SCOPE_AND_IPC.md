# Temperature / Disk / IPC Scope Decisions

## Temperature：A 延期

A：不实现temperature，仅保证collector可添加新typed观测。优点：不依赖未提供的BSP契约，测试/维护范围小；缺点：少一个硬件演示指标。
B：generic thermal zone discovery，读取各type/temp。优点：比固定thermal_zone0可移植，有板端价值；缺点：本仓库没有稳定zone types、sensor映射、单位/范围与health阈值契约，需RK3588 BSP fixtures和多zone测试。

推荐A。当前没有实际thermal样本或目标BSP接口证据，不能为了完整硬编码Rockchip路径。P5不创建空temperature字段、虚构sensor表或未使用虚拟接口；collector与policy已分离即保留扩展点。后续B必须按zone type发现，定义多个同type与missing/invalid处理，再加discovery/missing/invalid/multizone tests。

## Disk：延期

A：不做capacity。开发最快、避免虚拟fs遍历，符合当前CPU/Memory核心；缺点：不能发现数据分区即将满。
B：只监控产品指定mount point，使用statvfs。优点：可识别实际存储风险；缺点：rootfs/data目标、reserved blocks与阈值均无产品证据。
推荐A。当前未冻结应监控哪个mount；禁止默认扫描所有filesystem。未来需要明确单个/小集合mount及readonly/missing语义。

## IPC：A 不新增

当前GET_DEVICE_STATE/GET_HEALTH和SUBSCRIBE_EVENT(TYPE10)只覆盖设备状态与heartbeat派生健康，不能宣称已有resource数值查询或内部resource原始广播。
A：维持wire，资源通过健康变化和日志体现，process数值通过内部queryResourceSnapshot调试/测试。优点：协议/兼容成本最低，当前无真实客户端需求；缺点：外部客户端不能查询CPU/RSS数字。
B：最小GET_RESOURCE_USAGE（UDS request/response）返回current typed snapshot。优点：调试/设备管理/demo可展示数字；缺点：必须冻结type、payload/null/age/多进程64KiB bounds、错误与兼容，并增加IPC tests。

推荐A。演示价值本身不足以要求新协议；P5目标是health facts，不增type11。GET_HEALTH可能在RUNNING且未首心跳时UNKNOWN，保持原定义，不借资源采集改它。未来B是独立批准范围，仍UDS，不HTTP/REST/gRPC、不resource streaming subscription。
