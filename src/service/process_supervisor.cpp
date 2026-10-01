#include "runtime/process_supervisor.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace runtime {

PosixProcessSupervisor::~PosixProcessSupervisor() {
    for (int pid : children_) ::kill(pid, SIGKILL);
    for (int pid : children_) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
}

int PosixProcessSupervisor::start(const ServiceConfig& config) {
    std::vector<char*> argv;
    argv.reserve(config.arguments.size() + 2);
    argv.push_back(const_cast<char*>(config.executable.c_str()));
    for (const auto& arg : config.arguments) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int error_pipe[2];
    if (::pipe2(error_pipe, O_CLOEXEC) != 0)
        throw std::system_error(errno, std::generic_category(), "pipe2");
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int error = errno;
        ::close(error_pipe[0]); ::close(error_pipe[1]);
        throw std::system_error(error, std::generic_category(), "fork");
    }
    if (pid == 0) {
        ::close(error_pipe[0]);
        ::execv(config.executable.c_str(), argv.data());
        const int error = errno;
        (void)::write(error_pipe[1], &error, sizeof(error));
        _exit(127);
    }
    ::close(error_pipe[1]);
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        const int error = errno;
        ::close(error_pipe[0]);
        ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        throw std::system_error(error, std::generic_category(), "epoll_create1");
    }
    epoll_event watched{};
    watched.events = EPOLLIN | EPOLLHUP;
    watched.data.fd = error_pipe[0];
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, error_pipe[0], &watched) != 0) {
        const int error = errno;
        ::close(epoll_fd);
        ::close(error_pipe[0]);
        ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        throw std::system_error(error, std::generic_category(), "epoll_ctl");
    }
    const auto deadline = std::chrono::steady_clock::now() + config.startup_timeout;
    int wait_result = 0;
    int wait_error = 0;
    do {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { wait_result = 0; break; }
        epoll_event ready{};
        wait_result = ::epoll_wait(epoll_fd, &ready, 1, static_cast<int>(remaining));
        wait_error = errno;
    } while (wait_result < 0 && wait_error == EINTR);
    ::close(epoll_fd);
    if (wait_result <= 0) {
        ::close(error_pipe[0]);
        ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (wait_result == 0) throw std::runtime_error("process startup timeout");
        throw std::system_error(wait_error, std::generic_category(), "epoll_wait");
    }
    int child_error = 0;
    ssize_t count;
    do { count = ::read(error_pipe[0], &child_error, sizeof(child_error)); }
    while (count < 0 && errno == EINTR);
    const int read_error = errno;
    ::close(error_pipe[0]);
    if (count != 0) {
        if (count < 0) ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (count > 0) throw std::system_error(child_error, std::generic_category(), "execv");
        throw std::system_error(read_error, std::generic_category(), "exec handshake");
    }
    children_.push_back(pid);
    return pid;
}

void PosixProcessSupervisor::stop(int pid) {
    if (pid > 0 && std::find(children_.begin(), children_.end(), pid) != children_.end()) {
        if (::kill(pid, SIGTERM) != 0 && errno != ESRCH)
            throw std::system_error(errno, std::generic_category(), "kill");
    }
}

void PosixProcessSupervisor::force_stop(int pid) {
    if (pid > 0 && std::find(children_.begin(), children_.end(), pid) != children_.end()) {
        if (::kill(pid, SIGKILL) != 0 && errno != ESRCH)
            throw std::system_error(errno, std::generic_category(), "kill");
    }
}

std::vector<ProcessExit> PosixProcessSupervisor::reap() {
    std::vector<ProcessExit> exits;
    auto it = children_.begin();
    while (it != children_.end()) {
        int status = 0;
        const auto result = ::waitpid(*it, &status, WNOHANG);
        if (result == *it || (result < 0 && errno == ECHILD)) {
            // ECHILD means another reaper consumed the status. Do not report
            // an unknown exit as a successful exit code of zero.
            exits.push_back(ProcessExit{*it, result == *it ? status : -1});
            it = children_.erase(it);
        } else if (result < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "waitpid");
        } else {
            ++it;
        }
    }
    return exits;
}

} // namespace runtime
