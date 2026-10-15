# Static Configuration Ownership

Supervisor仅管理service executable/arguments/environment/dependency/timeouts/restart_policy，以及管理层health policy/producer授权；UART path/baud/termios/RS485/HELLO/wireheartbeat/watchdog属于VisionArm原CLI/UartLinkConfig，不能塞进Runtime ServiceConfig。

## 唯一 policy schema

复用已有ServiceHealthPolicy与ServiceCriticality。为避免config→aggregation→DSM的include耦合，可将这两个原类型移入include/runtime/health_policy.hpp，由原aggregation头重新include保留名字；不新增第二criticality enum。RuntimeConfig尾部新增HealthConfig，不改旧ServiceConfig字段及P5monitoring。
```json
{
  "services": [
    {
      "service_name": "control_service",
      "executable": "/opt/visionarm/visionarm_runtime",
      "arguments": ["--control-backend","uart","--uart-device","/dev/ttyS3","--uart-baud","115200","--runtime-service-socket","/run/runtime/service.sock","--runtime-service-name","control_service"],
      "autostart": true,
      "heartbeat_timeout": 15,
      "restart_policy": "on-failure"
    }
  ],
  "health": {
    "services": [
      {"service_name":"control_service","required":true,"criticality":"HIGH"}
    ],
    "components": [
      {"component":"mcu_link","producer_service":"control_service","required":true,"criticality":"HIGH"}
    ]
  }
}
```
这是schema与机器人profile示例，非可直接运行的部署配置：实际VisionArm需要原camera/model/output/width等CLI；补全已部署参数，路径/别名/socket与现有main启动参数一致，不能按示例猜测真实设备。

health不存在：空componentregistry，保持既有service默认/override和旧IPC。启用mcu_link时必须显式给producer_service、required、criticality，没有“名为mcu_service默认HIGH”的暗规则。纯视觉profile建议components=[]；监控非必需链路可required=false/LOW。required/HIGH推荐用于机器人，实际部署选择见freeze Unresolved，不影响schema。
services health overrides可缺省为空；显式项覆盖同service原名字默认。只有一份有效ServiceHealthPolicy。完整RuntimeConfig与AggregationOptions同时给同service不同override应拒绝配置，不能静默last-wins；同值合并、仅一方给值使用它，legacyprogrammatic入口保留。

## 校验与边界

- health必须object，只允许services/components；子array及各item字段严格；bool不是int，enum只HIGH/MEDIUM/LOW；不加decimal parser。
- 未知service/producer、duplicate service policy/key、缺必填、null/类型/未知字段、emptykey、非法enum拒绝；本期只允许component=mcu_link及至多一个entry，一个静态producer。
- producer必须现有受管前台直接child、autostart=true，heartbeat_timeout>=4s；启动图/依赖校验保持原逻辑。component gate不增加SM dependency边。
- health services policies最多32 entries且均命中registry；component0..1；report/lease固定1000/3500ms，本期不做新的JSON timers。旧single-service root同级health也支持；旧services root/monitoring/1MiB/重复key校验保持。
- ConfigManager.load_file wrapper仍只返回services，但必须校验全RuntimeConfig；新main一次加载并向aggregation/IPC传同静态health配置，不再次读文件。
- GET_SERVICE_LIST不回传UART/health根对象。旧配置不因新required项改变readiness。
- 新VisionArm managed参数需配对且仅UARTbackend；service_name仅声明目标，身份仍serverpeer判定。
- Runtime不open /dev/tty*、tcsetattr、TIOCSRS485；将来LinuxRS485支持只在ownerSerialPort/部署driver，当前缺失见硬件门。

全配置启动加载，无reload/cloud动态策略。UART本地超时不能由Runtime health配置反向覆盖。
