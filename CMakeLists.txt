cmake_minimum_required(VERSION 3.16)

project(runtime_manager LANGUAGES CXX)

find_package(Threads REQUIRED)

add_library(runtime_core
    src/config/config_manager.cpp
    src/runtime/event.cpp
    src/runtime/runtime_manager.cpp
    src/service/process_supervisor.cpp
    src/service/service_manager.cpp
    src/monitor/monitor.cpp
    src/logger/logger.cpp
)

target_compile_features(runtime_core PUBLIC cxx_std_17)
set_target_properties(runtime_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(runtime_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(runtime_core PUBLIC Threads::Threads)

add_library(phase1_ipc STATIC src/ipc/frame.cpp src/ipc/ipc_manager.cpp)
target_compile_features(phase1_ipc PUBLIC cxx_std_17)
target_include_directories(phase1_ipc PUBLIC "${PROJECT_SOURCE_DIR}/src/ipc")
target_link_libraries(phase1_ipc PUBLIC runtime_core)

add_executable(runtime_manager src/ipc/main.cpp)
target_link_libraries(runtime_manager PRIVATE phase1_ipc)

add_executable(fake_service tools/fake_service/main.cpp src/ipc/frame.cpp)
target_compile_features(fake_service PRIVATE cxx_std_17)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
