# Source Manifest

Repository: ppilll/runtime_management；branch: master；commit: 2ccbc733a1386c46917e6aee2fc7e0680e4321e9。本次通过已连接GitHub只读获取；递归tree完整（truncated=false）。交付前再次查询master，HEAD仍为同一提交。

下列90个非空文本文件已完整获取，blob SHA为连接器/tree所返回。两处空.gitkeep不包含内容，不计入读取文本数。Manifest表示获取范围；实质审查重点为指定P3设计、全部P0–P2设计/实施记录、Runtime/Service/IPC/Monitor/config链路，以及tests中的恢复/代次/关机/聚合/IPC断言。没有执行这些源码或历史检查脚本。

| Path | Blob SHA | 固定提交来源 |
| --- | --- | --- |
| .gitignore | 99d4f726b8473b7384e91564715ecf9055d00999 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/.gitignore) |
| CMakeLists.txt | 79d077c1d88564a410e292bb213cebedfca14ffd | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/CMakeLists.txt) |
| docs/P0/RK3588_Phase0_Final_Design_Result.txt | c2c5df92128d1ed8dfac8a0a07731254e42321a4 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P0/RK3588_Phase0_Final_Design_Result.txt) |
| docs/P0/RK3588_Runtime_Management_Phase0_Architecture_Design.md | 20fca743575f17b8132ac14c74fdeaa31afc5c83 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P0/RK3588_Runtime_Management_Phase0_Architecture_Design.md) |
| docs/P1/architecture_review.md | 8c4cf19f66eca8142b788fc823083c4d66fe3871 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/architecture_review.md) |
| docs/P1/coding_rules.md | 78fb7bac65448c3851af745419bedaafd45b62c6 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/coding_rules.md) |
| docs/P1/ipc_protocol.md | bce3136ff7049d0222eb57e844928c19c9cd59db | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/ipc_protocol.md) |
| docs/P1/module_design.md | d4ecfdc3d76833f63c29b6487805f2d0842476e0 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/module_design.md) |
| docs/P1/phase1_architecture.md | 3f8e26913b083d9233e286c76d071cc14f97a4f0 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/phase1_architecture.md) |
| docs/P1/review_report.md | 48c54586cde00b0ea6fea467553c43e0bdaa9357 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/review_report.md) |
| docs/P1/runtime_core_contract.md | aebbcbcbdcc096077cce9a5974416dc1fae2c330 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/runtime_core_contract.md) |
| docs/P1/test_strategy.md | a007f6c115008104ba0f9360236e05ef437915be | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/test_strategy.md) |
| docs/P1/thread_plan.md | 2be57bf29b1f439b3c77d21f5d1d8b0e5cb45045 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P1/thread_plan.md) |
| docs/P2/README.md | 1b1a7a54101a54dc03045b06162661c407cd633a | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/README.md) |
| docs/P2/architecture.md | 7f843adeb04a393f1488bfec899f227d135c1f68 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/architecture.md) |
| docs/P2/codex/thread1_service_manager_prompt.md | 0b1fb0d9b686ee81aeb25b4289dd3a8b4801a3b5 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/codex/thread1_service_manager_prompt.md) |
| docs/P2/codex/thread2_process_prompt.md | 45417e6d3980d36c915731acdd482ea58fef230b | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/codex/thread2_process_prompt.md) |
| docs/P2/codex/thread3_recovery_prompt.md | 76e24a6c651f5bcebec9387a228176537080d105 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/codex/thread3_recovery_prompt.md) |
| docs/P2/codex/thread4_ipc_prompt.md | 4c2aa99c8c5d22779ebf3cd361b3287ae776f7af | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/codex/thread4_ipc_prompt.md) |
| docs/P2/codex/thread5_test_prompt.md | 1915f240ccd6582b001a660d96e7b2d52c38e61d | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/codex/thread5_test_prompt.md) |
| docs/P2/implementation_plan.md | 6f0295dca4f1659689b790f8f4565c6778765577 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/implementation_plan.md) |
| docs/P2/ipc_config_extension.md | 54a588f4e61cf5688f4b923db860dd188ab7613b | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/ipc_config_extension.md) |
| docs/P2/ipc_config_implementation.md | 0dec4c1bcf3c4b527be2e2b0307eda76e6cc00d6 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/ipc_config_implementation.md) |
| docs/P2/process_backend_implementation.md | c93c688e3c6963ebb0b4cb6149b90d10f0ab0a42 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/process_backend_implementation.md) |
| docs/P2/process_lifecycle_design.md | a8949eda5a2c40576a96c292a76059765615a037 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/process_lifecycle_design.md) |
| docs/P2/recovery_dependency_design.md | bec96607e226184f0592a0d28b85b69e0be29989 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/recovery_dependency_design.md) |
| docs/P2/recovery_dependency_implementation.md | 21ad4e33235d343c49b1265ae16428af2a0da5a0 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/recovery_dependency_implementation.md) |
| docs/P2/testing_strategy.md | c3c9858209c17c05fea016d8a50e089db04c5b6f | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P2/testing_strategy.md) |
| docs/P3/AGENTS.md | 09e0cca47f2ced6f582c344d8e54fce88a894077 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/AGENTS.md) |
| docs/P3/AGGREGATION_RULE.md | 49001011e77f8009d85585581fc2f764d1c0bf08 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/AGGREGATION_RULE.md) |
| docs/P3/ARCHITECTURE.md | 6161abc3dbc69a794799959a3aa958897d24f59e | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/ARCHITECTURE.md) |
| docs/P3/CODEX_DEPENDENCY.md | 2c2926d854a71ba0106203a5f23f833ad07a9171 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/CODEX_DEPENDENCY.md) |
| docs/P3/CODE_REVIEW_CHECKLIST.md | 0c90f8ba26c6b8cf82e1c3855f6de45ba42f85e0 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/CODE_REVIEW_CHECKLIST.md) |
| docs/P3/DEVICE_STATE_MODEL.md | d1e66c3d94680612c1dfd374361e339d4d4c8381 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/DEVICE_STATE_MODEL.md) |
| docs/P3/EVENT_MODEL.md | 59681e793b94989ec6ac63c4220c32d9c68cf008 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/EVENT_MODEL.md) |
| docs/P3/IPC_EXTENSION.md | bb9d437253fec02ba74250920428f56d1f9c6cfc | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/IPC_EXTENSION.md) |
| docs/P3/MONITOR_INTEGRATION.md | bfb0d23885c46a1582d9df1baea43fdb9e9516d2 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/MONITOR_INTEGRATION.md) |
| docs/P3/PERSISTENCE_DESIGN.md | d9255e872f15420dd3135b2af77526744733c28a | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/PERSISTENCE_DESIGN.md) |
| docs/P3/README.md | 8b6028a19a9e3565d69b9e8c18e1bceec837eaf8 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/README.md) |
| docs/P3/STATE_TRANSITION.md | eab56ab5769b4e23092f08437448b3812543a0ce | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/STATE_TRANSITION.md) |
| docs/P3/codex/thread1_state_manager_prompt.md | 91a5ec3414f7395cf2e22eabed60cb2596fc3b51 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/codex/thread1_state_manager_prompt.md) |
| docs/P3/codex/thread2_event_aggregation_prompt.md | 7faf3d3c0f8a0343b57d1ab1fcb4acc699e8b8fb | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/codex/thread2_event_aggregation_prompt.md) |
| docs/P3/codex/thread3_ipc_prompt.md | 32b5f82599ad475db16417ffe4d407004e5b284d | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/codex/thread3_ipc_prompt.md) |
| docs/P3/codex/thread4_test_prompt.md | e1aebf1fbd9056765abe0c0060abe9ca8ed5fe42 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/codex/thread4_test_prompt.md) |
| docs/P3/device_state_manager_implementation.md | e98256e2bee3e4687132589106c8b1e396a5fa68 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/device_state_manager_implementation.md) |
| docs/P3/event_aggregation_implementation.md | b0d9f0215a90c0cded1c21823f7a74a141ab506e | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/event_aggregation_implementation.md) |
| docs/P3/ipc_extension_implementation.md | 1741fc0df060ee064c711a1860920a892414feb4 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/ipc_extension_implementation.md) |
| docs/P3/phase4/RECOVERY_STRATEGY_INTERFACE.md | 86648b586d6e1ef163ca694a5cc527b8672153e7 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/phase4/RECOVERY_STRATEGY_INTERFACE.md) |
| docs/P3/review_fixes.md | b703917bc1f2e91749ad14a5a2900d9353261527 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/review_fixes.md) |
| docs/P3/validation_review.md | 6e28a64b1ae9e424258845a5ae30dcb4490dd891 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/docs/P3/validation_review.md) |
| include/runtime/config_manager.hpp | 939b1cce2deb1461b65991b53b9c693a9417c6a2 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/config_manager.hpp) |
| include/runtime/device_state.hpp | 1aae629539ed8fa46cd815928fb1bbffcbdbdd4c | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/device_state.hpp) |
| include/runtime/device_state_manager.hpp | a9bd63e718755a799da19d64a45fad0de61440a4 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/device_state_manager.hpp) |
| include/runtime/event.hpp | 889deaf96758573dfbe69a67633520b14b61ad1f | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/event.hpp) |
| include/runtime/logger.hpp | f753e5038ceeaf991a95f607f1951a99fd89da61 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/logger.hpp) |
| include/runtime/monitor.hpp | 3278ee0b81d27a3669e868d4fc7495f9b0b417a8 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/monitor.hpp) |
| include/runtime/process_supervisor.hpp | ebbd1132db541569a03ad82a5db8dfa624e88ba9 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/process_supervisor.hpp) |
| include/runtime/runtime_manager.hpp | d4eed795140bdd9050a3dcc79e331b3bf81e2e16 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/runtime_manager.hpp) |
| include/runtime/service_aggregation.hpp | 2917f969e83b63882e3237efd7ed2bc21e627886 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/service_aggregation.hpp) |
| include/runtime/service_manager.hpp | 0934c5234b822a7607cbac9cec56d59fb663a5d4 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/include/runtime/service_manager.hpp) |
| src/config/config_manager.cpp | 081bf038e8eb13c79c11a01fe57aca8f0f1917cb | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/config/config_manager.cpp) |
| src/ipc/frame.cpp | 2ae24df6c858fd81d6bfc5e380969d567ac09398 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/ipc/frame.cpp) |
| src/ipc/frame.hpp | bbbf30d210c541c166d5408b0b4393b6f4f9534a | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/ipc/frame.hpp) |
| src/ipc/ipc_manager.cpp | 90850bdc64f961541ffb60effc56d794a185e859 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/ipc/ipc_manager.cpp) |
| src/ipc/ipc_manager.hpp | 3030a6fa1973fac2ed42c980d7bac532c06ea99e | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/ipc/ipc_manager.hpp) |
| src/ipc/main.cpp | 45468627792fb105a1d460b01971787c06685878 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/ipc/main.cpp) |
| src/logger/logger.cpp | bcd5f07b49f3ddf1610e7d6f9dc7bb2fab97ad36 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/logger/logger.cpp) |
| src/monitor/monitor.cpp | af7db87ceb439514ca97f238fbc3f1cd61fc3fa8 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/monitor/monitor.cpp) |
| src/runtime/device_state_manager.cpp | 12064dbc7a61bdeeb71bdf2635d103397cb7eae9 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/device_state_manager.cpp) |
| src/runtime/event.cpp | 995e29e4d9d6033e09c9c856bbcf35bce37f5ffe | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/event.cpp) |
| src/runtime/runtime_manager.cpp | 670d8bd3df68c4c645d4936cbaf027214d68e0c4 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/runtime_manager.cpp) |
| src/runtime/service_aggregation.cpp | e3d6d0ef2798f2aa237f8222e2604cc2b8024b78 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/runtime/service_aggregation.cpp) |
| src/service/process_supervisor.cpp | 27a721cc48965267b55baec9c8977db05321f4a5 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/service/process_supervisor.cpp) |
| src/service/service_manager.cpp | f573687405935c9a72cc10d736ebcc9535587706 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/src/service/service_manager.cpp) |
| tests/CMakeLists.txt | e990cb89a874bef9d0566d6ee8c021d5f3a937b3 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/CMakeLists.txt) |
| tests/device_ipc_tests.cpp | 02ac7e8b28c4e77ea2f9686de2a517945e2db9eb | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/device_ipc_tests.cpp) |
| tests/device_state_manager_tests.cpp | 83e296d23510042eb5077821d801c69173c80a0d | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/device_state_manager_tests.cpp) |
| tests/event_aggregation_tests.cpp | 80aa5f0951a8da1510f5f0a6b9c446564594c4a1 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/event_aggregation_tests.cpp) |
| tests/ipc_integration_tests.cpp | 829dc5743292b35c1278b42babcb7de553e3aba5 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/ipc_integration_tests.cpp) |
| tests/phase2_integration_tests.cpp | 576e8184ff75f996e86a8381773bd0d6f36758c2 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/phase2_integration_tests.cpp) |
| tests/phase2_review.md | a738137079333a70ac883c44b0191059bfb79a7a | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/phase2_review.md) |
| tests/phase2_static_check.py | 5611ce669bc287f7ed0c32dfb059ccce898450d4 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/phase2_static_check.py) |
| tests/phase3_aggregation_static_check.py | 52d796b2c032e992eeae84904aa8490599764124 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/phase3_aggregation_static_check.py) |
| tests/phase3_ipc_static_check.py | 6e9f06528ffae3a064e2b16ee11d345af1aadece | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/phase3_ipc_static_check.py) |
| tests/phase3_state_static_check.py | 01f90d6de14272f8bfe6e1efdfa49a05e1d18e4a | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/phase3_state_static_check.py) |
| tests/phase3_validation_static_check.py | 2fcc5ce641b64dc9ba2b20b99c24c5c823728a30 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/phase3_validation_static_check.py) |
| tests/process_lifecycle_tests.cpp | 30398fce799b800cffa42f0d92ca5d4b5bbeaabf | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/process_lifecycle_tests.cpp) |
| tests/recovery_dependency_tests.cpp | e807189343e909fcac1d74226002070baf1ac267 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/recovery_dependency_tests.cpp) |
| tests/runtime_core_tests.cpp | 97ec5f75cea4a857417730ea22229fae70acab59 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tests/runtime_core_tests.cpp) |
| tools/fake_service/main.cpp | 409b1d74051411217baae474aedcdc06e90b7a98 | [GitHub](https://github.com/ppilll/runtime_management/blob/2ccbc733a1386c46917e6aee2fc7e0680e4321e9/tools/fake_service/main.cpp) |

## Requested output mapping
| 用户要求 | 完整内容位置 |
| --- | --- |
| Current Repository Assessment / 前序Phase、预留接口审查 | REPOSITORY_ASSESSMENT |
| Architecture / Ownership / A-B方案 | ARCHITECTURE、DECISIONS |
| State / Request / Result | RECOVERY_MODEL |
| Generation / stale protection | RECOVERY_GENERATION |
| Retry / Backoff / Timeout / Config | RECOVERY_POLICY |
| Dependency / Shutdown Debt | SERVICE_INTEGRATION、RISK_ANALYSIS |
| Events / Concurrency | RECOVERY_EVENT_FLOW |
| Device / IPC impact | DEVICE_STATE_INTEGRATION |
| Modules / Interfaces / Boundaries | CHANGE_LIST |
| Testing matrix / 20 Acceptance Criteria | TEST_PLAN |
| Risk | RISK_ANALYSIS |
| docs/P4全套 / AGENTS | 本目录全部实质文档 |
| codex_package独立prompts / 顺序 / parallel风险 | codex_package/README和四个prompt |
| Final freeze / unresolved / blocking / deferred | DESIGN_FREEZE_CANDIDATE |

以上文档是设计内容，源码片段不会生成或修改。本地work缓存仅用于读取/审查，不随交付包提供C++文件。
