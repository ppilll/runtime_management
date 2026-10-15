# Phase6 Local Agent Rules

- 本设计交付不授权实现；仅后续用户明确执行对应docs/P6/codex_package任务才修改源码。先读根/P6规则、README、CHANGE_LIST、对应task和P4/P5owner文档，严格Allowed Files。
- Runtime不创建UartLink/SerialPort，不open tty/tcsetattr/TIOCSRS485，不复制VisionArm frame/escape/CRC/messages/HELLO/STATUS/ACK/sequence/reopen或MCUprotocol；仅normalizedfacts。
- B方案真实owner是VisionArm visionarm_runtime，不能按control_service/mcu_service名字推断独立程序。T2仅projection/client/CLI/managedUART接线，不改camera/RKNN/control业务。
- processheartbeat必须独立MCUwire/linkhealth；loss不停止processheartbeat，不生成FailureType/RecoveryRequest、不主动exit/restart，不增加第二linkwatchdog/retryowner。
- RM唯一processpolicy；SM唯一PID/ServiceState/launched_generation；DSM唯一DeviceState；Collector测量/Monitor资源policy。componentHEALTHY只清本component并全量evaluate；原OFFLINEterminal不解锁。
- producer认证用SO_PEERCRED exactforegroundchildPID+SMlaunched_generation+epoch/session+writer提交重验；不用timestamp/自报generation/status.generation。P6producertype4不能绕绑定；禁止daemon/wrapper/fddelegation/externalreaper。
- 五healthstatus、一个component_health_changed、type11bind/report；10-byte/64KiB/旧编号/原EVENT方向不变。新payload<=4KiB、privatequeue<=32；ACK只在writer应用后，lease是reportfreshness非wiretimeout。
- ownercoherentprojection+pendingissue/AckThrough不丢瞬态，网络不在UART锁/线程；metrics留owner。bootid可能碰撞，同ID不能声称未reboot。
- required/criticality复用ServiceHealthPolicy，启用component必须显式producer/policy，不按名字推断；旧配置无component。UARTconfig仍由owner负责。
- 跨repo：Runtime仅CHANGE_LIST指定model/event/aggregation/IPC/config/接线/test/docs；VisionArm仅列明路径；MCUproduction冻结。需要wire变化立即停止并列BlockingADR。
- 同版docs/P6复制VisionArm供T2读，跨repo仅schema/JSONfixtures，不复制业务/协议源。T1锁定后T2/T3分repo可并行，T4等双方；sharedheader/CMake/checker修改串行。
- Runtime Linux：cmake -S . -B build -DBUILD_TESTING=ON；cmake --build build；ctest --test-dir build --output-on-failure。原12executables/13CTest及原6s/PID/ECHILD保留，默认串行。
- Static：python3 -B tests/phase3_validation_static_check.py、phase4_static_check.py、phase5_static_check.py、实施后phase6_static_check.py；git diff --check。仅CHANGE_LIST批准迁移，不删owner/budget/DSM/IPC/旧oracle，禁止DISABLED/WILL_FAIL。
- VisionArmproduct依visionram/build.sh/README和实际SDKoverrides；hostharness创建后用tools/test_apps/runtime_supervision独立CMake；PTY/交叉编译不替代RK，不在host执行ARMbinary，不虚构toolchain。
- shutdown gate后不提交componentstate；原RMcancel/grace/reap与VisionArm业务drain/join保留；不detach、不无限等ACK退出。
- 分报Implemented、Statically Verified、Host Linux Runtime Verified、RK3588 + MCU Hardware Verified、Not Verified。源码存在不写PASS，open成功不写RS485电气PASS。
- 禁止newMCUfirmware/newUARTprotocol、Camera/V4L2/RKNN/image pipeline/control/motor/actuator、cloud/MQTT/DB/web/OTA/GPU/NPU/genericHAL/telemetryframework扩展。
