#pragma once

// A program the launcher runs (the builder, the importer, the game): started
// through GSubprocess, its output read line by line as it comes, on the
// window's thread and without blocking it.

#include <gio/gio.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace launcher {

class Process : public std::enable_shared_from_this<Process> {
public:
    struct Handlers {
        // A line of its output (standard output, or `error` for standard
        // error) without its line break, as the program wrote it: paths in
        // it are the file system's bytes, not always UTF-8 (ui::valid()
        // for showing it).
        std::function<void(const std::string& line, bool error)> line;
        // It ended and its output is read: its exit code, 128 + the signal
        // when a signal ended it.
        std::function<void(int code)> exited;
    };

    using Environment = std::vector<std::pair<std::string, std::string>>;

    // Starts `argv` with these variables added to the launcher's own; none,
    // with the reason, when it cannot start. With `pipes` (the builder, the
    // importer) it runs in a process group of its own, with what it starts,
    // and gets SIGTERM if the launcher ends before it. `pipes` false: its
    // output goes where the launcher's goes (the game), and it gets a
    // session of its own, so that it keeps running when the launcher is
    // closed.
    static std::shared_ptr<Process> start(const std::vector<std::string>& argv, const Environment& environment,
                                          Handlers handlers, std::string* error, bool pipes = true);

    // How long a program asked to stop may take to clean up: the importer
    // first closes the hidden Tekken 7 (its helper, Wine's server, gamescope,
    // what is left of them, then the silent sound output), usually in a
    // second, but each of those steps has a time limit of its own when it
    // hangs, 30 s together at worst.
    static constexpr int kStopSeconds = 45;

    // Asks it to stop: SIGTERM to it alone, so that it stops what it
    // started in its own order and cleans up. What is left of its process
    // group once it has ended, or all of it when it has not ended within
    // kStopSeconds, is killed (SIGKILL).
    void terminate();
    bool running() const { return !exited_; }

    ~Process();

private:
    Process() = default;
    struct Stream {
        GDataInputStream* data = nullptr;
        bool done = true;
    };
    void read(Stream& stream);
    void finish();
    void kill_group();
    static void line_read(GObject* source, GAsyncResult* result, gpointer self);
    static void waited(GObject* source, GAsyncResult* result, gpointer self);
    static gboolean drained(gpointer self);
    static gboolean overdue(gpointer self);

    GSubprocess* subprocess_ = nullptr;
    GCancellable* cancellable_ = g_cancellable_new();
    Stream out_, err_;
    int group_ = 0;       // its process group (its pid), 0 when it has none of its own
    guint overdue_ = 0;   // the end of the time it has to stop
    bool exited_ = false, reported_ = false, stopping_ = false;
    int code_ = 0;
    Handlers handlers_;
};

} // namespace launcher
