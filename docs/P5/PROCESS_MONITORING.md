# Managed Process Monitoring

## 范围与事实边界

只观察 SM 管理的前台直接子进程，不追踪后代/进程组，不扫描全 /proc。
PID existence、RSS、CPU 是测量；wait/reap/退出码与 ServiceState 是生命周期；heartbeat 是业务存活信号。三者不能互相替代。

stat 行存在不保证服务 healthy；zombie 仍有 proc entry，但进程已不可运行。
采集 ENOENT 不发 service_failed，不调用 waitpid/kill/checkProcessAlive 的生命周期动作；真正退出仍由原 Supervisor.reap→Runtime→SM 路径处理。Heartbeat watch/check 不重新实现。

## Identity 与线程竞态

身份为(service_name,pid,launched_generation,proc_start_time_ticks)。snapshot 的 instance_generation 明确复用 launched_generation，不复用 status.generation 或 recovery_generation，也不新增 generation owner。

采集步骤：
1. 使用 SM 的批量 trySnapshotProcessIdentities，try_lock 失败则本轮 process_scan unavailable；system 采集与 heartbeat 不因此等待同步 exec。
2. 锁外读取每个 /proc/PID/stat，确认行 PID 等于 captured PID；解析 starttime/state/counters/rss。
3. 用 SM 的批量 tryValidateProcessIdentities 重验证(name,pid,launched_generation)。失败/锁忙：丢弃本轮 process results 与将要提交的 previous 更新，不能发布未验证数据。
4. 对比已缓存该 launch 的 starttime；不同则 identity_changed、清 previous，本轮不接纳替代进程数值。只有新的 SM launch token 可建立新身份。
5. 成功且身份稳定后才提交过程 snapshot/previous。缓存只留当前 eligible launch；停止或重启淘汰旧条目。

两次 owner 查询之间身份可以变化，不能假装事务一致性；查询返回 row 时仍携带捕获 token，使用方不得当作当前 lifecycle 状态。SM 独占 reap 和前台服务契约确保未回收 child PID 不复用；第一次采样在该契约下建立 starttime 锚点。外部 reaper/SA_NOCLDWAIT/daemonizing 不支持，不承诺与任意外部 PID 操作竞争的绝对身份证明。collector 不接管 pidfd 或进程后端。

新 try 快照接口只复制/比对，不改变 registry，不新增 PID 数据源。失败是可预期 skip，不能因等待已有 launch 锁阻塞 Monitor worker。现有 blocking listServices/all_statuses 继续供原调用方使用。

## /proc/PID/stat 解析

开头 PID；comm 在括号中且可能含空格和右括号，使用最后一个右括号找到真正 comm 结束，再按固定字段偏移解析。测试必须包含空格/括号组合，禁止简单按整行空格 token 定位。
需要 field3 state、field14 utime、field15 stime、field22 starttime、field24 rss（signed pages）；不累加 cutime/cstime，避免子进程资源归属混淆。
拒绝不足字段、非数字、负 CPU/starttime、负 RSS、乘页大小溢出、PID mismatch。Z 状态标记 zombie，不提供本轮 CPU/RSS为活跃值。其它状态只是观测，D/S/R 都不能由 collector 判服务故障。

RSS = rss_pages × sysconf(_SC_PAGESIZE)。页大小启动读取一次并验证>0；不可硬编码4KiB，ARM64可能不同。
CPU ticks = utime + stime；使用实际 process sample 时刻 elapsed_seconds。
process_cpu_percent = 100 × delta_ticks / (sysconf(_SC_CLK_TCK) × elapsed_seconds)。
单核等价100%，多线程可>100%；不是 system aggregate%。运行时 CLK_TCK 启动读取一次并验证>0，不能硬编码100。

首次 same identity 为 warming_up；RSS可独立有效。负 delta/elapsed<=0、读取或身份失败不生成CPU值。间隔超过3×配置interval时清旧baseline，新合法观测重新 warming_up；正常略晚采样按真实elapsed计算，不强用2s。
read失败清该进程 previous；not_present 不能继承旧rss。全扫描锁忙时 process_scan unavailable，本轮不改 old previous；再次成功按真实elapsed或上述长间隔规则处理。

不为 Process CPU/RSS 设置 WARNING/CRITICAL，不基于它们 restart。它们只供 queryResourceSnapshot、调试与测试使用。

每个launch的starttime身份锚点与上一CPU计数分开保存：read失败或identity_changed只清CPU previous，已验证锚点保留到SM token/PID变更或退役。不得清锚点后在同launch下一轮接纳已复用PID；P5-16须覆盖重复mismatch。sysconf(HZ/pagesize)失败只让process采集unavailable，system CPU/Memory继续，记录节流诊断。
