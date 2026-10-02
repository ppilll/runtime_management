#include "runtime/process_supervisor.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace runtime {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return fd_; }
    int release() { const int fd = fd_; fd_ = -1; return fd; }
private:
    int fd_;
};

bool contains_nul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

// WNOWAIT keeps an exited child as a zombie until reap(), reserving its PID.
// No kill(pid, 0): that also accepts zombies and unrelated/reused PIDs.
bool child_alive(int pid, int pid_fd) {
    if (pid_fd >= 0) {
        pollfd watched{pid_fd, POLLIN, 0};
        int result;
        do { result = ::poll(&watched, 1, 0); } while (result < 0 && errno == EINTR);
        if (result < 0)
            throw std::system_error(errno, std::generic_category(), "poll(pidfd)");
        if (watched.revents & POLLNVAL)
            throw std::runtime_error("invalid process pidfd");
        return result == 0;
    }
    siginfo_t info{};
    int result;
    do { result = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT); }
    while (result < 0 && errno == EINTR);
    if (result < 0) {
        if (errno == ECHILD) return false;
        throw std::system_error(errno, std::generic_category(), "waitid");
    }
    return info.si_pid == 0;
}

int open_pid_fd(int pid) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int fd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (fd >= 0) return fd;
    if (errno != ENOSYS)
        throw std::system_error(errno, std::generic_category(), "pidfd_open");
#else
    (void)pid;
#endif
    // Older kernels/headers rely on exclusive child reaping and WNOWAIT.
    return -1;
}

int send_signal(int pid, int pid_fd, int signal) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    if (pid_fd >= 0)
        return static_cast<int>(::syscall(SYS_pidfd_send_signal, pid_fd, signal, nullptr, 0));
#else
    (void)pid_fd;
#endif
    return ::kill(pid, signal);
}

// Used only while destroying the backend or rolling back a failed launch.
// Never throw from cleanup; ECHILD must not lead to signalling a reused PID.
void kill_child(int pid, int pid_fd) noexcept {
    try {
        if (pid > 0 && child_alive(pid, pid_fd)) (void)send_signal(pid, pid_fd, SIGKILL);
    } catch (...) {}
}

void wait_child(int pid) noexcept {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

enum class LaunchStage { signals, directory, execute };
struct LaunchError { LaunchStage stage; int error; };

// Everything in the forked child uses async-signal-safe calls. Prepare strings,
// argv and envp in the parent; never use setenv or allocate after fork().
[[noreturn]] void launch_error(int fd, LaunchStage stage, int error) {
    const LaunchError failure{stage, error};
    const auto* bytes = reinterpret_cast<const char*>(&failure);
    std::size_t sent = 0;
    while (sent < sizeof(failure)) {
        const auto count = ::write(fd, bytes + sent, sizeof(failure) - sent);
        if (count > 0) sent += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    ::_exit(127);
}

void await_exec(int read_fd, int epoll_fd, std::chrono::steady_clock::time_point deadline) {
    LaunchError failure{};
    auto* bytes = reinterpret_cast<char*>(&failure);
    std::size_t received = 0;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) throw std::runtime_error("process startup timeout");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        // Round up, and cap before narrowing to epoll's signed int timeout.
        const int timeout = static_cast<int>(std::min<long long>(remaining + 1, INT_MAX));
        epoll_event ready{};
        const int result = ::epoll_wait(epoll_fd, &ready, 1, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "epoll_wait");
        }
        if (result == 0) continue;
        const auto count = ::read(read_fd, bytes + received, sizeof(failure) - received);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            throw std::system_error(errno, std::generic_category(), "exec handshake");
        }
        if (count == 0) {
            if (received == 0) return; // O_CLOEXEC closed the child's writer.
            throw std::runtime_error("incomplete exec handshake");
        }
        received += static_cast<std::size_t>(count);
        if (received == sizeof(failure)) {
            const char* operation = "child signal setup";
            if (failure.stage == LaunchStage::directory) operation = "chdir";
            if (failure.stage == LaunchStage::execute) operation = "execve";
            throw std::system_error(failure.error, std::generic_category(), operation);
        }
    }
}

} // namespace

PosixProcessSupervisor::~PosixProcessSupervisor() {
    // The owner must finish calls before destruction. Kill all before waiting.
    for (const auto& child : children_) kill_child(child.pid, child.pid_fd);
    for (const auto& child : children_) {
        wait_child(child.pid);
        if (child.pid_fd >= 0) ::close(child.pid_fd);
    }
}

int PosixProcessSupervisor::start(const ServiceConfig& config) {
    if (config.executable.empty() || contains_nul(config.executable) ||
        contains_nul(config.service_name) || contains_nul(config.working_directory) ||
        config.startup_timeout <= std::chrono::seconds::zero())
        throw std::invalid_argument("invalid process launch configuration");

    std::vector<char*> argv;
    argv.reserve(config.arguments.size() + 2);
    argv.push_back(const_cast<char*>(config.executable.c_str()));
    for (const auto& arg : config.arguments) {
        if (contains_nul(arg)) throw std::invalid_argument("argument contains NUL");
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    // Inherit the parent's environment; explicit KEY=VALUE entries override it.
    // Last duplicate wins and an empty value is supported.
    std::unordered_map<std::string, std::string> environment;
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string value(*entry);
        environment[value.substr(0, value.find('='))] = value;
    }
    for (const auto& value : config.environment) {
        const auto separator = value.find('=');
        if (separator == std::string::npos || separator == 0 || contains_nul(value))
            throw std::invalid_argument("environment entry must be KEY=VALUE without NUL");
        environment[value.substr(0, separator)] = value;
    }
    std::vector<char*> envp;
    envp.reserve(environment.size() + 1);
    for (auto& entry : environment) envp.push_back(entry.second.data());
    envp.push_back(nullptr);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!config.service_name.empty() && std::any_of(children_.begin(), children_.end(),
            [&](const Child& child) { return child.service_name == config.service_name; }))
        throw std::logic_error("service already has a tracked process: " + config.service_name);

    struct sigaction disposition{};
    if (::sigaction(SIGCHLD, nullptr, &disposition) < 0)
        throw std::system_error(errno, std::generic_category(), "sigaction(SIGCHLD)");
    if (disposition.sa_handler == SIG_IGN || (disposition.sa_flags & SA_NOCLDWAIT))
        throw std::logic_error("process supervisor requires waitable children");

    int error_pipe[2];
    if (::pipe2(error_pipe, O_CLOEXEC) != 0)
        throw std::system_error(errno, std::generic_category(), "pipe2");
    FileDescriptor reader(error_pipe[0]);
    FileDescriptor writer(error_pipe[1]);
    if (::fcntl(reader.get(), F_SETFL, O_NONBLOCK) < 0)
        throw std::system_error(errno, std::generic_category(), "fcntl");
    FileDescriptor epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
    if (epoll_fd.get() < 0)
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    epoll_event watched{};
    watched.events = EPOLLIN | EPOLLHUP;
    watched.data.fd = reader.get();
    if (::epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, reader.get(), &watched) != 0)
        throw std::system_error(errno, std::generic_category(), "epoll_ctl");

    // Allocate registry storage before fork so allocation failure cannot orphan
    // a successfully exec'd child. No other backend operation can reap it here.
    children_.push_back(Child{-1, -1, config.service_name});
    auto child = std::prev(children_.end());
    const char* executable = config.executable.c_str();
    const char* directory = config.working_directory.empty() ? nullptr : config.working_directory.c_str();
    char** arguments = argv.data();
    char** variables = envp.data();
    const auto launch_time = std::chrono::steady_clock::now();
    const auto max_timeout = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::time_point::max() - launch_time);
    if (config.startup_timeout > max_timeout) {
        children_.erase(child);
        throw std::invalid_argument("startup timeout exceeds clock range");
    }
    const auto deadline = launch_time + config.startup_timeout;
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int error = errno;
        children_.erase(child);
        throw std::system_error(error, std::generic_category(), "fork");
    }
    if (pid == 0) {
        ::close(error_pipe[0]);
        // The runtime may have installed handlers or blocked termination signals.
        struct sigaction defaults{};
        defaults.sa_handler = SIG_DFL;
        ::sigemptyset(&defaults.sa_mask);
        if (::sigaction(SIGTERM, &defaults, nullptr) < 0 ||
            ::sigaction(SIGINT, &defaults, nullptr) < 0 ||
            ::sigaction(SIGCHLD, &defaults, nullptr) < 0)
            launch_error(error_pipe[1], LaunchStage::signals, errno);
        sigset_t empty;
        ::sigemptyset(&empty);
        if (::sigprocmask(SIG_SETMASK, &empty, nullptr) < 0)
            launch_error(error_pipe[1], LaunchStage::signals, errno);
        if (directory && ::chdir(directory) < 0)
            launch_error(error_pipe[1], LaunchStage::directory, errno);
        ::execve(executable, arguments, variables);
        launch_error(error_pipe[1], LaunchStage::execute, errno);
    }
    child->pid = pid;
    ::close(writer.release());
    try {
        child->pid_fd = open_pid_fd(pid);
        await_exec(reader.get(), epoll_fd.get(), deadline);
    } catch (...) {
        kill_child(pid, child->pid_fd);
        wait_child(pid);
        if (child->pid_fd >= 0) ::close(child->pid_fd);
        children_.erase(child);
        throw;
    }
    return pid;
}

void PosixProcessSupervisor::signal_child(int pid, int signal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid <= 0) return; // In particular, never signal a process group.
    const auto child = std::find_if(children_.begin(), children_.end(),
        [pid](const Child& entry) { return entry.pid == pid; });
    if (child == children_.end() || !child_alive(pid, child->pid_fd)) return;
    if (send_signal(pid, child->pid_fd, signal) != 0 && errno != ESRCH)
        throw std::system_error(errno, std::generic_category(), "process signal");
}

void PosixProcessSupervisor::stop(int pid) { signal_child(pid, SIGTERM); }
void PosixProcessSupervisor::force_stop(int pid) { signal_child(pid, SIGKILL); }

bool PosixProcessSupervisor::checkProcessAlive(int pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid <= 0) return false;
    const auto child = std::find_if(children_.begin(), children_.end(),
        [pid](const Child& entry) { return entry.pid == pid; });
    return child != children_.end() && child_alive(pid, child->pid_fd);
}

int PosixProcessSupervisor::getPid(const std::string& service_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (service_name.empty()) return -1;
    const auto child = std::find_if(children_.begin(), children_.end(),
        [&](const Child& entry) { return entry.service_name == service_name; });
    return child == children_.end() ? -1 : child->pid;
}

std::vector<ProcessExit> PosixProcessSupervisor::reap() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProcessExit> exits;
    // Reserve before consuming any statuses so allocation failure loses none.
    exits.reserve(children_.size());
    auto child = children_.begin();
    while (child != children_.end()) {
        int status = 0;
        pid_t result;
        do { result = ::waitpid(child->pid, &status, WNOHANG); }
        while (result < 0 && errno == EINTR);
        if (result == child->pid || (result < 0 && errno == ECHILD)) {
            // An external reaper is a contract violation, but don't claim success.
            exits.push_back(ProcessExit{child->pid, result == child->pid ? status : -1});
            if (child->pid_fd >= 0) ::close(child->pid_fd);
            child = children_.erase(child);
        } else if (result < 0) {
            throw std::system_error(errno, std::generic_category(), "waitpid");
        } else {
            ++child;
        }
    }
    return exits;
}

} // namespace runtime
