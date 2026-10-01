# Phase1 Coding Rules


# 1. Language


C++17



---

# 2. Platform


Linux ARM64



---

# 3. Build


Use:

CMake



---

# 4. Allowed System API


Allowed:


- epoll
- timerfd
- signalfd
- Unix Domain Socket



---

# 5. Dependency


禁止引入：


- gRPC
- ROS
- Boost.Asio
- Database
- Web Framework



---

# 6. Architecture Rule


Runtime is management layer.


Runtime does not implement business.



---

# 7. State Ownership


Only:

service_manager


can modify Service lifecycle state.



禁止：


monitor

ipc_manager

runtime_manager


直接修改Service状态。



---

# 8. Event Rule


Internal Event Queue:


用于Runtime内部事件。



IPC EVENT:


用于跨进程通知。



两者禁止混用。



---

# 9. Directory Rule


Source:


src/


Headers:


include/


Tests:


tests/


Documents:


docs/P1/



---

# 10. Module Rule


禁止创建未定义模块。


Phase1模块：


- runtime_manager
- service_manager
- process_supervisor
- ipc_manager
- config_manager
- monitor
- logger



---

# 11. Change Rule


架构修改必须先修改：

docs/P1


然后再修改代码。


