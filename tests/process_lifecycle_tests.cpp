#include "runtime/process_supervisor.hpp"
#include "runtime/service_manager.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace runtime;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

// Re-exec this test binary as a fake service, avoiding shell quoting and tools.
volatile std::sig_atomic_t running = 1;
void stop_fixture(int) { running = 0; }

int fixture(int argc, char** argv) {
    if (argc != 5 || std::string(argv[4]) != "argument with spaces") return 90;
    const std::string mode(argv[2]);
    if (mode == "environment") {
        const char* value = std::getenv("RUNTIME_PROCESS_TEST");
        const char* inherited = std::getenv("RUNTIME_PROCESS_INHERITED");
        const char* empty = std::getenv("RUNTIME_PROCESS_EMPTY");
        char directory[4096];
        return value && std::string(value) == "last=value" &&
            inherited && std::string(inherited) == "inherited" && empty && *empty == '\0' &&
            ::getcwd(directory, sizeof(directory)) && std::string(directory) == "/" ? 23 : 91;
    }
    if (mode == "exit") return 0;
    if (mode != "wait" && mode != "ignore") return 92;
    struct sigaction action{};
    action.sa_handler = mode == "ignore" ? SIG_IGN : stop_fixture;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGTERM, &action, nullptr) != 0) return 93;
    const int ready_fd = std::stoi(argv[3]);
    const char ready = 'R';
    if (::write(ready_fd, &ready, 1) != 1) return 94;
    ::close(ready_fd);
    while (running) std::this_thread::sleep_for(5ms);
    return 0;
}

struct ReadyPipe {
    int fds[2];
    ReadyPipe() { require(::pipe(fds) == 0, "readiness pipe failed"); }
    ~ReadyPipe() { ::close(fds[0]); ::close(fds[1]); }
    void wait() {
        pollfd ready{fds[0], POLLIN, 0};
        int result;
        do { result = ::poll(&ready, 1, 3000); } while (result < 0 && errno == EINTR);
        char value = 0;
        require(result > 0 && (ready.revents & POLLIN) &&
                ::read(fds[0], &value, 1) == 1 && value == 'R', "fake service did not become ready");
    }
};

ServiceConfig definition(const std::string& name, const std::string& mode, int ready_fd = -1) {
    ServiceConfig config;
    config.service_name = name;
    config.executable = "/proc/self/exe";
    config.arguments = {"--fixture", mode, std::to_string(ready_fd), "argument with spaces"};
    config.startup_timeout = 3s;
    return config;
}

ProcessExit wait_exit(PosixProcessSupervisor& processes, int pid) {
    const auto deadline = Clock::now() + 3s;
    while (Clock::now() < deadline) {
        for (const auto& exit : processes.reap()) {
            require(exit.pid == pid, "unexpected process exit");
            return exit;
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("process exit timeout");
}

void test_start_stop_and_pid_validation() {
    ReadyPipe ready;
    PosixProcessSupervisor processes;
    const auto config = definition("graceful", "wait", ready.fds[1]);
    const int pid = processes.launchProcess(config);
    ready.wait();
    require(pid > 0 && processes.getPid("graceful") == pid &&
            processes.checkProcessAlive(pid), "launched process was not tracked/alive");
    rejects([&] { processes.launchProcess(config); }, "duplicate service process accepted");
    for (const int invalid : {-1, 0, static_cast<int>(::getpid())}) {
        require(!processes.checkProcessAlive(invalid), "invalid/unowned PID accepted");
        processes.terminateProcess(invalid);
        processes.force_stop(invalid);
    }
    require(processes.getPid("missing") == -1, "unknown service has a PID");
    processes.terminateProcess(pid);
    const auto exit = wait_exit(processes, pid);
    require(WIFEXITED(exit.status) && WEXITSTATUS(exit.status) == 0, "SIGTERM did not stop fake service gracefully");
    require(!processes.checkProcessAlive(pid) && processes.getPid("graceful") == -1 &&
            processes.reap().empty(), "reaped PID/status was retained");
    processes.force_stop(pid); // A stale PID must be a harmless no-op.
}

void test_exit_observation_and_environment() {
    require(::setenv("RUNTIME_PROCESS_INHERITED", "inherited", 1) == 0, "setenv failed");
    PosixProcessSupervisor processes;
    auto config = definition("environment", "environment");
    config.environment = {"RUNTIME_PROCESS_TEST=first", "RUNTIME_PROCESS_TEST=last=value", "RUNTIME_PROCESS_EMPTY="};
    config.working_directory = "/";
    const int pid = processes.start(config);
    ::unsetenv("RUNTIME_PROCESS_INHERITED");
    const auto deadline = Clock::now() + 3s;
    while (processes.checkProcessAlive(pid) && Clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    require(!processes.checkProcessAlive(pid), "exited child reported alive");
    require(processes.getPid("environment") == pid, "alive query consumed exit tracking");
    const auto exit = wait_exit(processes, pid);
    require(WIFEXITED(exit.status) && WEXITSTATUS(exit.status) == 23,
            "arguments/environment/cwd or preserved exit code failed");
    config = definition("environment", "exit");
    const auto clean = wait_exit(processes, processes.start(config));
    require(WIFEXITED(clean.status) && WEXITSTATUS(clean.status) == 0, "clean exit/relaunch failed");
}

void test_launch_failures() {
    PosixProcessSupervisor processes;
    auto config = definition("bad", "exit");
    config.executable = "/runtime-process-test/nonexistent-executable";
    rejects([&] { processes.start(config); }, "missing executable accepted");
    require(processes.getPid("bad") == -1 && processes.reap().empty(), "exec failure left a child tracked");
    config = definition("bad", "exit");
    config.working_directory = "/runtime-process-test/nonexistent-directory";
    rejects([&] { processes.start(config); }, "invalid working directory accepted");
    require(processes.getPid("bad") == -1 && processes.reap().empty(), "chdir failure left a child tracked");
    config.working_directory.clear();
    for (const auto& entry : std::vector<std::string>{"no-equals", "=empty-key", std::string("KEY=x\0y", 7)}) {
        config.environment = {entry};
        rejects([&] { processes.start(config); }, "malformed environment accepted");
    }
    config.environment.clear();
    config.arguments = {std::string("bad\0argument", 12)};
    rejects([&] { processes.start(config); }, "NUL argument accepted");
    config = definition("bad", "exit");
    config.startup_timeout = 0s;
    rejects([&] { processes.start(config); }, "nonpositive startup timeout accepted");
    config = definition("bad", "exit");
    const auto exit = wait_exit(processes, processes.start(config));
    require(WIFEXITED(exit.status) && WEXITSTATUS(exit.status) == 0, "failed launch poisoned registry");
}

void test_service_shutdown_escalation() {
    ReadyPipe ready;
    PosixProcessSupervisor processes;
    Monitor monitor([](Event) {});
    std::ostringstream output;
    Logger logger(output);
    ServiceManager manager(processes, monitor, logger);
    auto config = definition("stubborn", "ignore", ready.fds[1]);
    config.shutdown_timeout = 1s;
    manager.registerService(config);
    manager.startService("stubborn");
    ready.wait();
    const int pid = manager.queryServiceStatus("stubborn")->pid;
    const auto now = Clock::now();
    manager.stopService("stubborn", now);
    manager.tick(now + 999ms);
    require(processes.checkProcessAlive(pid) && manager.queryServiceStatus("stubborn")->state == ServiceState::stopping,
            "shutdown did not respect grace period");
    manager.tick(now + 1s);
    const auto exit = wait_exit(processes, pid);
    require(WIFSIGNALED(exit.status) && WTERMSIG(exit.status) == SIGKILL, "shutdown did not escalate to SIGKILL");
    manager.handle(Event{EventType::process_exited, "stubborn", now + 1s, pid, exit.status});
    require(manager.queryServiceStatus("stubborn")->state == ServiceState::stopped &&
            manager.queryServiceStatus("stubborn")->pid == -1, "exit did not synchronize service status");
}

void test_external_reaper_and_destructor() {
    int status = 0;
    int pid;
    {
        ReadyPipe ready;
        PosixProcessSupervisor processes;
        pid = processes.start(definition("cleanup", "wait", ready.fds[1]));
        ready.wait();
    }
    errno = 0;
    require(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD, "destructor did not reap its child");
    PosixProcessSupervisor processes;
    pid = processes.start(definition("external", "exit"));
    pid_t result;
    do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    require(result == pid && !processes.checkProcessAlive(pid), "external reap still reported alive");
    processes.stop(pid);
    processes.force_stop(pid);
    const auto exits = processes.reap();
    require(exits.size() == 1 && exits[0].pid == pid && exits[0].status == -1, "unknown exit status reported as success");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--fixture") return fixture(argc, argv);
    try {
        test_start_stop_and_pid_validation();
        test_exit_observation_and_environment();
        test_launch_failures();
        test_service_shutdown_escalation();
        test_external_reaper_and_destructor();
        std::cout << "process lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "process lifecycle test failed: " << error.what() << '\n';
        return 1;
    }
}
