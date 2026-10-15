# Risk Analysis

| 风险 / 实际证据 | 处理 / 剩余限制 |
| --- | --- |
| snapshot transientloss/boot会漏 | ownerpendingissue+ACK；P6-29/30；只有最严重摘要，非完整历史 |
| MCUboot_id可能碰撞 | peer_reboot只在不同validID有证据时；同ID重连合理降级，绝对每boot检测另ADR |
| IPC自报名称旧风险 | P6producer强SO_PEERCRED+SMlaunchedtoken+epoch/session+writer重验；legacy其他service风险不宣称已全修 |
| PID复用/启动登记race | accepted_at>=start_time、running+exactchildPID+token，早bind关conn重试；禁止daemon/fd delegation |
| stop/fault时token仍有效供reap | 不仅token，还核对RUNNING和sessionrevoked；不拿status.generation替producer token |
| report续租缓存过期 | serverreceived_at+3500ms、seq重复不续租；snapshotowner保持可信，lease只证明报告通道，不保证业务活性 |
| processloophealthy≠业务/执行器ready | 区分processrows、linkcontract；READY不证明remote_stopclear/controlvalid/actuatorsafe |
| managed移除fatalreadytimeout | 仅UART等待路径；product其它fatal保持；实际prod部署必须RK验证，harness不足 |
| B同进程耦合 | camera/RKNNfatal会同时失UART；本期接受，隔离A需新commandIPC/抽库ADR |
| IPC断连/满队列/ACK遗漏 | 有限pending/inflight/issue、disconnecttombstone和weakreply；ACK onlyAPPLIED；P6-10/28/30 |
| 全局queue/writer同步exec阻塞 | 新ingress有限但原全局queue无总cap，deadline仅writer能推进时生效；不承诺硬实时 |
| P5protectedchecker冲突 | CHANGE_LIST精确迁移旧oracle到新contract，保留owner/budget/DSM/原6s/覆盖；不能弱化检查 |
| host依赖误判 | fullVisionArm依赖vendorSDK；host专用harness直接canonicalUART/client，不伪完整product通过 |
| LinuxRS485配置缺失 | H01/RS485硬门；driver/device/电气未知，Runtime不接管transport |
| P4/P5无runtime证据 | P6实施前/集成门baselinebuild/全部旧CTest；失败是实施阻塞而非设计PASS |
| policy产品未选 | 两profile明确，部署必须选实际producer/required/criticality；不给service名字推断 |
| 新worker/join异常 | clientRAII、原信号/业务drain、projection短锁、不detach、不等待最后ACK阻塞退出 |
| 信息可信边界 | owner/root被攻陷无法靠同机UDS证明健康；不委派fd、目录权限、真实peer验证保证别的service不能自报冒充 |

新增clientCPU<1%、issue提交p95<300ms为目标，Host/RK实测，不是本包证明。暂无必须改MCUwire的证据；一旦必须改协议，停止对应Phase6实施并作为BlockingADR，不隐式纳入T4。
