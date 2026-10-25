// The task panel at the window's bottom (see launcher.h): the one task that
// runs (installing, updating, adding or removing characters, uninstalling,
// updating the launcher),
// its step and progress from the builder's or importer's lines, Cancel
// (SIGTERM: they stop what they started and clean up, which can take
// seconds; see Process::terminate), and its log in a list that opens.

#include "launcher.h"

#include "widgets.h"

#include <csignal>
#include <cstdlib>
#include <cstring>

namespace launcher {
namespace {

constexpr int kLogLines = 5000; // the log keeps the newest

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) out += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    return out;
}

} // namespace

void Launcher::build_task_panel(GtkWidget* parent) {
    task_revealer_ = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(task_revealer_), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    // (The panel spans the window; what is in it lines up with the column.)
    GtkWidget* panel = ui::box(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(panel, 24);
    gtk_widget_set_margin_end(panel, 24);

    GtkWidget* top = ui::box(GTK_ORIENTATION_HORIZONTAL, 14);
    task_spinner_ = gtk_spinner_new();
    gtk_widget_set_valign(task_spinner_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(top), task_spinner_);
    GtkWidget* text = ui::box(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(text, TRUE);
    task_title_ = ui::label("", "task-title");
    task_detail_ = ui::label("", "small dim");
    gtk_box_append(GTK_BOX(text), task_title_);
    gtk_box_append(GTK_BOX(text), task_detail_);
    gtk_box_append(GTK_BOX(top), text);
    task_button_ = ui::button("Cancel");
    ui::set_id(task_button_, "task");
    gtk_widget_set_size_request(task_button_, 96, -1);
    clicked(task_button_, [](Launcher* self) {
        if (self->task_) self->cancel_task();
        else self->close_task_panel();
    });
    gtk_box_append(GTK_BOX(top), task_button_);
    gtk_box_append(GTK_BOX(panel), top);

    task_progress_ = gtk_progress_bar_new();
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(task_progress_), 0.04);
    gtk_box_append(GTK_BOX(panel), task_progress_);

    log_expander_ = gtk_expander_new("Details");
    log_buffer_ = gtk_text_buffer_new(nullptr);
    log_view_ = gtk_text_view_new_with_buffer(log_buffer_);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view_), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(log_view_), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(log_view_), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(log_view_), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(log_view_), 8);
    gtk_widget_add_css_class(log_view_, "log");
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), log_view_);
    gtk_widget_set_size_request(scroller, -1, 200);
    gtk_widget_add_css_class(scroller, "log-frame");
    gtk_widget_set_margin_top(scroller, 8);
    gtk_expander_set_child(GTK_EXPANDER(log_expander_), scroller);
    gtk_box_append(GTK_BOX(panel), log_expander_);

    GtkWidget* background = ui::box(GTK_ORIENTATION_VERTICAL, 0, "task-panel");
    gtk_box_append(GTK_BOX(background), ui::clamp_new(panel, kColumn));
    gtk_revealer_set_child(GTK_REVEALER(task_revealer_), background);
    gtk_box_append(GTK_BOX(parent), task_revealer_);
}

void Launcher::begin_task(Kind kind, std::string title) {
    ++result_serial_;
    task_ = std::make_unique<Task>();
    task_->kind = kind;
    task_->title = std::move(title);
    task_->progress = Progress(kind == Kind::kImport || kind == Kind::kRemove ? Progress::Kind::kImport
                               : kind == Kind::kLauncherUpdate                 ? Progress::Kind::kDownload
                                                                               : Progress::Kind::kBuild);
    if (status_) {
        std::map<std::string, std::string> names;
        for (const Fighter& f : status_->fighters) names[f.name] = f.display;
        task_->progress.set_names(std::move(names));
    }
    gtk_text_buffer_set_text(log_buffer_, "", 0);
    ui::set_text(task_title_, task_->title);
    gtk_widget_remove_css_class(task_title_, "error-text");
    gtk_label_set_text(GTK_LABEL(task_detail_), "Starting…");
    gtk_widget_set_visible(task_detail_, TRUE);
    gtk_widget_set_visible(task_progress_, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(task_progress_), 0);
    gtk_widget_set_visible(task_spinner_, TRUE);
    gtk_spinner_start(GTK_SPINNER(task_spinner_));
    gtk_button_set_label(GTK_BUTTON(task_button_), "Cancel");
    gtk_widget_set_sensitive(task_button_, kind != Kind::kUninstall);
    gtk_revealer_set_reveal_child(GTK_REVEALER(task_revealer_), TRUE);
    if (!pulse_source_) {
        pulse_source_ = g_timeout_add(100, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Launcher*>(data);
            if (self->task_ && self->task_->progress.fraction() < 0) {
                gtk_progress_bar_pulse(GTK_PROGRESS_BAR(self->task_progress_));
            }
            return G_SOURCE_CONTINUE;
        }, this);
    }
    show_all();
    // The button pressed (Add, Update ...) cannot be pressed during the task:
    // the keyboard's place is Cancel then, rather than nowhere.
    if (!gtk_root_get_focus(GTK_ROOT(window_)) && kind != Kind::kUninstall) gtk_widget_grab_focus(task_button_);
}

bool Launcher::start_task(Kind kind, std::string title, const std::vector<std::string>& argv,
                          const Process::Environment& environment, std::function<void(Task&, int)> ended) {
    if (task_) {
        refuse("Another task is running", "Wait for it to end.");
        return false;
    }
    begin_task(kind, std::move(title));
    task_->ended = std::move(ended);
    log("$ " + command_text(argv));
    std::string error;
    task_->process = Process::start(argv, environment,
                                    {[this](const std::string& line, bool is_error) { task_line(line, is_error); },
                                     [this](int code) { task_exited(code); }},
                                    &error);
    if (!task_->process) {
        finish_task(false, task_->title + ": cannot start", error);
        return false;
    }
    // (TTT2_LAUNCHER_TEST_CANCEL_FILE=<file>: tests press Cancel once that
    // file is there, made by a script that watches the task, for a step that
    // prints nothing meanwhile, such as Tekken 7's hidden run.)
    if (const char* file = std::getenv("TTT2_LAUNCHER_TEST_CANCEL_FILE"); file && *file && test_running_) {
        struct Watch {
            Launcher* self;
            int serial;
            fs::path file;
        };
        g_timeout_add_full(G_PRIORITY_DEFAULT, 250, +[](gpointer data) -> gboolean {
            const auto* watch = static_cast<Watch*>(data);
            Launcher* self = watch->self;
            if (self->result_serial_ != watch->serial || !self->task_ || self->task_->cancelled) return G_SOURCE_REMOVE;
            std::error_code ec;
            if (!fs::exists(watch->file, ec)) return G_SOURCE_CONTINUE;
            self->cancel_task();
            self->save_shot(self->window_, self->tests_[self->next_test_ - 1].label() + "-stopping");
            return G_SOURCE_REMOVE;
        }, new Watch{this, result_serial_, file}, +[](gpointer data) { delete static_cast<Watch*>(data); });
    }
    return true;
}

std::string Launcher::command_text(const std::vector<std::string>& argv) {
    std::string command;
    for (const std::string& a : argv) {
        const std::string shown = ui::shown(a);
        command += (command.empty() ? "" : " ") + (a.find(' ') == std::string::npos ? shown : "\"" + shown + "\"");
    }
    return command;
}

void Launcher::log(const std::string& line) {
    GtkAdjustment* adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(log_view_));
    const bool at_end = gtk_adjustment_get_value(adjustment) + gtk_adjustment_get_page_size(adjustment) >=
                        gtk_adjustment_get_upper(adjustment) - 4;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(log_buffer_, &end);
    gtk_text_buffer_insert(log_buffer_, &end, (ui::valid(line) + "\n").c_str(), -1);
    if (gtk_text_buffer_get_line_count(log_buffer_) > kLogLines + 500) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(log_buffer_, &start);
        gtk_text_buffer_get_iter_at_line(log_buffer_, &cut, 500);
        gtk_text_buffer_delete(log_buffer_, &start, &cut);
    }
    if (at_end) {
        gtk_text_buffer_get_end_iter(log_buffer_, &end);
        GtkTextMark* mark = gtk_text_buffer_create_mark(log_buffer_, nullptr, &end, FALSE);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_view_), mark);
        gtk_text_buffer_delete_mark(log_buffer_, mark);
    }
}

void Launcher::task_line(const std::string& text, bool error) {
    log(text);
    if (error || !task_) return;
    const Line line = parse_line(text);
    switch (line.kind) {
    case Line::Kind::kError: task_->error = line.text; break;
    case Line::Kind::kFolder:
        // ttt2 --install chose this folder (one inside the chosen, say):
        // the record must name it, by the bytes of its path as they are.
        if (task_->kind == Kind::kInstall) {
            if (const auto started = read_started_record(places_); started && started->folder != line.text) {
                write_started_record(places_, line.text, started->game);
            }
        }
        break;
    case Line::Kind::kAdded:
    case Line::Kind::kRemoved: task_->added.push_back(line.word); break;
    case Line::Kind::kSkipped: task_->skipped.push_back(line.word + ": " + line.text); break;
    default: break;
    }
    if (task_->progress.feed(line)) show_task();
}

void Launcher::show_task() {
    const Progress& p = task_->progress;
    std::string detail = p.step();
    if (!p.detail().empty()) detail += (detail.empty() ? "" : " · ") + p.detail();
    if (p.fraction() >= 0) {
        detail += (detail.empty() ? "" : " · ") + std::to_string(static_cast<int>(p.fraction() * 100)) + "%";
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(task_progress_), p.fraction());
    }
    if (!task_->cancelled) ui::set_text(task_detail_, detail); // (else it says that it stops)
    if (test_running_ && !task_->shot_running && p.fraction() >= 0.5) {
        task_->shot_running = true;
        save_shot(window_, tests_[next_test_ - 1].label() + "-running");
    }
    // (TTT2_LAUNCHER_TEST_CANCEL=<fraction>: tests press Cancel there, -1 at
    // the first step, and see it stopping.)
    const char* cancel_at = std::getenv("TTT2_LAUNCHER_TEST_CANCEL");
    if (cancel_at && test_running_ && !task_->cancelled && p.fraction() >= std::atof(cancel_at)) {
        cancel_task();
        save_shot(window_, tests_[next_test_ - 1].label() + "-stopping");
    }
}

void Launcher::cancel_task() {
    if (!task_ || !task_->process || task_->cancelled) return;
    task_->cancelled = true;
    task_->process->terminate();
    gtk_button_set_label(GTK_BUTTON(task_button_), "Stopping…");
    gtk_widget_set_sensitive(task_button_, FALSE);
    ui::set_text(task_detail_, task_->kind == Kind::kImport
                                   ? "Stopping… Tekken 7 is closed first when it runs hidden: a few seconds."
                                   : "Stopping…");
    log("Stopping…");
}

void Launcher::task_exited(int code) {
    Task& t = *task_;
    if (t.cancelled && code == 128 + SIGKILL) {
        log("It did not stop within " + std::to_string(Process::kStopSeconds) +
            " seconds: it and what it started were stopped at once.");
    }
    if (t.ended) t.ended(t, code);
    const bool ok = code == 0 && !t.cancelled;
    std::string title, detail;
    std::string why = t.error;
    if (why.empty()) {
        // (Process: 128 + the signal that ended it, as a shell says; a crash
        // before it could say why, such as an instruction this processor
        // does not have, then has at least a name.)
        why = code > 128 && code < 128 + NSIG
                  ? std::string("It was stopped: ") + ::strsignal(code - 128) + " (code " + std::to_string(code) +
                        ", see Details)"
                  : "It stopped with code " + std::to_string(code) + " (see Details)";
    }
    if (why[0] >= 'a' && why[0] <= 'z') why[0] = static_cast<char>(why[0] - 'a' + 'A'); // (as a sentence)
    std::vector<std::string> added;
    for (const std::string& name : t.added) {
        std::string display = name;
        if (status_) {
            for (const Fighter& f : status_->fighters) {
                if (f.name == name) display = f.display;
            }
        }
        added.push_back(display);
    }
    switch (t.kind) {
    case Kind::kInstall:
        if (ok) {
            title = "Installed";
            detail = "Play it from here, or as Tekken Tag Tournament 2 in your application menu.";
        } else if (t.cancelled) {
            std::error_code ec;
            title = "Install cancelled";
            detail = fs::exists(installing_ / kStarted, ec)
                         ? "Resume it any time: the game's files copied so far are kept."
                         : "It stopped before copying the game: nothing was installed.";
        } else {
            title = "Install failed";
            detail = why;
        }
        break;
    case Kind::kUpdate:
        title = ok ? "Updated" : t.cancelled ? "Update cancelled" : "Update failed";
        detail = ok ? "The game program is built again for this PC." : t.cancelled ? "The game is as it was." : why;
        break;
    case Kind::kImport:
        if (ok) {
            title = added.empty() ? "No characters added" : "Added " + joined(added);
            detail = added.empty() ? "" : "They are on the select screen next time the game starts.";
        } else {
            title = t.cancelled ? "Adding cancelled" : "Adding " + t.subject + " failed";
            detail = t.cancelled ? "The characters added before stay." : why;
        }
        if (!t.skipped.empty()) detail += (detail.empty() ? "" : " ") + ("Skipped " + joined(t.skipped) + ".");
        break;
    case Kind::kRemove:
        title = ok ? "Removed " + t.subject : t.cancelled ? "Removing cancelled" : "Removing " + t.subject + " failed";
        detail = ok ? "They are gone from the select screen next time the game starts." : why;
        break;
    case Kind::kUninstall:
    case Kind::kLauncherUpdate: break; // (their own)
    }
    finish_task(ok, title, detail, t.cancelled);
}

void Launcher::finish_task(bool ok, const std::string& title, const std::string& detail, bool cancelled) {
    gtk_spinner_stop(GTK_SPINNER(task_spinner_));
    gtk_widget_set_visible(task_spinner_, FALSE);
    gtk_widget_set_visible(task_progress_, FALSE);
    ui::set_text(task_title_, title);
    if (ok || cancelled) gtk_widget_remove_css_class(task_title_, "error-text");
    else gtk_widget_add_css_class(task_title_, "error-text");
    ui::set_text(task_detail_, detail);
    gtk_widget_set_visible(task_detail_, !detail.empty());
    gtk_button_set_label(GTK_BUTTON(task_button_), "Close");
    gtk_widget_set_sensitive(task_button_, TRUE);
    // (Cancel pressed with the keyboard lost the focus as it was greyed out
    // while stopping: it comes back to the same button, now Close.)
    if (!gtk_root_get_focus(GTK_ROOT(window_))) gtk_widget_grab_focus(task_button_);
    log(ok ? "Done." : title + (detail.empty() ? "" : ": " + detail));
    task_.reset();
    if (pulse_source_) g_source_remove(pulse_source_);
    pulse_source_ = 0;
    installing_.clear();
    // A success says so for a while; a failure stays until closed.
    const int serial = ++result_serial_;
    if (ok) {
        struct Shown {
            Launcher* self;
            int serial;
        };
        g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 12, +[](gpointer data) -> gboolean {
            const auto* shown = static_cast<Shown*>(data);
            Launcher* self = shown->self;
            if (self->result_serial_ == shown->serial && !self->task_ &&
                !gtk_expander_get_expanded(GTK_EXPANDER(self->log_expander_))) {
                self->close_task_panel();
            }
            return G_SOURCE_REMOVE;
        }, new Shown{this, serial}, +[](gpointer data) { delete static_cast<Shown*>(data); });
    }
    if (quit_after_task_) {
        g_application_quit(G_APPLICATION(app_));
        return;
    }
    test_done(ok, ok ? title : title + (detail.empty() ? "" : ": " + detail));
    refresh();
    settled();
}

void Launcher::close_task_panel() {
    gtk_revealer_set_reveal_child(GTK_REVEALER(task_revealer_), FALSE);
}

} // namespace launcher
