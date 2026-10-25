// Programs the launcher runs (see process.h). Every pending read and the
// wait hold a reference to the Process, so it lives until all of them have
// come back, whatever the window does meanwhile.

#include "process.h"

#include <sys/prctl.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>

namespace launcher {
namespace {

using Ref = std::shared_ptr<Process>;

void delete_ref(gpointer ref) { delete static_cast<Ref*>(ref); }

void own_session(gpointer) { ::setsid(); }

// In the child, before it runs the program (only what is safe there): a
// process group of its own, which Cancel can stop whole, and SIGTERM, as on
// Cancel, when the launcher ends first (killed, or Ctrl+C in its terminal,
// which no longer reaches another group).
void own_group(gpointer launcher) {
    ::setpgid(0, 0);
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() != static_cast<pid_t>(GPOINTER_TO_INT(launcher))) ::raise(SIGTERM); // (it has ended already)
}

} // namespace

std::shared_ptr<Process> Process::start(const std::vector<std::string>& argv, const Environment& environment,
                                        Handlers handlers, std::string* error, bool pipes) {
    std::shared_ptr<Process> p(new Process());
    p->handlers_ = std::move(handlers);
    GSubprocessLauncher* launcher = g_subprocess_launcher_new(
        pipes ? GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE)
              : G_SUBPROCESS_FLAGS_NONE); // (standard input: /dev/null, so nothing asks on a terminal)
    for (const auto& [name, value] : environment) {
        g_subprocess_launcher_setenv(launcher, name.c_str(), value.c_str(), TRUE);
    }
    if (pipes) g_subprocess_launcher_set_child_setup(launcher, own_group, GINT_TO_POINTER(::getpid()), nullptr);
    else g_subprocess_launcher_set_child_setup(launcher, own_session, nullptr, nullptr);
    std::vector<const char*> args;
    for (const std::string& a : argv) args.push_back(a.c_str());
    args.push_back(nullptr);
    GError* failure = nullptr;
    p->subprocess_ = g_subprocess_launcher_spawnv(launcher, args.data(), &failure);
    g_object_unref(launcher);
    if (!p->subprocess_) {
        if (error) *error = failure ? failure->message : "cannot start " + argv[0];
        g_clear_error(&failure);
        return nullptr;
    }
    if (pipes) {
        // (The child has made its group before the spawn returns: GLib waits
        // for it to start the program.)
        if (const char* pid = g_subprocess_get_identifier(p->subprocess_)) p->group_ = std::atoi(pid);
        p->out_ = {g_data_input_stream_new(g_subprocess_get_stdout_pipe(p->subprocess_)), false};
        p->err_ = {g_data_input_stream_new(g_subprocess_get_stderr_pipe(p->subprocess_)), false};
        for (Stream* s : {&p->out_, &p->err_}) {
            g_data_input_stream_set_newline_type(s->data, G_DATA_STREAM_NEWLINE_TYPE_ANY);
            p->read(*s);
        }
    }
    g_subprocess_wait_async(p->subprocess_, p->cancellable_, waited, new Ref(p));
    return p;
}

Process::~Process() {
    g_cancellable_cancel(cancellable_);
    if (out_.data) g_object_unref(out_.data);
    if (err_.data) g_object_unref(err_.data);
    if (subprocess_) g_object_unref(subprocess_);
    g_object_unref(cancellable_);
}

void Process::terminate() {
    if (exited_ || stopping_) return;
    stopping_ = true;
    g_subprocess_send_signal(subprocess_, SIGTERM);
    // (The timer holds a reference: it is removed once the program ends.)
    overdue_ = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, kStopSeconds, overdue, new Ref(shared_from_this()),
                                          delete_ref);
}

gboolean Process::overdue(gpointer data) {
    Process& p = **static_cast<Ref*>(data);
    p.overdue_ = 0;
    if (!p.exited_) p.kill_group();
    return G_SOURCE_REMOVE;
}

void Process::kill_group() {
    // (Its group outlives it while anything of it runs, so its number is not
    // given to another process meanwhile.)
    if (group_ > 0) ::kill(-group_, SIGKILL);
    else if (!exited_) g_subprocess_force_exit(subprocess_);
}

void Process::read(Stream& stream) {
    g_data_input_stream_read_line_async(stream.data, G_PRIORITY_DEFAULT, cancellable_, line_read,
                                        new Ref(shared_from_this()));
}

void Process::line_read(GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<Ref> ref(static_cast<Ref*>(data));
    Process& p = **ref;
    Stream& stream = G_DATA_INPUT_STREAM(source) == p.out_.data ? p.out_ : p.err_;
    gsize length = 0;
    GError* failure = nullptr;
    char* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, &failure);
    g_clear_error(&failure);
    if (!line) {
        stream.done = true;
        p.finish();
        return;
    }
    const std::string text(line, length);
    g_free(line);
    if (p.handlers_.line && !p.reported_) p.handlers_.line(text, &stream == &p.err_);
    p.read(stream);
}

void Process::waited(GObject*, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<Ref> ref(static_cast<Ref*>(data));
    Process& p = **ref;
    GError* failure = nullptr;
    g_subprocess_wait_finish(p.subprocess_, result, &failure);
    g_clear_error(&failure);
    p.exited_ = true;
    if (g_subprocess_get_if_exited(p.subprocess_)) p.code_ = g_subprocess_get_exit_status(p.subprocess_);
    else if (g_subprocess_get_if_signaled(p.subprocess_)) p.code_ = 128 + g_subprocess_get_term_sig(p.subprocess_);
    else p.code_ = 1;
    if (p.stopping_) {
        // Stopped: nothing it started stays (a hidden Tekken 7 that an
        // importer which did not clean up left running, say).
        if (p.overdue_) g_source_remove(p.overdue_);
        p.overdue_ = 0;
        p.kill_group();
    }
    p.finish();
    // A program it started that is still running may hold its output open
    // (Wine's server): what was written is read soon after the exit.
    if (!p.reported_) {
        g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 3, drained, new Ref(*ref),
                                   [](gpointer r) { delete static_cast<Ref*>(r); });
    }
}

gboolean Process::drained(gpointer data) {
    Process& p = **static_cast<Ref*>(data);
    g_cancellable_cancel(p.cancellable_); // the reads come back cancelled: their streams are done
    return G_SOURCE_REMOVE;
}

void Process::finish() {
    if (reported_ || !exited_ || !out_.done || !err_.done) return;
    reported_ = true;
    if (handlers_.exited) handlers_.exited(code_);
}

} // namespace launcher
