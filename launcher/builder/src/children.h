#pragma once

// The programs ttt2-build runs: cafe-recomp, the compiler and the built game
// program. Each runs in a process group of its own, so that stopping it
// stops what it started too (a compiler driver's cc1plus and as), and is
// killed by the kernel if ttt2-build dies first. SIGINT, SIGTERM and SIGHUP
// cancel the build: whatever waits for a child then throws Cancelled.

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace build {

struct Cancelled {};

// Before any child is started.
void watch_signals();
bool cancelled();
// Throws Cancelled after a cancelling signal.
void check_cancelled();

struct Command {
    Command(std::vector<std::string> argv, std::vector<std::string> environment = {})
        : argv(std::move(argv)), environment(std::move(environment)) {}

    std::vector<std::string> argv;        // argv[0] a path, or a name looked up in PATH
    std::vector<std::string> environment; // "NAME=value", over ours
    bool capture = false;                 // its stdout through a pipe; else to our stderr
    bool capture_errors = false;          // with capture, its stderr too
};

struct Ended {
    pid_t pid = -1;
    int status = 0;         // as waitpid gives it
    uint64_t peak_bytes = 0; // the largest resident size of it and what it started
};

bool succeeded(const Ended& ended);
// "exit code 1", "killed by signal 9".
std::string describe(const Ended& ended);

// Starts a child; `output` gets the read end of its stdout when captured.
pid_t start(const Command& command, int* output = nullptr);
// Sleeps until a child may have ended, a signal came, `output` (if >= 0)
// is readable or `timeout_ms` (-1: none) passed.
void wait_event(int timeout_ms, int output = -1);
// The children that have ended since the last call.
std::vector<Ended> reap();
// Stops these children and everything they started, and waits for them.
void stop(const std::vector<pid_t>& children);

// Runs one child to its end; with `line`, its stdout line by line.
Ended run(const Command& command, const std::function<void(std::string_view)>& line = {});

} // namespace build
