# Process Lifecycle Design


## Objective


Implement Linux process management backend.


---

# 1. Process Backend Decision


Selected:


fork/exec


Reason:


- Linux native

- simple

- ARM64 compatible

- direct process control


Not using:


systemd


Reason:


- additional dependency

- reduces Runtime control

- embedded systems may not include systemd


---

# 2. Process Controller Responsibilities


Must support:


## Start


Input:


service executable

arguments

environment


Output:


PID



---


## Stop


Sequence:



SIGTERM
|

wait shutdown_timeout
|

SIGKILL


---


## Monitor


Detect:


- process exited

- process abnormal termination


---

# 3. PID Management


Runtime maintains process information.


Do not depend on pid file.


Reason:


Avoid:


- stale pid

- pid reuse problem


---

# 4. Interface


Required:



launchProcess()
terminateProcess()
checkProcessAlive()
getPid()


---

# 5. Limitation


Process Controller does not decide recovery.

Recovery Manager owns restart policy.