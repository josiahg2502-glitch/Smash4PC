#include "children.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>

extern char** environ;

namespace build {
namespace {

volatile sig_atomic_t g_cancelled = 0;
// The self-pipe: a signal handler writes a byte, wait_event polls for it, so
// that a signal or a child's end can never fall between a check and a sleep.
int g_wake[2] = {-1, -1};

void on_signal(int number) {
    const int saved = errno;
    if (number != SIGCHLD) g_cancelled = 1;
    const char byte = 0;
    [[maybe_unused]] const ssize_t n = ::write(g_wake[1], &byte, 1);
    errno = saved;
}

std::runtime_error system_error(const std::string& what) {
    return std::runtime_error(what + ": " + std::strerror(errno));
}

} // namespace

void watch_signals() {
    if (::pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0) throw system_error("cannot create a pipe");
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    for (const int number : {SIGINT, SIGTERM, SIGHUP, SIGCHLD}) ::sigaction(number, &action, nullptr);
    // A reader that went away (the launcher closed) must not kill the build
    // before it has stopped its children.
    ::signal(SIGPIPE, SIG_IGN);
}

bool cancelled() { return g_cancelled != 0; }

void check_cancelled() {
    if (cancelled()) throw Cancelled{};
}

bool succeeded(const Ended& ended) { return WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 0; }

std::string describe(const Ended& ended) {
    if (WIFEXITED(ended.status)) return "exit code " + std::to_string(WEXITSTATUS(ended.status));
    if (WIFSIGNALED(ended.status)) {
        const int number = WTERMSIG(ended.status);
        return "killed by signal " + std::to_string(number) + " (" + ::strsignal(number) + ")";
    }
    return "status " + std::to_string(ended.status);
}

pid_t start(const Command& command, int* output) {
    std::vector<char*> argv;
    for (const std::string& arg : command.argv) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** entry = environ; *entry; ++entry) {
        const std::string_view name(*entry, std::strcspn(*entry, "="));
        const bool replaced = std::any_of(command.environment.begin(), command.environment.end(), [&](const std::string& e) {
            return e.size() > name.size() && e.starts_with(name) && e[name.size()] == '=';
        });
        if (!replaced) environment.emplace_back(*entry);
    }
    environment.insert(environment.end(), command.environment.begin(), command.environment.end());
    std::vector<char*> envp;
    for (const std::string& entry : environment) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

    int pipe_fds[2] = {-1, -1};
    if (command.capture && ::pipe2(pipe_fds, O_CLOEXEC) != 0) throw system_error("cannot create a pipe");
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) throw system_error("cannot start " + command.argv[0]);
    if (pid == 0) {
        ::setpgid(0, 0);
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent) ::_exit(127);
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        for (const int number : {SIGINT, SIGTERM, SIGHUP, SIGCHLD, SIGPIPE}) ::sigaction(number, &action, nullptr);
        if (const int null = ::open("/dev/null", O_RDONLY); null >= 0) ::dup2(null, STDIN_FILENO);
        ::dup2(command.capture ? pipe_fds[1] : STDERR_FILENO, STDOUT_FILENO);
        if (command.capture && command.capture_errors) ::dup2(pipe_fds[1], STDERR_FILENO);
        ::execvpe(argv[0], argv.data(), envp.data());
        ::dprintf(STDERR_FILENO, "ttt2-build: cannot start %s: %s\n", argv[0], std::strerror(errno));
        ::_exit(127);
    }
    ::setpgid(pid, pid); // (whichever of the two runs first)
    if (command.capture) {
        ::close(pipe_fds[1]);
        ::fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
        *output = pipe_fds[0];
    }
    return pid;
}

void wait_event(int timeout_ms, int output) {
    pollfd fds[2] = {{g_wake[0], POLLIN, 0}, {output, POLLIN, 0}};
    ::poll(fds, output >= 0 ? 2 : 1, timeout_ms);
}

std::vector<Ended> reap() {
    char buffer[64];
    while (::read(g_wake[0], buffer, sizeof buffer) > 0) {
    }
    std::vector<Ended> ended;
    for (;;) {
        Ended e;
        struct rusage usage {};
        e.pid = ::wait4(-1, &e.status, WNOHANG, &usage);
        if (e.pid <= 0) break;
        e.peak_bytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
        ended.push_back(e);
    }
    return ended;
}

void stop(const std::vector<pid_t>& children) {
    for (const pid_t pid : children) ::kill(-pid, SIGTERM);
    std::vector<pid_t> left = children;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!left.empty() && std::chrono::steady_clock::now() < deadline) {
        wait_event(100);
        for (const Ended& e : reap()) std::erase(left, e.pid);
    }
    // Whatever is left of the groups (also of a child that ended, if what it
    // started did not).
    for (const pid_t pid : children) ::kill(-pid, SIGKILL);
    for (const pid_t pid : left) ::waitpid(pid, nullptr, 0);
}

Ended run(const Command& command, const std::function<void(std::string_view)>& line) {
    Command captured = command;
    captured.capture = static_cast<bool>(line);
    int output = -1;
    const pid_t pid = start(captured, &output);
    std::string pending;
    // Everything readable now; false at the end of the output.
    const auto read_output = [&] {
        char buffer[65536];
        for (;;) {
            const ssize_t n = ::read(output, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return true; // (EAGAIN)
            if (n == 0) return false;
            pending.append(buffer, static_cast<size_t>(n));
            for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
                line(std::string_view(pending).substr(0, end));
                pending.erase(0, end + 1);
            }
        }
    };
    std::optional<Ended> ended;
    try {
        while (!ended) {
            if (cancelled()) throw Cancelled{};
            wait_event(-1, output);
            if (output >= 0 && !read_output()) {
                ::close(output);
                output = -1;
            }
            for (const Ended& e : reap()) {
                if (e.pid == pid) ended = e;
            }
        }
        // (What it wrote before it ended; not what a process it left behind
        // may still write.)
        if (output >= 0) read_output();
        if (!pending.empty()) line(pending);
    } catch (...) {
        if (!ended) stop({pid});
        if (output >= 0) ::close(output);
        throw;
    }
    if (output >= 0) ::close(output);
    return *ended;
}

} // namespace build
