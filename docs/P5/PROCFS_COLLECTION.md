# CPU / Memory / procfs Collection

## System CPU 固定公式

仅使用 /proc/stat 的 aggregate 行 cpu（不是 cpu0）；至少需要 user/nice/system/idle 四字段，现代附加 iowait/irq/softirq/steal 缺失按0。所有已出现的这八字段必须是合法非负 uint64，sum 做 overflow check。忽略 guest/guest_nice，因它们已包含在 user/nice，不重复相加；忽略后续不相关行。

total = user + nice + system + idle + iowait + irq + softirq + steal
idle_all = idle + iowait
busy_percent = 100 × (delta_total - delta_idle_all) / delta_total

busy 包含 user/nice/system/irq/softirq/steal；iowait 冻结为非忙，与 idle 合并。不建立 CPU accounting framework。此量是全系统 aggregate busy%，不是 load average、CPU MHz 或某核心利用率。steal 计入忙代表该预算不可供本机工作。

首次合法 counters：只建立 previous，cpu warming_up/value absent，不发 warning，也不发 CPU clear。
两次合法相邻 counters：每个已使用分量不回退、total delta>0、0<=idle delta<=total delta；转换到浮点后计算，避免先乘100溢出；结果须有限且0..100。fixture：total 1000→1200，idle_all 400→450，usage=75%。

counter regression（含 iowait 的 Linux 已知非单调可能）、wrap、字段集合不一致、sum overflow、非法 delta：本次 unavailable，不推进 CPU policy；若当前完整 counters 合法但不能与 previous 相减，以当前重建 baseline，下一次可恢复。parse/read failure 清 previous，下一次合法 sample warming_up；不猜 wrap modulus、不钳制坏值成100%。delta=0 同样不产生0%测量；采用重建 baseline 的 invalid-delta 路径。

热插拔造成 regression/形状变化按以上失效重建处理；P5 不输出 per-core 拓扑。不用 wall-clock 参与 CPU busy delta。

## System Memory 固定定义

只解析 /proc/meminfo 中 MemTotal、MemAvailable；允许其它行存在但不用于计算。
两字段都必须唯一、整数非负、单位为 kB（Linux procfs 此单位=1024 bytes），乘1024前检查 uint64 溢出。
MemTotal>0、0<=MemAvailable<=MemTotal。used_percent = 100 × (MemTotal-MemAvailable)/MemTotal。
例：total=1000 kB、available=200 kB，used=80%，不使用 MemFree，避免把可回收 page cache 全算作压力。

缺 MemAvailable：unavailable，不采用 MemFree/缓存估算法 fallback；目标 Linux 是否提供它留待 host/RK3588 验证。读失败、重复字段、非法单位/负值/数字溢出/total=0/available>total：unavailable，不能触发 memory clear。available=0 是合法100%，total=available是合法0%。

## 最小读与 parser seam

reader 只访问生产固定 /proc/stat、/proc/meminfo、以及 SM 提供正整数 PID 拼接的 /proc/PID/stat，不收 IPC 任意路径。测试 reader 用内存 path→text fixture/错误，不写真实 /proc，不全文件系统抽象。

读取限额：stat/meminfo 每文件64KiB；进程 stat4KiB，超过则 unavailable。只保存当次文本，及时释放。read/parse 预期失败作为结果，unexpected exception 不能泄漏穿出 std::thread 导致 terminate：worker 边界捕获、记录 fatal、请求已有 shutdown。

本文件公式与 THRESHOLD_POLICY 分离：parser/collector 不引用 ResourceSeverity、DSM 或 RecoveryManager。
