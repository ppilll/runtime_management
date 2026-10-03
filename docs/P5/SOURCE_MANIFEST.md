# Source Manifest

Repository: ppilll/runtime_management
Pinned ref: 4843061fce2a329832105d34aaea380a35560c0e

通过已连接GitHub的branch/tree/fetch_file读取；tree未截断。以下120个文件完整取回到审查缓存；blob SHA是GitHub报告的原文件标识，不是转换后本地缓存字节的hash。

完整取回不代表每个历史文档逐句复审，也不代表运行了测试。重点审阅范围见REPOSITORY_ASSESSMENT。完成前再次通过GitHub确认master未变化；当前包未写远端。

## 重点事实定位

| 文件 | 实际审阅关键内容 |
| --- | --- |
| docs/P0 两份架构文档 | Supervisor/进程/UDS/资源与业务边界 |
| docs/P1 runtime_core_contract / architecture_review / ipc_protocol | writer/threads/config/帧，历史与当前语义差异 |
| docs/P2 process_backend / ipc_config / recovery_dependency 文档 | 独占reap、pidfd/WNOWAIT、root/parser、旧owner记录 |
| docs/P3 MONITOR_INTEGRATION / AGGREGATION_RULE / EVENT_MODEL / review_fixes | 外部输入、source/severity/clear、全量聚合、代次 |
| docs/P4 ARCHITECTURE / DECISIONS / DESIGN_FREEZE_CANDIDATE / validation_review / review_fixes | RM/SM/DSM owner、仅静态验证、F01–F03 |
| include/runtime 与 src/monitor/runtime/service/config/ipc | 当前实际路径/线程/身份/阈值/loader/queue/恢复边界 |
| tests/CMakeLists与资源/恢复/生命周期/IPC测试源码、静态脚本 | 旧断言、10 executable/11 CTest、6s回归、main字节保护 |

## 完整取回清单

| 路径 | GitHub blob SHA | bytes（GitHub元数据） |
| --- | --- | --- |
| [CMakeLists.txt](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/CMakeLists.txt) | 0040296f45440fe5da2d19f0b541cc851050fba0 | 1314 |
| [docs/P0/RK3588_Phase0_Final_Design_Result.txt](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P0/RK3588_Phase0_Final_Design_Result.txt) | c2c5df92128d1ed8dfac8a0a07731254e42321a4 | 2317 |
| [docs/P0/RK3588_Runtime_Management_Phase0_Architecture_Design.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P0/RK3588_Runtime_Management_Phase0_Architecture_Design.md) | 20fca743575f17b8132ac14c74fdeaa31afc5c83 | 5602 |
| [docs/P1/architecture_review.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/architecture_review.md) | 8c4cf19f66eca8142b788fc823083c4d66fe3871 | 6050 |
| [docs/P1/coding_rules.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/coding_rules.md) | 78fb7bac65448c3851af745419bedaafd45b62c6 | 1121 |
| [docs/P1/ipc_protocol.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/ipc_protocol.md) | bce3136ff7049d0222eb57e844928c19c9cd59db | 2988 |
| [docs/P1/module_design.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/module_design.md) | d4ecfdc3d76833f63c29b6487805f2d0842476e0 | 2453 |
| [docs/P1/phase1_architecture.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/phase1_architecture.md) | 3f8e26913b083d9233e286c76d071cc14f97a4f0 | 4356 |
| [docs/P1/review_report.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/review_report.md) | 48c54586cde00b0ea6fea467553c43e0bdaa9357 | 4513 |
| [docs/P1/runtime_core_contract.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/runtime_core_contract.md) | aebbcbcbdcc096077cce9a5974416dc1fae2c330 | 3797 |
| [docs/P1/test_strategy.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/test_strategy.md) | a007f6c115008104ba0f9360236e05ef437915be | 1647 |
| [docs/P1/thread_plan.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P1/thread_plan.md) | 2be57bf29b1f439b3c77d21f5d1d8b0e5cb45045 | 2545 |
| [docs/P2/architecture.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/architecture.md) | 7f843adeb04a393f1488bfec899f227d135c1f68 | 2319 |
| [docs/P2/codex/thread1_service_manager_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/codex/thread1_service_manager_prompt.md) | 0b1fb0d9b686ee81aeb25b4289dd3a8b4801a3b5 | 1078 |
| [docs/P2/codex/thread2_process_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/codex/thread2_process_prompt.md) | 45417e6d3980d36c915731acdd482ea58fef230b | 771 |
| [docs/P2/codex/thread3_recovery_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/codex/thread3_recovery_prompt.md) | 76e24a6c651f5bcebec9387a228176537080d105 | 746 |
| [docs/P2/codex/thread4_ipc_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/codex/thread4_ipc_prompt.md) | 4c2aa99c8c5d22779ebf3cd361b3287ae776f7af | 544 |
| [docs/P2/codex/thread5_test_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/codex/thread5_test_prompt.md) | 1915f240ccd6582b001a660d96e7b2d52c38e61d | 569 |
| [docs/P2/implementation_plan.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/implementation_plan.md) | 6f0295dca4f1659689b790f8f4565c6778765577 | 854 |
| [docs/P2/ipc_config_extension.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/ipc_config_extension.md) | 54a588f4e61cf5688f4b923db860dd188ab7613b | 4725 |
| [docs/P2/ipc_config_implementation.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/ipc_config_implementation.md) | 0dec4c1bcf3c4b527be2e2b0307eda76e6cc00d6 | 3948 |
| [docs/P2/process_backend_implementation.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/process_backend_implementation.md) | c93c688e3c6963ebb0b4cb6149b90d10f0ab0a42 | 5212 |
| [docs/P2/process_lifecycle_design.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/process_lifecycle_design.md) | a8949eda5a2c40576a96c292a76059765615a037 | 1023 |
| [docs/P2/README.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/README.md) | 1b1a7a54101a54dc03045b06162661c407cd633a | 1485 |
| [docs/P2/recovery_dependency_design.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/recovery_dependency_design.md) | bec96607e226184f0592a0d28b85b69e0be29989 | 3462 |
| [docs/P2/recovery_dependency_implementation.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/recovery_dependency_implementation.md) | 21ad4e33235d343c49b1265ae16428af2a0da5a0 | 4481 |
| [docs/P2/testing_strategy.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P2/testing_strategy.md) | c3c9858209c17c05fea016d8a50e089db04c5b6f | 861 |
| [docs/P3/AGENTS.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/AGENTS.md) | 09e0cca47f2ced6f582c344d8e54fce88a894077 | 878 |
| [docs/P3/AGGREGATION_RULE.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/AGGREGATION_RULE.md) | 49001011e77f8009d85585581fc2f764d1c0bf08 | 4695 |
| [docs/P3/ARCHITECTURE.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/ARCHITECTURE.md) | 6161abc3dbc69a794799959a3aa958897d24f59e | 928 |
| [docs/P3/CODE_REVIEW_CHECKLIST.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/CODE_REVIEW_CHECKLIST.md) | 0c90f8ba26c6b8cf82e1c3855f6de45ba42f85e0 | 1723 |
| [docs/P3/CODEX_DEPENDENCY.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/CODEX_DEPENDENCY.md) | 2c2926d854a71ba0106203a5f23f833ad07a9171 | 512 |
| [docs/P3/codex/thread1_state_manager_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/codex/thread1_state_manager_prompt.md) | 91a5ec3414f7395cf2e22eabed60cb2596fc3b51 | 2059 |
| [docs/P3/codex/thread2_event_aggregation_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/codex/thread2_event_aggregation_prompt.md) | 7faf3d3c0f8a0343b57d1ab1fcb4acc699e8b8fb | 1837 |
| [docs/P3/codex/thread3_ipc_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/codex/thread3_ipc_prompt.md) | 32b5f82599ad475db16417ffe4d407004e5b284d | 1340 |
| [docs/P3/codex/thread4_test_prompt.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/codex/thread4_test_prompt.md) | e1aebf1fbd9056765abe0c0060abe9ca8ed5fe42 | 993 |
| [docs/P3/device_state_manager_implementation.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/device_state_manager_implementation.md) | e98256e2bee3e4687132589106c8b1e396a5fa68 | 7859 |
| [docs/P3/DEVICE_STATE_MODEL.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/DEVICE_STATE_MODEL.md) | d1e66c3d94680612c1dfd374361e339d4d4c8381 | 1190 |
| [docs/P3/event_aggregation_implementation.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/event_aggregation_implementation.md) | b0d9f0215a90c0cded1c21823f7a74a141ab506e | 9713 |
| [docs/P3/EVENT_MODEL.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/EVENT_MODEL.md) | 59681e793b94989ec6ac63c4220c32d9c68cf008 | 4356 |
| [docs/P3/ipc_extension_implementation.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/ipc_extension_implementation.md) | 1741fc0df060ee064c711a1860920a892414feb4 | 4206 |
| [docs/P3/IPC_EXTENSION.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/IPC_EXTENSION.md) | bb9d437253fec02ba74250920428f56d1f9c6cfc | 5491 |
| [docs/P3/MONITOR_INTEGRATION.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/MONITOR_INTEGRATION.md) | bfb0d23885c46a1582d9df1baea43fdb9e9516d2 | 1786 |
| [docs/P3/PERSISTENCE_DESIGN.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/PERSISTENCE_DESIGN.md) | d9255e872f15420dd3135b2af77526744733c28a | 501 |
| [docs/P3/phase4/RECOVERY_STRATEGY_INTERFACE.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/phase4/RECOVERY_STRATEGY_INTERFACE.md) | 86648b586d6e1ef163ca694a5cc527b8672153e7 | 1396 |
| [docs/P3/README.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/README.md) | 8b6028a19a9e3565d69b9e8c18e1bceec837eaf8 | 1233 |
| [docs/P3/review_fixes.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/review_fixes.md) | b703917bc1f2e91749ad14a5a2900d9353261527 | 7542 |
| [docs/P3/STATE_TRANSITION.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/STATE_TRANSITION.md) | eab56ab5769b4e23092f08437448b3812543a0ce | 1015 |
| [docs/P3/validation_review.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P3/validation_review.md) | 6e28a64b1ae9e424258845a5ae30dcb4490dd891 | 15780 |
| [docs/P4/AGENTS.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/AGENTS.md) | 1242ae3a7561d51306c3c7f4016dd820ce874849 | 2185 |
| [docs/P4/ARCHITECTURE.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/ARCHITECTURE.md) | 65fb41677a20d7e7fe7444df5a9faffa7b91f91b | 5081 |
| [docs/P4/CHANGE_LIST.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/CHANGE_LIST.md) | a48c3e3a942058641c3756ae47402fbfa08cc92e | 5433 |
| [docs/P4/DECISIONS.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/DECISIONS.md) | 414e1552983daf3d985c2f827589d961dc9e9991 | 7683 |
| [docs/P4/DESIGN_FREEZE_CANDIDATE.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/DESIGN_FREEZE_CANDIDATE.md) | 46ae247d44c3e01389f48fde627eb7c44b70e305 | 3531 |
| [docs/P4/DEVICE_STATE_INTEGRATION.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/DEVICE_STATE_INTEGRATION.md) | ad6b6a7933e160dc705c551695e1bdd104ef52a5 | 3837 |
| [docs/P4/implementation_thread1.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/implementation_thread1.md) | d3b4d482e697b78df9269d9fe186ee2323f05dd7 | 12691 |
| [docs/P4/implementation_thread2.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/implementation_thread2.md) | f98b9d43823f8a490efcfb1b1f61cd21169890c8 | 11743 |
| [docs/P4/implementation_thread3.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/implementation_thread3.md) | d7f0507a70ba7a30cb9d251f5694ab97a594905c | 11128 |
| [docs/P4/PACKAGE_VALIDATION.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/PACKAGE_VALIDATION.md) | d77a3d5eace07d7c5e3cdf2ac98d1798980b8182 | 2079 |
| [docs/P4/README.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/README.md) | e0a383f5b8a699159774387a0ef421e4ad95bd14 | 3556 |
| [docs/P4/RECOVERY_EVENT_FLOW.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/RECOVERY_EVENT_FLOW.md) | f31b8b9050220bfe39c536f4fc3f72edb01f0855 | 5221 |
| [docs/P4/RECOVERY_GENERATION.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/RECOVERY_GENERATION.md) | 49c637178be12884e9eb868a85c6ce2bae5bac79 | 5915 |
| [docs/P4/RECOVERY_MODEL.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/RECOVERY_MODEL.md) | 0c856ec9da78c44d2beaf147bf418ad9f26c2fd9 | 6133 |
| [docs/P4/RECOVERY_POLICY.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/RECOVERY_POLICY.md) | 460ebb6f1ffe1a136b3cd7e8d35c298e388fd3db | 5826 |
| [docs/P4/REPOSITORY_ASSESSMENT.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/REPOSITORY_ASSESSMENT.md) | 4ab22d22f0fe16e21b6df3b027c0b913ddc1fd87 | 8186 |
| [docs/P4/review_fixes.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/review_fixes.md) | 1738ba853161f27084755c352f2f082f313a346a | 4611 |
| [docs/P4/RISK_ANALYSIS.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/RISK_ANALYSIS.md) | 7ab20105efed09349e05814ea3c34f91a0e47b17 | 4068 |
| [docs/P4/SERVICE_INTEGRATION.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/SERVICE_INTEGRATION.md) | 6f21f106cb48271d293366e95c0036c3e73f67e0 | 6435 |
| [docs/P4/SOURCE_MANIFEST.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/SOURCE_MANIFEST.md) | 0266ef5d58e900978f852c04006644499665be36 | 21002 |
| [docs/P4/static_validation.log](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/static_validation.log) | b2f4225d5a7a17dda3455a41afcdda90b1eb258b | 5589 |
| [docs/P4/TEST_PLAN.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/TEST_PLAN.md) | 7b8d21c5249c3e89bf02ab42f7d137c29c77933d | 10125 |
| [docs/P4/thread2_static_check.py](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/thread2_static_check.py) | 730fa22836a35ad6859cc689447ba6d948e140c0 | 4981 |
| [docs/P4/thread3_static_check.py](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/thread3_static_check.py) | 1bf9547305d70dcddb76fd31b5c1acd601aaca8a | 10240 |
| [docs/P4/validation_review.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/docs/P4/validation_review.md) | 3e149bad2604847ddeee640e91fcab81e3549638 | 21749 |
| [include/runtime/config_manager.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/config_manager.hpp) | cc4a77efa7a166bb885a63b3d50a18f11b931e03 | 1132 |
| [include/runtime/device_state_manager.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/device_state_manager.hpp) | a9bd63e718755a799da19d64a45fad0de61440a4 | 1266 |
| [include/runtime/device_state.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/device_state.hpp) | 1aae629539ed8fa46cd815928fb1bbffcbdbdd4c | 1460 |
| [include/runtime/event.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/event.hpp) | bf2ddcc69b9857ec88d3574736c30e24932679cd | 3035 |
| [include/runtime/logger.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/logger.hpp) | f753e5038ceeaf991a95f607f1951a99fd89da61 | 774 |
| [include/runtime/monitor.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/monitor.hpp) | 96835161898ae177ec6d8a848983583965311eb4 | 1562 |
| [include/runtime/process_supervisor.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/process_supervisor.hpp) | ebbd1132db541569a03ad82a5db8dfa624e88ba9 | 1890 |
| [include/runtime/recovery_manager.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/recovery_manager.hpp) | fa21a93591f8439bc9ba8965e0e4d2681230147e | 4939 |
| [include/runtime/recovery.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/recovery.hpp) | 746f39efc1ee091b488b27b0c9b656ae9f80b212 | 5864 |
| [include/runtime/runtime_manager.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/runtime_manager.hpp) | ea9f9ee8074f6b380ced2cfad8cd55d80fb66183 | 2408 |
| [include/runtime/service_aggregation.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/service_aggregation.hpp) | d6c810e81a409ac0692f363fde4f25ebb5ae4f30 | 2378 |
| [include/runtime/service_manager.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/include/runtime/service_manager.hpp) | 08774f61a55fa82b4f235b62dc54069175b37a79 | 5854 |
| [src/config/config_manager.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/config/config_manager.cpp) | b01fee7166a817c203fdb522538b599dbedd2295 | 13197 |
| [src/ipc/frame.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/ipc/frame.cpp) | 2ae24df6c858fd81d6bfc5e380969d567ac09398 | 1673 |
| [src/ipc/frame.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/ipc/frame.hpp) | bbbf30d210c541c166d5408b0b4393b6f4f9534a | 623 |
| [src/ipc/ipc_manager.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/ipc/ipc_manager.cpp) | bf870a82b966e699a37c5dd69c71a31b67e10e56 | 29447 |
| [src/ipc/ipc_manager.hpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/ipc/ipc_manager.hpp) | 3030a6fa1973fac2ed42c980d7bac532c06ea99e | 1722 |
| [src/ipc/main.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/ipc/main.cpp) | 45468627792fb105a1d460b01971787c06685878 | 1134 |
| [src/logger/logger.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/logger/logger.cpp) | bcd5f07b49f3ddf1610e7d6f9dc7bb2fab97ad36 | 1424 |
| [src/monitor/monitor.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/monitor/monitor.cpp) | cb2188b7ec406678452a06ff96ee2b26c73139f5 | 3671 |
| [src/runtime/device_state_manager.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/runtime/device_state_manager.cpp) | 12064dbc7a61bdeeb71bdf2635d103397cb7eae9 | 5092 |
| [src/runtime/event.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/runtime/event.cpp) | 995e29e4d9d6033e09c9c856bbcf35bce37f5ffe | 1904 |
| [src/runtime/recovery_manager.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/runtime/recovery_manager.cpp) | 91492268e363146295a9e106eab8acdcaef41f39 | 34721 |
| [src/runtime/runtime_manager.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/runtime/runtime_manager.cpp) | 1914c4063ab029391b4baae083bab14c4da6cd57 | 22875 |
| [src/runtime/service_aggregation.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/runtime/service_aggregation.cpp) | 3cc80afee52f17aa0feb5bec10d10e24eb189f61 | 15090 |
| [src/service/process_supervisor.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/service/process_supervisor.cpp) | 27a721cc48965267b55baec9c8977db05321f4a5 | 13941 |
| [src/service/service_manager.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/src/service/service_manager.cpp) | 24ab9af2434685845bae9522b68bffd064d22268 | 30045 |
| [tests/CMakeLists.txt](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/CMakeLists.txt) | 34661821fbac6aeeb4483d5cf57619f6f63ad95c | 2819 |
| [tests/device_ipc_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/device_ipc_tests.cpp) | eed6bfc07a395d8a0d1c246fc7e2533db5e9a4b1 | 25845 |
| [tests/device_state_manager_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/device_state_manager_tests.cpp) | 83e296d23510042eb5077821d801c69173c80a0d | 17393 |
| [tests/event_aggregation_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/event_aggregation_tests.cpp) | a45569767077c6a2a8d50e8ae85a04768d94069e | 29836 |
| [tests/ipc_integration_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/ipc_integration_tests.cpp) | 595365e92a6409824aedc4a18687d21c1f765474 | 27023 |
| [tests/phase2_integration_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/phase2_integration_tests.cpp) | f4616e2dadbd35606bf5b3b3dd0c85487b440587 | 20051 |
| [tests/phase2_review.md](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/phase2_review.md) | a738137079333a70ac883c44b0191059bfb79a7a | 8729 |
| [tests/phase2_static_check.py](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/phase2_static_check.py) | 5611ce669bc287f7ed0c32dfb059ccce898450d4 | 6758 |
| [tests/phase3_aggregation_static_check.py](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/phase3_aggregation_static_check.py) | 7879868d3ac7e3ed6010d49236df720c406bd58b | 10210 |
| [tests/phase3_ipc_static_check.py](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/phase3_ipc_static_check.py) | 6e9f06528ffae3a064e2b16ee11d345af1aadece | 7365 |
| [tests/phase3_state_static_check.py](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/phase3_state_static_check.py) | 01f90d6de14272f8bfe6e1efdfa49a05e1d18e4a | 8270 |
| [tests/phase3_validation_static_check.py](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/phase3_validation_static_check.py) | 8771315ef54df6dbd69001664e253485ea26bf1e | 13212 |
| [tests/phase4_static_check.py](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/phase4_static_check.py) | cd008d0c90a0f662697057660e9a36e4e631050c | 17171 |
| [tests/process_lifecycle_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/process_lifecycle_tests.cpp) | 30398fce799b800cffa42f0d92ca5d4b5bbeaabf | 10077 |
| [tests/recovery_coordination_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/recovery_coordination_tests.cpp) | f1acaadeb1d79a027632e7ff63ccdbe41e8d0620 | 28447 |
| [tests/recovery_dependency_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/recovery_dependency_tests.cpp) | 2604640b46ddc0bb471cc6de98f59519207f2c63 | 25654 |
| [tests/recovery_manager_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/recovery_manager_tests.cpp) | c74c8a3178345203aa01ea6dbc49af5adf28a17e | 47232 |
| [tests/runtime_core_tests.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tests/runtime_core_tests.cpp) | 4da7a62813a0d1d29c2c53cc20aa4859a1d1a96d | 36277 |
| [tools/fake_service/main.cpp](https://github.com/ppilll/runtime_management/blob/4843061fce2a329832105d34aaea380a35560c0e/tools/fake_service/main.cpp) | 409b1d74051411217baae474aedcdc06e90b7a98 | 4066 |
