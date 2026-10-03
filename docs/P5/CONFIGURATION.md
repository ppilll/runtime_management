# Configuration Design

## Repository constraints

当前load_file返回vector<ServiceConfig>，自带JSON parser只接受整数，root支持单服务对象和{"services":[...]}。没有全局RuntimeConfig。
P5新增RuntimeConfig{services,monitoring}与load_runtime_file，load_file作为wrapper返回services。不引入外部JSON依赖，不扩decimal parser，不把系统阈值塞进ServiceConfig。

## 最小global monitoring（7字段）

```json
{
  "services": [
    {
      "service_name": "idle",
      "executable": "/bin/true",
      "autostart": false
    }
  ],
  "monitoring": {
    "sample_interval_seconds": 2,
    "cpu_warning": 80,
    "cpu_clear": 75,
    "memory_warning": 80,
    "memory_clear": 75,
    "memory_critical": 95,
    "memory_critical_clear": 90
  }
}
```

示例是配置格式，不是生产业务service建议。旧单服务root可追加同级monitoring对象，仍保留原service字段；旧services root无monitoring时得到默认值。禁止新增裸array root支持，当前实际parser不支持。

JSON字段只接受整数（bool不能当int）；sample interval1..60s；0<=cpu_clear<cpu_warning<=100；0<=memory_clear<memory_warning<=memory_critical_clear<memory_critical<=100。
未给字段使用各自固定默认值后整体校验。若仅修改warning导致默认clear不合法，启动失败，错误指出冲突字段，不偷偷调整JSON clear。
monitoring若出现必须为object；null/string拒绝。新monitoring对象内未知字段拒绝（避免拼写错误），已有root/service未知字段处理不顺带收紧。service validation/recovery_timeout/1MiB限额/duplicate root keys原行为保留。

## Programmatic / 兼容 / precedence

- ResourceThresholds前三个已有字段及构造意义保留；尾部添加optional clear值或等价兼容构造参数，不能改变原aggregate三个值顺序。未指定legacy clear时派生cpu_clear=max(0,cpu_warning-5)，memory_clear=max(0,memory_warning-5)，memory_critical_clear=max(memory_warning,memory_critical-5)，使旧合法激活阈值仍可构造。
- 完整MonitoringConfig显式clear值必须按上表校验；programmatic百分比可double，但必须finite，JSON保持integer。
- 新RuntimeManager以RuntimeConfig为首参数的重载，默认native，使用该config完整monitoring；测试可显式external或注入窄reader。该入口不再同时接受第二份旧ResourceThresholds，消除谁覆盖谁的歧义。
- 原config_path + aggregation + sink + ResourceThresholds构造签名保留；delegates external，阈值来自明确的旧参数，interval为默认2s（external不自动采样）。它使用load_runtime_file统一服务校验，文件global monitoring不会覆盖旧参数；此legacy兼容入口不用于新生产采集。
- src/ipc/main.cpp解析一次RuntimeConfig，用新native重载构造Runtime，将同一services副本传IpcManager。CLI参数、socket、device_changes生命周期保持。
- config文件无monitoring也在新生产入口默认启用native；测试若需精确合成资源事实，必须选择external，不依赖真实host压力。

全配置启动加载一次。无reload、remote/dynamic云配置、每service系统阈值、enabled/process/thermal/disk字段或N次计数。process采集为P5固定能力；native/external是程序接线/测试选项，不是JSON远程控制。

## Validation requirements

旧single root/services、缺monitoring默认、7字段完整与部分、每个边界、反序threshold、wrong type/float/overflow/duplicate/unknown monitoring key均测试。保留原ServiceConfig及IPC GET_SERVICE_LIST字段，不把monitoring对象加入service list。
