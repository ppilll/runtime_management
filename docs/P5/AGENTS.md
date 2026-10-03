# Phase5 Local Agent Rules

- 当前设计包不授权实现；只有后续明确执行某个docs/P5/codex_package任务时才修改C++。每任务显式读取docs/P5/AGENTS.md、README、CHANGE_LIST及对应设计。
- P5只做Linux CPU/Memory/受管直接子进程资源、迟滞health facts、静态global config、memory-only快照。严格按任务Allowed Files；头文件include/runtime，资源实现src/monitor，测试tests，文档docs/P5。
- Collector只测量；Monitor唯一资源policy；SM唯一PID/lifecycle/launched_generation owner；RM唯一recovery policy；DSM唯一DeviceState owner。资源不得反推任意service restart。
- CPU aggregate delta：idle+iowait非忙，guest不重复；首次/无效sample不能当0%。Memory只用MemTotal/MemAvailable，缺失unknown，不fallback MemFree。
- process身份使用name/PID/launched_generation/starttime；批量try身份快照/重验证，锁外proc I/O。不得wait/kill/reap或新增generation owner。
- 默认2s静态1..60s，复用Monitor worker，先heartbeat，无新增resource/per-service thread。旧external与新native互斥，保留source不能被外部覆盖。
- CPU onlyWARNING；Memory WARNING/CRITICAL迟滞，clear同source后全量重聚合，不直接set RUNNING。invalid不clear已有latch，不直接设ERROR。
- 保持原UDS10字节/64KiB/types1..10/255、DSM表、P4恢复/预算/代次、backend、Logger、原六秒shutdown函数及所有旧测试目标。
- 禁止GPU/NPU/RKNN业务、thermal/disk本轮实现、新IPC/HTTP/gRPC、cloud/MQTT/DB/history、动态配置、metrics framework/复杂rule engine/one-thread-per-service。
- Linux构建：cmake -S . -B build -DBUILD_TESTING=ON；cmake --build build；ctest --test-dir build --output-on-failure。CTest默认串行。ARM64用部署已有toolchain，不在host执行ARM64 binary。
- 静态：python3 -B tests/phase3_validation_static_check.py、tests/phase4_static_check.py、实施后tests/phase5_static_check.py；git diff --check。旧checker仅按CHANGE_LIST迁移批准语义，不能删owner/terminal/DSM/IPC/6s检查或DISABLED/WILL_FAIL。
- 报告分别列Implemented、Statically Verified、Host Linux Runtime Verified、RK3588 Runtime Verified、Not Verified；测试源码存在不写PASS，x86/cross-build不写RK3588 verified。

自动作用域仅docs/P5；include/src/tests实施任务必须显式读取这些约束。根AGENTS.md为同一P5规则的仓库入口，不添加项目历史或通用C++规范。
