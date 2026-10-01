add_executable(runtime_core_tests runtime_core_tests.cpp)
target_link_libraries(runtime_core_tests PRIVATE runtime_core)
add_test(NAME runtime_core_unit COMMAND runtime_core_tests)

add_executable(ipc_integration_tests ipc_integration_tests.cpp)
target_link_libraries(ipc_integration_tests PRIVATE phase1_ipc)
add_test(NAME phase1_ipc_integration COMMAND ipc_integration_tests
    $<TARGET_FILE:runtime_manager> $<TARGET_FILE:fake_service>)
set_tests_properties(phase1_ipc_integration PROPERTIES TIMEOUT 90)
