// The game's section (see launcher.h): installing from the player's .wua
// (the file and folder pickers, the checks of the PC, then ttt2-build
// install), resuming, updating, playing and uninstalling.

#include "launcher.h"

#include "system.h"
#include "widgets.h"

#include <cctype>
#include <csignal>

namespace launcher {
namespace {

// What an install needs, about: the game (15.9 GB) and its program; the
// builder's work folder (the generated C++, its objects and the program).
constexpr uint64_t kGameBytes = 16'200'000'000;
constexpr uint64_t kBuildBytes = 3'000'000'000;

bool there(const fs::path& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(path, ec);
}

fs::path home() {
    const char* h = std::getenv("HOME");
    return h && *h ? fs::path(h) : fs::path("/");
}

// The card's top: the game's name, its folder, and a pill with its state.
GtkWidget* card_header(const fs::path& folder, const char* state, const char* state_class) {
    GtkWidget* row = ui::box(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* names = ui::box(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(names, TRUE);
    gtk_box_append(GTK_BOX(names), ui::label("Tekken Tag Tournament 2", "card-title"));
    if (!folder.empty()) {
        GtkWidget* path = ui::label(ui::shown(folder), "path", false);
        ui::set_tooltip(path, ui::shown(folder));
        gtk_box_append(GTK_BOX(names), path);
    }
    gtk_box_append(GTK_BOX(row), names);
    GtkWidget* pill = ui::label(state, (std::string("pill ") + state_class).c_str(), false);
    gtk_widget_set_valign(pill, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), pill);
    return row;
}

// A button with its id, to keep the keyboard focus on it when the card is
// shown again (ui::Rebuild).
GtkWidget* button(const char* id, const char* text, const char* classes = nullptr, const char* icon = nullptr) {
    GtkWidget* b = ui::button(text, classes, icon);
    ui::set_id(b, id);
    return b;
}

GtkWidget* button_row() {
    GtkWidget* row = ui::box(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(row, 4);
    return row;
}

GtkWidget* spacer() {
    GtkWidget* s = ui::box(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(s, TRUE);
    return s;
}

// A line with an icon: a check's result, a note.
GtkWidget* icon_row(const char* icon, const char* icon_class, GtkWidget* text) {
    GtkWidget* row = ui::box(GTK_ORIENTATION_HORIZONTAL, 12, "check-row");
    GtkWidget* image = gtk_image_new_from_icon_name(icon);
    ui::add_classes(image, icon_class);
    gtk_widget_set_valign(image, GTK_ALIGN_START);
    gtk_widget_set_margin_top(image, 2);
    gtk_box_append(GTK_BOX(row), image);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_box_append(GTK_BOX(row), text);
    return row;
}

// ---------------------------------------------------------------- checks

struct PreflightJob {
    fs::path game, folder, work;
    std::vector<Check> checks;
};

std::vector<Check> run_checks(const fs::path& game, const fs::path& folder, const fs::path& work) {
    std::vector<Check> checks;
    std::error_code ec;
    if (fs::is_regular_file(game, ec) || fs::is_directory(game, ec)) {
        checks.push_back({true, "Game", ui::shown(game.filename()), {}});
    } else {
        checks.push_back({false, "Game", ui::shown(game) + " does not exist", {}});
    }

    const auto missing = missing_cpu_features();
    if (missing.empty()) {
        checks.push_back({true, "Processor", "Has what the game program is built for (x86-64-v3)", {}});
    } else {
        std::string names;
        for (size_t i = 0; i < missing.size() && i < 6; ++i) names += (i ? ", " : "") + missing[i];
        checks.push_back({false, "Processor",
                          "Lacks " + names + (missing.size() > 6 ? " and more" : "") +
                              ": the game program needs an x86-64-v3 processor (Intel Haswell, AMD Excavator or newer)",
                          {}});
    }

    std::string why;
    if (const auto compiler = find_compiler(&why)) {
        checks.push_back({true, "C++ compiler", compiler->name + " (" + ui::shown(compiler->path) + ")", {}});
    } else {
        const Distro distro = this_distro();
        const std::string command = install_command(distro, {Package::kCompiler});
        if (!why.empty()) why[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(why[0])));
        why += ". The game program is compiled on this PC";
        if (image_based(distro)) why += ". " + image_based_hint("Clang (or GCC 15 or newer)");
        else if (command.empty()) why += ": install Clang (or GCC 15 or newer) with your distribution's packages.";
        else why += ": install Clang with";
        checks.push_back({false, "C++ compiler", why, command});
    }

    // Space for the game (what is not copied yet) and for building, on one
    // file system or two.
    uint64_t needed = kGameBytes - std::min(kGameBytes - 300'000'000, size_of(folder / "game"));
    const bool together = file_system(folder) == file_system(work);
    if (together) needed += kBuildBytes;
    const auto space = [](const fs::path& where, uint64_t bytes, const char* title, const char* what) {
        const auto free = free_space(where);
        if (!free) return Check{true, title, "Unknown for " + ui::shown(where), {}};
        return Check{*free >= bytes, title,
                     gigabytes(*free) + " free, about " + gigabytes(bytes) + " needed " + what +
                         (*free >= bytes ? "" : ": make room on that disk, or choose a folder on another"),
                     {}};
    };
    checks.push_back(
        space(folder, needed, "Free space", together ? "(the game, and building its program)" : "(the game)"));
    if (!together) {
        checks.push_back(space(work, kBuildBytes, "Free space for building", ("in " + ui::shown(work)).c_str()));
    }
    return checks;
}

} // namespace

// ---------------------------------------------------------------- the section

void Launcher::show_game() {
    // (Settings is there for an installed game only.)
    if (install_.state != InstallState::kInstalled) close_settings();
    const ui::Rebuild rebuild(game_card_);
    if (preflight_) show_preflight(game_card_);
    else if (task_ && task_->kind == Kind::kInstall && !installing_.empty()) game_installing(game_card_);
    else if (install_.state == InstallState::kInstalled) game_installed(game_card_);
    else if (install_.state == InstallState::kUnfinished) game_unfinished(game_card_);
    else game_not_installed(game_card_);
}

bool Launcher::game_running() const {
    return (game_process_ && game_process_->running()) ||
           (install_.state != InstallState::kNone && program_running(install_.folder));
}

void Launcher::watch_game() {
    if (game_watch_ || game_process_) return; // (one started here says when it ends)
    game_watch_ = g_timeout_add_seconds(2, +[](gpointer data) -> gboolean {
        auto* self = static_cast<Launcher*>(data);
        if (self->game_running()) return G_SOURCE_CONTINUE;
        self->game_watch_ = 0;
        self->show_game();
        return G_SOURCE_REMOVE;
    }, this);
}

void Launcher::game_installed(GtkWidget* card) {
    const bool playing = game_running();
    if (playing) watch_game();
    const char* state = playing ? "Running"
                        : task_ && task_->kind == Kind::kUpdate    ? "Updating"
                        : task_ && task_->kind == Kind::kUninstall ? "Uninstalling"
                                                                   : "Installed";
    gtk_box_append(GTK_BOX(card), card_header(install_.folder, state, task_ && !playing ? "neutral" : "ok"));
    if (playing && !game_process_) {
        gtk_box_append(GTK_BOX(card),
                       icon_row("dialog-information-symbolic", "dim",
                                ui::label("It was started outside the launcher. Quit it to update or uninstall it.",
                                          "small dim")));
    }

    const std::string built = built_version(install_.folder);
    const bool update = !kit_version_.empty() && built != kit_version_;
    if (update) {
        // (An install made with ttt2 --install has no record of what built
        // it: Update is offered, not urged.)
        const std::string text =
            built.empty() ? "This game program was not built by the launcher. Update builds it again from the game in "
                            "its folder."
                          : "An update of the game program is ready (" + kit_version_ + ", installed " + built +
                                "). Update builds it again from the game in its folder: a few minutes.";
        gtk_box_append(GTK_BOX(card), icon_row("dialog-information-symbolic", "warn", ui::label(text, "small dim")));
    }
    if (!game_error_.empty()) gtk_box_append(GTK_BOX(card), ui::label(game_error_, "error-text"));

    GtkWidget* row = button_row();
    GtkWidget* play_button =
        button("play", playing ? "Playing" : "Play", "suggested-action play", "media-playback-start-symbolic");
    gtk_widget_set_sensitive(play_button, !playing && !task_);
    clicked(play_button, [](Launcher* self) { self->play(); });
    gtk_box_append(GTK_BOX(row), play_button);
    // Settings, right of Play: its icon alone (keys can change while the
    // game runs, and during a task).
    GtkWidget* settings = gtk_button_new_from_icon_name("ttt2-settings-symbolic");
    ui::set_id(settings, "settings");
    gtk_widget_set_tooltip_text(settings, "Settings");
    gtk_accessible_update_property(GTK_ACCESSIBLE(settings), GTK_ACCESSIBLE_PROPERTY_LABEL, "Settings", -1);
    gtk_widget_set_valign(settings, GTK_ALIGN_CENTER);
    clicked(settings, [](Launcher* self) { self->open_settings(); });
    gtk_box_append(GTK_BOX(row), settings);
    if (update) {
        GtkWidget* update_button = button("update", "Update");
        gtk_widget_set_tooltip_text(update_button, "Build the game program again with this launcher's version");
        gtk_widget_set_sensitive(update_button, !playing && !task_ && there(kit_ / "bin/ttt2-build"));
        clicked(update_button, [](Launcher* self) { self->start_update(); });
        gtk_box_append(GTK_BOX(row), update_button);
    }
    gtk_box_append(GTK_BOX(row), spacer());
    GtkWidget* open = gtk_button_new_from_icon_name("folder-symbolic");
    ui::set_id(open, "open-folder");
    gtk_widget_set_tooltip_text(open, "Open the install folder");
    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
    clicked(open, [](Launcher* self) {
        GFile* folder = g_file_new_for_path(self->install_.folder.c_str());
        GtkFileLauncher* launcher = gtk_file_launcher_new(folder);
        gtk_file_launcher_launch(launcher, GTK_WINDOW(self->window_), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
        g_object_unref(folder);
    });
    gtk_box_append(GTK_BOX(row), open);
    GtkWidget* uninstall_button = button("uninstall", "Uninstall…", "uninstall");
    gtk_widget_set_sensitive(uninstall_button, !playing && !task_);
    clicked(uninstall_button, [](Launcher* self) { self->ask_uninstall(); });
    gtk_box_append(GTK_BOX(row), uninstall_button);
    gtk_box_append(GTK_BOX(card), row);
}

void Launcher::game_unfinished(GtkWidget* card) {
    gtk_box_append(GTK_BOX(card), card_header(install_.folder, "Not finished", "warn"));
    gtk_box_append(GTK_BOX(card), ui::label("The install stopped before it was complete. Resume it to finish: the "
                                            "files copied so far are kept.",
                                            "dim"));
    if (!game_error_.empty()) gtk_box_append(GTK_BOX(card), ui::label(game_error_, "error-text"));
    GtkWidget* row = button_row();
    GtkWidget* resume_button = button("resume", "Resume", "suggested-action");
    gtk_widget_set_sensitive(resume_button, !task_ && there(kit_ / "bin/ttt2-build"));
    clicked(resume_button, [](Launcher* self) { self->resume(); });
    gtk_box_append(GTK_BOX(row), resume_button);
    gtk_box_append(GTK_BOX(row), spacer());
    GtkWidget* uninstall_button = button("uninstall", "Uninstall…", "uninstall");
    gtk_widget_set_sensitive(uninstall_button, !task_);
    clicked(uninstall_button, [](Launcher* self) { self->ask_uninstall(); });
    gtk_box_append(GTK_BOX(row), uninstall_button);
    gtk_box_append(GTK_BOX(card), row);
}

void Launcher::game_not_installed(GtkWidget* card) {
    gtk_box_append(GTK_BOX(card), card_header("", "Not installed", "neutral"));
    gtk_box_append(GTK_BOX(card),
                   ui::label("Install it from your own copy of TEKKEN TAG TOURNAMENT 2 Wii U EDITION (European, with "
                             "update 16) as a .wua archive. The launcher builds the game program for this PC from it, "
                             "then copies the game into a folder you choose (about 16 GB).",
                             "dim"));
    if (!chosen_game_.empty()) {
        GtkWidget* text = ui::box(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(text), ui::label(ui::shown(chosen_game_.filename()), nullptr, false));
        gtk_box_append(GTK_BOX(text), ui::label(ui::shown(chosen_game_.parent_path()), "path", false));
        gtk_box_append(GTK_BOX(card), icon_row("document-open-symbolic", "dim", text));
    }
    const bool kit = there(kit_ / "bin/ttt2-build");
    if (!kit) {
        gtk_box_append(GTK_BOX(card), ui::label("The game builder is missing from this launcher" +
                                                    (kit_.empty() ? std::string() : " (" + ui::shown(kit_) + ")") +
                                                    ": the game cannot be installed.",
                                                "error-text"));
    }
    if (!game_error_.empty()) gtk_box_append(GTK_BOX(card), ui::label(game_error_, "error-text"));
    GtkWidget* row = button_row();
    GtkWidget* install_button = button("install", "Install…", "suggested-action");
    gtk_widget_set_sensitive(install_button, kit && !task_);
    clicked(install_button, [](Launcher* self) {
        self->choose_game([self](const fs::path& game) {
            self->chosen_game_ = game;
            self->choose_folder();
        });
    });
    gtk_box_append(GTK_BOX(row), install_button);
    gtk_box_append(GTK_BOX(card), row);
}

void Launcher::game_installing(GtkWidget* card) {
    gtk_box_append(GTK_BOX(card), card_header(installing_, "Installing", "neutral"));
    gtk_box_append(GTK_BOX(card), ui::label("The game program is built for this PC from your game, then the game is "
                                            "copied into this folder. Stopping keeps what is copied: the install "
                                            "resumes from there.",
                                            "dim"));
}

void Launcher::show_preflight(GtkWidget* card) {
    const Preflight& p = *preflight_;
    gtk_box_append(GTK_BOX(card), card_header(p.folder, p.running ? "Checking" : "Cannot install yet",
                                              p.running ? "neutral" : "bad"));
    if (p.running) {
        GtkWidget* row = ui::box(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget* spinner = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_box_append(GTK_BOX(row), spinner);
        gtk_box_append(GTK_BOX(row), ui::label("Checking this PC…", "dim"));
        gtk_box_append(GTK_BOX(card), row);
        return;
    }
    gtk_box_append(GTK_BOX(card), ui::label("Before the install can start:", "dim"));
    GtkWidget* list = ui::box(GTK_ORIENTATION_VERTICAL, 12);
    for (const Check& c : p.checks) {
        GtkWidget* text = ui::box(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append(GTK_BOX(text), ui::label(c.title, "heading"));
        gtk_box_append(GTK_BOX(text), ui::label(c.detail, c.ok ? "small dim" : "small"));
        if (!c.command.empty()) gtk_box_append(GTK_BOX(text), ui::command(c.command));
        gtk_box_append(GTK_BOX(list), icon_row(c.ok ? "object-select-symbolic" : "dialog-error-symbolic",
                                               c.ok ? "ok" : "bad", text));
    }
    gtk_box_append(GTK_BOX(card), list);
    GtkWidget* row = button_row();
    GtkWidget* again = button("check-again", "Check Again", "suggested-action");
    clicked(again, [](Launcher* self) {
        self->preflight_->running = true;
        self->show_game();
        self->run_preflight();
    });
    gtk_box_append(GTK_BOX(row), again);
    GtkWidget* cancel = button("preflight-cancel", "Cancel");
    clicked(cancel, [](Launcher* self) {
        self->preflight_.reset();
        self->show_game();
    });
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(card), row);
}

// ---------------------------------------------------------------- choosing

namespace {

struct Chosen {
    Launcher* self;
    std::function<void(const fs::path&)> then;
};

void chosen(GFile* file, gpointer data) {
    const std::unique_ptr<Chosen> c(static_cast<Chosen*>(data));
    if (!file) return; // cancelled
    char* path = g_file_get_path(file);
    g_object_unref(file);
    if (path) c->then(path);
    g_free(path);
}

void file_chosen(GObject* dialog, GAsyncResult* result, gpointer data) {
    chosen(gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, nullptr), data);
}

void folder_chosen(GObject* dialog, GAsyncResult* result, gpointer data) {
    chosen(gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(dialog), result, nullptr), data);
}

} // namespace

void Launcher::choose_game(std::function<void(const fs::path&)> then) {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose the game's .wua");
    gtk_file_dialog_set_modal(dialog, TRUE);
    GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter* wua = gtk_file_filter_new();
    gtk_file_filter_set_name(wua, "Wii U game archive (.wua)");
    gtk_file_filter_add_suffix(wua, "wua");
    g_list_store_append(filters, wua);
    GtkFileFilter* all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, wua);
    g_object_unref(wua);
    g_object_unref(all);
    g_object_unref(filters);
    GFile* start = nullptr;
    if (there(chosen_game_)) {
        start = g_file_new_for_path(chosen_game_.c_str());
        gtk_file_dialog_set_initial_file(dialog, start);
    } else {
        start = g_file_new_for_path(home().c_str());
        gtk_file_dialog_set_initial_folder(dialog, start);
    }
    g_object_unref(start);
    gtk_file_dialog_open(dialog, GTK_WINDOW(window_), nullptr, file_chosen, new Chosen{this, std::move(then)});
    g_object_unref(dialog);
}

void Launcher::choose_folder() {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose where to install Tekken Tag Tournament 2");
    gtk_file_dialog_set_accept_label(dialog, "Install Here");
    gtk_file_dialog_set_modal(dialog, TRUE);
    const fs::path start_path = there(home() / "Games") ? home() / "Games" : home();
    GFile* start = g_file_new_for_path(start_path.c_str());
    gtk_file_dialog_set_initial_folder(dialog, start);
    g_object_unref(start);
    auto then = [this](const fs::path& folder) { start_install(chosen_game_, folder); };
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(window_), nullptr, folder_chosen, new Chosen{this, then});
    g_object_unref(dialog);
}

// ---------------------------------------------------------------- installing

void Launcher::refuse(const std::string& title, const std::string& why) {
    if (task_) {
        GtkAlertDialog* dialog = gtk_alert_dialog_new("%s", ui::valid(title).c_str());
        gtk_alert_dialog_set_detail(dialog, ui::valid(why).c_str());
        gtk_alert_dialog_show(dialog, GTK_WINDOW(window_));
        g_object_unref(dialog);
        test_done(false, why);
        return;
    }
    begin_task(Kind::kInstall, title);
    finish_task(false, title, why);
}

void Launcher::start_install(const fs::path& game, const fs::path& chosen) {
    if (task_) return refuse("Another task is running", "Wait for it to end, then install.");
    if (!there(kit_ / "bin/ttt2-build")) {
        return refuse("Cannot install", "The game builder is missing from this launcher.");
    }
    std::string error;
    const auto folder = install_target(chosen, &error);
    if (!folder) {
        return refuse("Cannot install into this folder",
                      ui::shown(normal(chosen)) + " " + error + ": choose another folder.");
    }
    chosen_game_ = game;
    game_error_.clear();
    preflight_ = Preflight{game, *folder, true, {}};
    show_game();
    run_preflight();
}

void Launcher::run_preflight() {
    auto* job = new PreflightJob{preflight_->game, preflight_->folder, places_.build_work(), {}};
    GTask* task = g_task_new(nullptr, nullptr, +[](GObject*, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<Launcher*>(data);
        auto* job = static_cast<PreflightJob*>(g_task_get_task_data(G_TASK(result)));
        if (self->preflight_ && self->preflight_->folder == job->folder) {
            self->preflight_->checks = job->checks;
            self->preflight_done();
        }
    }, this);
    g_task_set_task_data(task, job, [](gpointer job) { delete static_cast<PreflightJob*>(job); });
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
        auto* job = static_cast<PreflightJob*>(data);
        job->checks = run_checks(job->game, job->folder, job->work);
        g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
}

void Launcher::preflight_done() {
    preflight_->running = false;
    for (const Check& c : preflight_->checks) {
        if (!c.ok) {
            show_game();
            test_done(false, c.title + ": " + c.detail);
            settled();
            return;
        }
    }
    begin_install();
}

void Launcher::begin_install() {
    const fs::path game = preflight_->game, folder = preflight_->folder;
    preflight_.reset();
    write_started_record(places_, folder, game);
    installing_ = folder;
    start_task(Kind::kInstall, "Installing Tekken Tag Tournament 2",
               {(kit_ / "bin/ttt2-build").string(), "install", game.string(), folder.string()}, {},
               [this](Task& task, int code) {
                   // Done, or stopped before ttt2 --install began (while the
                   // program was built): there is nothing to resume.
                   const auto started = read_started_record(places_);
                   std::error_code ec;
                   if ((code == 0 && !task.cancelled) || (started && !fs::exists(started->folder / kStarted, ec))) {
                       fs::remove(places_.started_record(), ec);
                   }
               });
}

void Launcher::resume() {
    const fs::path folder = install_.folder;
    if (there(install_.game)) return start_install(install_.game, folder);
    // (The launcher did not start it, or the archive has moved.)
    choose_game([this, folder](const fs::path& game) { start_install(game, folder); });
}

void Launcher::start_update() {
    if (!there(kit_ / "bin/ttt2-build")) {
        return refuse("Cannot update", "The game builder is missing from this launcher.");
    }
    if (game_running()) return refuse("The game is running", "Quit it, then update.");
    game_error_.clear();
    start_task(Kind::kUpdate, "Updating the game program",
               {(kit_ / "bin/ttt2-build").string(), "update", install_.folder.string()}, {}, nullptr);
}

void Launcher::play() {
    // (Two would share the saves and the shader cache.)
    if (game_running()) return refuse("The game is running", "Only one can run at a time.");
    const fs::path program = install_.folder / kProgramName;
    game_error_.clear();
    std::string error;
    game_process_ = Process::start({program.string()}, {},
                                   {nullptr,
                                    [this](int code) {
                                        if (code != 0 && code != 128 + SIGTERM && code != 128 + SIGINT) {
                                            game_error_ = "The game stopped with an error (exit code " +
                                                          std::to_string(code) + ").";
                                        }
                                        game_process_.reset();
                                        show_game();
                                    }},
                                   &error, false);
    if (!game_process_) game_error_ = "The game cannot start: " + error;
    show_game();
}

// ---------------------------------------------------------------- uninstalling

void Launcher::uninstall_confirmed(GtkWidget* button, gpointer data) {
    auto* self = static_cast<Launcher*>(data);
    const auto checked = [&](const char* name) {
        return gtk_check_button_get_active(GTK_CHECK_BUTTON(g_object_get_data(G_OBJECT(button), name))) != 0;
    };
    const UninstallOptions options{.saves = checked("saves"), .characters = checked("characters")};
    gtk_window_destroy(GTK_WINDOW(g_object_get_data(G_OBJECT(button), "dialog")));
    self->start_uninstall(options);
}

namespace {

gboolean escape_closes(GtkEventControllerKey*, guint key, guint, GdkModifierType, gpointer dialog) {
    if (key != GDK_KEY_Escape) return FALSE;
    gtk_window_destroy(GTK_WINDOW(dialog));
    return TRUE;
}

} // namespace

void Launcher::ask_uninstall() {
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Uninstall");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 480, -1);
    gtk_window_set_titlebar(GTK_WINDOW(dialog), ui::box(GTK_ORIENTATION_HORIZONTAL, 0)); // (no title bar)

    GtkWidget* content = ui::box(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(content, 24);
    gtk_widget_set_margin_end(content, 24);
    gtk_widget_set_margin_top(content, 24);
    gtk_widget_set_margin_bottom(content, 22);
    gtk_box_append(GTK_BOX(content), ui::label("Uninstall Tekken Tag Tournament 2?", "card-title"));
    gtk_box_append(GTK_BOX(content),
                   ui::label("This deletes the game, its program and its menu entry from this folder; anything else "
                             "in it stays:",
                             "dim"));
    gtk_box_append(GTK_BOX(content), ui::label(ui::shown(install_.folder), "path"));

    GtkWidget* checks = ui::box(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(checks, 6);
    GtkWidget* saves = gtk_check_button_new_with_label("Also delete saves");
    gtk_box_append(GTK_BOX(checks), saves);
    // Where they are (the player may have chosen the folder: then only the
    // game's own saves in it go).
    GtkWidget* where = ui::box(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(where, 30);
    gtk_widget_set_margin_bottom(where, 4);
    gtk_box_append(GTK_BOX(where), ui::label(ui::shown(normal(places_.saves)), "path"));
    std::error_code ec;
    if (normal(places_.saves) != normal(places_.default_saves()) &&
        !fs::equivalent(places_.saves, places_.default_saves(), ec)) {
        gtk_box_append(GTK_BOX(where), ui::label("Only the game's saves in it are deleted; anything else there stays.",
                                                 "small dim"));
    }
    gtk_box_append(GTK_BOX(checks), where);
    GtkWidget* characters = gtk_check_button_new_with_label("Also delete added characters and saved Tekken 7 data");
    gtk_box_append(GTK_BOX(checks), characters);
    gtk_box_append(GTK_BOX(content), checks);

    GtkWidget* row = ui::box(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 8);
    GtkWidget* cancel = ui::button("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    GtkWidget* uninstall_button = ui::button("Uninstall", "destructive-action");
    g_object_set_data(G_OBJECT(uninstall_button), "saves", saves);
    g_object_set_data(G_OBJECT(uninstall_button), "characters", characters);
    g_object_set_data(G_OBJECT(uninstall_button), "dialog", dialog);
    g_signal_connect(uninstall_button, "clicked", G_CALLBACK(uninstall_confirmed), this);
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(row), uninstall_button);
    gtk_box_append(GTK_BOX(content), row);
    gtk_window_set_child(GTK_WINDOW(dialog), content);
    gtk_window_set_default_widget(GTK_WINDOW(dialog), cancel);

    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(escape_closes), dialog);
    gtk_widget_add_controller(dialog, keys);
    gtk_window_present(GTK_WINDOW(dialog));

    if (test_running_ && tests_[next_test_ - 1].name == "show-uninstall") {
        shot_window_ = dialog;
        test_done(true, "");
    }
}

namespace {

struct UninstallJob {
    Places places;
    fs::path folder;
    UninstallOptions options;
    UninstallReport report;
};

std::string joined(const std::vector<std::string>& items, size_t most) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < most; ++i) out += (i ? ", " : "") + items[i];
    if (items.size() > most) out += " and " + std::to_string(items.size() - most) + " more";
    return out;
}

} // namespace

void Launcher::start_uninstall(UninstallOptions options) {
    if (task_) return refuse("Another task is running", "Wait for it to end, then uninstall.");
    // (The uninstall looks again right before it deletes anything.)
    if (game_running()) return refuse("The game is running", "Quit it, then uninstall.");
    begin_task(Kind::kUninstall, "Uninstalling Tekken Tag Tournament 2");
    log("Uninstalling from " + ui::shown(install_.folder));
    auto* job = new UninstallJob{places_, install_.folder, options, {}};
    GTask* task = g_task_new(nullptr, nullptr, +[](GObject*, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<Launcher*>(data);
        const auto* job = static_cast<UninstallJob*>(g_task_get_task_data(G_TASK(result)));
        const UninstallReport& r = job->report;
        std::vector<std::string> left, problems;
        for (const std::string& l : r.left) left.push_back(ui::shown(l));
        for (const UninstallProblem& p : r.problems) problems.push_back(ui::shown(p.path) + ": " + p.why);
        for (const std::string& p : problems) self->log("Not deleted: " + p);
        for (const std::string& l : left) self->log("Left in the folder: " + l);
        if (!r.refused.empty()) {
            self->finish_task(false, "Not uninstalled", r.refused);
            return;
        }
        std::string detail;
        if (!r.folder_removed && !left.empty()) {
            detail = "Left in " + ui::shown(job->folder) + ", as they are not the game's: " + joined(left, 6) + ".";
        }
        if (!problems.empty()) {
            detail += (detail.empty() ? "" : " ") + std::string("Not deleted: ") + joined(problems, 3) + ".";
        }
        if (detail.empty()) {
            const UninstallOptions& o = job->options;
            detail = o.saves && o.characters ? "Your saves, the added characters and the saved Tekken 7 data are "
                                               "deleted too."
                     : o.saves               ? "Your saves are deleted too; the added characters are kept."
                     : o.characters          ? "The added characters and the saved Tekken 7 data are deleted too; "
                                               "your saves are kept."
                                             : "Your saves and the added characters are kept.";
        }
        self->finish_task(r.problems.empty(), r.problems.empty() ? "Uninstalled" : "Uninstalled, but not everything",
                          detail);
    }, this);
    g_task_set_task_data(task, job, [](gpointer job) { delete static_cast<UninstallJob*>(job); });
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
        auto* job = static_cast<UninstallJob*>(data);
        job->report = uninstall(job->places, job->folder, job->options);
        g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
}

} // namespace launcher
