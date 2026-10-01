# Phase1 IPC Protocol


# 1. Purpose


定义Phase1 Runtime与外部控制端、Service之间的最小通信协议。


IPC只负责Runtime管理。


禁止传输业务数据。



---

# 2. Transport


使用：

Unix Domain Socket


Socket Type:

SOCK_STREAM



---

# 3. IPC Channels


Phase1存在两个逻辑通道。


---

## 3.1 Control Channel


方向：


test_client

↓

runtime_manager



用途：


START

STOP

QUERY_STATUS



---

## 3.2 Service Channel


连接：fake_service连接runtime_manager的Service Socket。

消息方向：HEARTBEAT由fake_service发送给runtime_manager；EVENT由runtime_manager发送给fake_service。



---

# 4. Message Frame


Unix Stream Socket必须进行Frame封装。


格式：

Header
message_length
message_type
request_id
Payload
JSON UTF-8


字段：


message_length:

uint32

表示Payload的字节数，不包含10字节Header。


message_type:

uint16


request_id:

uint32



Encoding:

Little Endian

Header固定10字节，字段顺序为`message_length`、`message_type`、`request_id`。Payload是UTF-8 JSON。SOCK_STREAM允许拆包、合包；接收端按长度组帧。Phase1实现限制Payload最大64 KiB。



---

# 5. Message Type


|Type|Value|
|-|-|
|START|1|
|STOP|2|
|QUERY_STATUS|3|
|HEARTBEAT|4|
|EVENT|5|
|ERROR|255|



---

# 6. Request / Response


Request:

client

↓

runtime_manager


Response:

runtime_manager

↓

client



request_id必须保持一致。

START、STOP、QUERY_STATUS成功响应使用与请求相同的`message_type`。错误响应使用ERROR类型，并保留请求的`request_id`。HEARTBEAT是单向消息，无响应。



---

# 7. START


Request:

```json
{
"service_name":"fake_service"
}
```

Response:
```json
{
"result":"OK",
"state":"STARTING"
}
```

## 8. STOP

Request:
```json
{
"service_name":"fake_service"
}
```

Response:
```json
{
"result":"OK"
}
```

## 9. QUERY_STATUS

Request:
```json
{
"service_name":"fake_service"
}
```

Response:
```json
{
"service_name":"fake_service",
"state":"RUNNING",
"heartbeat_time":123456,
"restart_count":0
}
```

## 10. HEARTBEAT

Direction:
fake_service
↓
runtime_manager
Payload:
```json
{
"service_name":"fake_service",
"timestamp":123456
}
```

Runtime以接收时间更新心跳；Payload中的`timestamp`不决定健康判定。

## 11. EVENT

EVENT只表示：
Runtime发送给Service的异步通知。
例如：
SERVICE_STOP
STOP命令触发已通过HEARTBEAT标识该Service的连接收到EVENT。通知不与控制请求配对，`request_id`为0。
fake_service按相同帧格式接收`SERVICE_STOP`并退出；Runtime同时使用进程信号保证停止流程。

```json
{"event":"SERVICE_STOP","service_name":"fake_service"}
```
禁止用于Runtime内部事件。
Runtime内部事件使用：
Event Queue。

## 12. Error

统一错误：
```json
{
"result":"ERROR",
"code":1001,
"message":"invalid service"
}
```

## 13. Phase1 Limit
禁止：
- video data
- image data
- AI result
- MCU message
