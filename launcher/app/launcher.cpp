// The launcher's window (see launcher.h): its layout, finding the builder's
// kit, the importer and Python, refreshing what is installed, and the test
// hooks (TTT2_LAUNCHER_TEST_ACTIONS, TTT2_LAUNCHER_TEST_SHOTS, --screenshot).

#include "launcher.h"

#include "widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace launcher {
namespace {

fs::path program_file() {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : self;
}

fs::path program_folder() { return program_file().parent_path(); }

bool there(const fs::path& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(path, ec);
}

// The last line of a program's output (its error, usually).
std::string last_line(std::string text) {
    while (!text.empty() && text.back() == '\n') text.pop_back();
    const size_t start = text.rfind('\n');
    return start == std::string::npos ? text : text.substr(start + 1);
}

GtkWidget* section_title(GtkWidget* parent, const char* text) {
    GtkWidget* l = ui::label(text, "section-title");
    gtk_box_append(GTK_BOX(parent), l);
    return l;
}

} // namespace

Launcher::Launcher(GtkApplication* app, Options options) : app_(app), options_(std::move(options)) {
    const fs::path here = program_folder();
    kit_ = find_kit(here, TTT2_LAUNCHER_BUILD_KIT);
    importer_ = find_importer(here, TTT2_LAUNCHER_SOURCE_IMPORTER);
    python_ = find_python(here);
    kit_version_ = kit_version(kit_);
    appimage_ = running_appimage(program_file(), std::getenv("APPIMAGE"), std::getenv("APPDIR"));
    chosen_game_ = options_.game;
    // Steam's last answer to which DLC is owned, until it is asked again.
    if (const auto saved = saved_ownership(places_.owned_cache())) {
        owned_ = saved->owned;
        owned_answer_ = true;
    }

    if (const char* actions = std::getenv("TTT2_LAUNCHER_TEST_ACTIONS"); actions && *actions) {
        std::string error;
        if (auto parsed = parse_test_actions(actions, &error)) {
            tests_ = std::move(*parsed);
            testing_ = true;
        } else {
            std::fprintf(stderr, "@test actions failed %s\n", error.c_str());
        }
    }
    if (const char* shots = std::getenv("TTT2_LAUNCHER_TEST_SHOTS"); shots && *shots) {
        shots_ = shots;
        std::error_code ec;
        fs::create_directories(shots_, ec);
    }
    std::fprintf(stderr, "ttt2-launcher %s: kit %s, importer %s, python %s, AppImage %s\n", TTT2_LAUNCHER_VERSION,
                 kit_.empty() ? "(none)" : kit_.c_str(), importer_.empty() ? "(none)" : importer_.c_str(),
                 python_.empty() ? "(none)" : python_.c_str(), appimage_ ? appimage_->c_str() : "(none)");

    ui::load_style();
    build_window();
    refresh();
    check_release();
}

Launcher::~Launcher() {
    if (pulse_source_) g_source_remove(pulse_source_);
    if (game_watch_) g_source_remove(game_watch_);
}

void Launcher::present() { gtk_window_present(GTK_WINDOW(window_)); }

void Launcher::open_game(const fs::path& game) {
    chosen_game_ = game;
    show_game();
}

void Launcher::build_window() {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), kName);
    int width = 880, height = 860;
    if (const char* size = std::getenv("TTT2_LAUNCHER_TEST_SIZE")) std::sscanf(size, "%dx%d", &width, &height);
    gtk_window_set_default_size(GTK_WINDOW(window_), width, height);
    // Its icon, installed beside it (or in the source tree, unless built
    // without its paths): none rather than a missing picture when the theme
    // does not have it.
    GtkIconTheme* icons = gtk_icon_theme_get_for_display(gtk_widget_get_display(window_));
    // (Its own icons are built into it: Settings, Back, the GamePad's inputs.)
    gtk_icon_theme_add_resource_path(icons, TTT2_LAUNCHER_RESOURCE_PATH "/icons");
    gtk_icon_theme_add_search_path(icons, (program_folder() / "../share/icons").c_str());
    if (*TTT2_LAUNCHER_SOURCE_ICON) gtk_icon_theme_add_search_path(icons, TTT2_LAUNCHER_SOURCE_ICON);
    if (gtk_icon_theme_has_icon(icons, TTT2_LAUNCHER_APP_ID)) {
        gtk_window_set_icon_name(GTK_WINDOW(window_), TTT2_LAUNCHER_APP_ID);
    }
    gtk_widget_set_size_request(window_, 520, 480);
    g_signal_connect(window_, "close-request", G_CALLBACK(on_close), this);
    g_signal_connect(window_, "notify::is-active", G_CALLBACK(on_active), this);
    // (Pictures for tests wait until the window has been drawn.)
    gtk_widget_add_tick_callback(window_, +[](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
        auto* self = static_cast<Launcher*>(data);
        return ++self->frames_ < 2 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }, this, nullptr);

    // The header bar: on the Settings page, Back and its title instead of
    // the window's, Check Again and (in the AppImage) Update.
    GtkWidget* header = gtk_header_bar_new();
    title_ = gtk_label_new(kName);
    gtk_widget_add_css_class(title_, "title");
    gtk_label_set_ellipsize(GTK_LABEL(title_), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(title_), TRUE);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title_);
    back_button_ = gtk_button_new_from_icon_name("ttt2-back-symbolic");
    ui::set_id(back_button_, "back");
    gtk_widget_set_tooltip_text(back_button_, "Back");
    gtk_accessible_update_property(GTK_ACCESSIBLE(back_button_), GTK_ACCESSIBLE_PROPERTY_LABEL, "Back", -1);
    gtk_widget_set_visible(back_button_, FALSE);
    clicked(back_button_, [](Launcher* self) { self->close_settings(); });
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), back_button_);
    refresh_button_ = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_button_, "Check again");
    clicked(refresh_button_, [](Launcher* self) { self->check_again(); });
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh_button_);
    update_button_ = gtk_button_new_from_icon_name("ttt2-update-symbolic");
    ui::set_id(update_button_, "update-launcher");
    gtk_widget_set_tooltip_text(update_button_, "Update the launcher (this is version " TTT2_LAUNCHER_VERSION ")");
    gtk_accessible_update_property(GTK_ACCESSIBLE(update_button_), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Update the launcher", -1);
    gtk_widget_set_visible(update_button_, appimage_.has_value());
    clicked(update_button_, [](Launcher* self) { self->update_launcher(); });
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), update_button_);
    gtk_window_set_titlebar(GTK_WINDOW(window_), header);

    GtkWidget* outer = ui::box(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* content = ui::box(GTK_ORIENTATION_VERTICAL, 12, "launcher-content");

    // A newer version of the launcher, when GitHub has one (show_update_notice).
    update_notice_ = ui::box(GTK_ORIENTATION_HORIZONTAL, 14, "update-notice");
    GtkWidget* notice_icon = gtk_image_new_from_icon_name("ttt2-update-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(notice_icon), 20);
    gtk_widget_set_valign(notice_icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(update_notice_), notice_icon);
    GtkWidget* notice_text = ui::box(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(notice_text, TRUE);
    gtk_widget_set_valign(notice_text, GTK_ALIGN_CENTER);
    update_notice_title_ = ui::label("", "notice-title");
    update_notice_detail_ = ui::label("", "small dim");
    gtk_box_append(GTK_BOX(notice_text), update_notice_title_);
    gtk_box_append(GTK_BOX(notice_text), update_notice_detail_);
    gtk_box_append(GTK_BOX(update_notice_), notice_text);
    GtkWidget* notice_buttons = ui::box(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(notice_buttons, GTK_ALIGN_CENTER);
    update_notice_page_ = ui::button("What's New");
    ui::set_id(update_notice_page_, "release-page");
    clicked(update_notice_page_, [](Launcher* self) { self->open_release_page(); });
    gtk_box_append(GTK_BOX(notice_buttons), update_notice_page_);
    update_notice_button_ = ui::button("Update", "suggested-action");
    ui::set_id(update_notice_button_, "notice-update");
    clicked(update_notice_button_, [](Launcher* self) { self->update_launcher(); });
    gtk_box_append(GTK_BOX(notice_buttons), update_notice_button_);
    gtk_box_append(GTK_BOX(update_notice_), notice_buttons);
    gtk_widget_set_visible(update_notice_, FALSE);
    gtk_box_append(GTK_BOX(content), update_notice_);

    // The banner: the game's name, and its icon once installed (from the
    // install: nothing of the game is in the launcher).
    GtkWidget* banner = ui::box(GTK_ORIENTATION_HORIZONTAL, 20, "banner");
    banner_icon_ = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(banner_icon_), 72);
    gtk_widget_set_valign(banner_icon_, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(banner_icon_, FALSE);
    gtk_box_append(GTK_BOX(banner), banner_icon_);
    GtkWidget* names = ui::box(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(names, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(names), ui::label("Tekken Tag Tournament 2", "game-title"));
    gtk_box_append(GTK_BOX(names), ui::label("ULTIMATE EDITION", "game-edition"));
    GtkWidget* note = ui::label("The Wii U game as a native Linux program, built on this PC from your own copy, "
                                "with Tekken 7's characters added from yours.",
                                "game-note small");
    gtk_widget_set_margin_top(note, 6);
    gtk_box_append(GTK_BOX(names), note);
    gtk_box_append(GTK_BOX(banner), names);
    gtk_box_append(GTK_BOX(content), banner);

    GtkWidget* game_title = section_title(content, "Game");
    gtk_widget_set_margin_top(game_title, 14);
    game_card_ = ui::box(GTK_ORIENTATION_VERTICAL, 14, "card");
    gtk_box_append(GTK_BOX(content), game_card_);
    GtkWidget* characters_title = section_title(content, "Characters");
    gtk_widget_set_margin_top(characters_title, 14);
    characters_card_ = ui::box(GTK_ORIENTATION_VERTICAL, 14, "card");
    gtk_box_append(GTK_BOX(content), characters_card_);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    content_ = ui::clamp_new(content, kColumn);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), content_);

    // The main page and Settings, one beside the other (the task panel stays
    // below both).
    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_add_named(GTK_STACK(stack_), scroller, "main");
    gtk_stack_add_named(GTK_STACK(stack_), build_settings(), "settings");
    gtk_widget_set_vexpand(stack_, TRUE);
    gtk_box_append(GTK_BOX(outer), stack_);
    build_task_panel(outer);
    gtk_window_set_child(GTK_WINDOW(window_), outer);
}

void Launcher::refresh() {
    install_ = find_install(places_);
    const fs::path icon = install_.folder / "icon.png";
    if (install_.state == InstallState::kInstalled && there(icon)) {
        gtk_image_set_from_file(GTK_IMAGE(banner_icon_), icon.c_str());
        gtk_widget_set_visible(banner_icon_, TRUE);
    } else {
        gtk_widget_set_visible(banner_icon_, FALSE);
    }
    show_game();
    show_update_notice();
    load_status();
}

void Launcher::check_again() {
    if (owned_state_ == Owned::kFailed) owned_state_ = Owned::kUnknown;
    refresh();
}

bool Launcher::busy() const {
    return task_ || status_loading_ || owned_state_ == Owned::kChecking || (preflight_ && preflight_->running) ||
           release_checking_;
}

void Launcher::show_all() {
    show_game();
    show_characters();
    show_update_notice();
}

void Launcher::clicked(GtkWidget* button, void (*action)(Launcher*)) {
    struct Click {
        Launcher* self;
        void (*action)(Launcher*);
    };
    g_signal_connect_data(
        button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            const auto* c = static_cast<Click*>(data);
            c->action(c->self);
        }),
        new Click{this, action}, +[](gpointer data, GClosure*) { delete static_cast<Click*>(data); }, GConnectFlags(0));
}

Process::Environment Launcher::importer_environment() const {
    // (The importer may be read-only, as in the AppImage: no bytecode files
    // next to it.)
    Process::Environment environment = {{"PYTHONUNBUFFERED", "1"}, {"PYTHONDONTWRITEBYTECODE", "1"}};
    if (there(kit_ / "t7helper.exe")) environment.emplace_back("TTT2_T7HELPER", (kit_ / "t7helper.exe").string());
    return environment;
}

std::vector<std::string> Launcher::importer_command(const std::string& script) const {
    return {python_.string(), (importer_ / script).string()};
}

// ---------------------------------------------------------------- the importer's status

void Launcher::load_status() {
    // (Asked while one runs, as the install changes: once more after it.)
    status_again_ = status_loading_;
    if (importer_.empty() || python_.empty() || status_loading_) {
        show_characters();
        return;
    }
    std::vector<std::string> argv = importer_command("launcher.py");
    argv.push_back("status");
    if (install_.state == InstallState::kInstalled) {
        argv.push_back("--game");
        argv.push_back(install_.folder.string());
    }
    auto out = std::make_shared<std::string>();
    auto err = std::make_shared<std::string>();
    std::string error;
    status_loading_ = true;
    const auto process = Process::start(
        argv, importer_environment(),
        {[out, err](const std::string& line, bool is_error) { (is_error ? *err : *out) += line + "\n"; },
         [this, out, err](int code) {
             status_loading_ = false;
             status_time_ = std::chrono::steady_clock::now();
             std::string why;
             if (auto parsed = parse_status(*out, &why)) {
                 status_ = std::move(parsed);
                 status_error_.clear();
             } else {
                 status_.reset();
                 status_error_ =
                     code != 0 && !err->empty() ? last_line(*err) : "its answer is not readable (" + why + ")";
                 if (!err->empty()) std::fprintf(stderr, "%s", err->c_str());
             }
             // Steam closed: asked again once it runs again (as another
             // account, maybe); its answer is still shown meanwhile.
             if (status_ && !status_->steam_running && owned_state_ != Owned::kChecking) {
                 owned_state_ = Owned::kUnknown;
             }
             if (status_again_) return load_status();
             show_characters();
             check_ownership();
             settled();
         }},
        &error);
    if (!process) {
        status_loading_ = false;
        status_.reset();
        status_error_ = "Python cannot start: " + error;
    }
    show_characters();
}

void Launcher::check_ownership() {
    if (!status_ || owned_state_ != Owned::kUnknown || task_ || !status_->steam_running || !status_->tekken7_usable()) {
        return;
    }
    bool dlc = false;
    for (const Fighter& f : status_->fighters) dlc = dlc || f.dlc != 0;
    if (!dlc) return;
    std::vector<std::string> argv = importer_command("launcher.py");
    argv.push_back("owned");
    auto out = std::make_shared<std::string>();
    std::string error;
    owned_state_ = Owned::kChecking;
    const auto process = Process::start(
        argv, importer_environment(),
        {[out](const std::string& line, bool is_error) {
             if (!is_error) *out += line + "\n";
             else std::fprintf(stderr, "%s\n", line.c_str());
         },
         [this, out](int) {
             Ownership answer = parse_owned(*out);
             if (answer.error.empty()) {
                 owned_ = std::move(answer.owned);
                 owned_answer_ = true;
                 owned_state_ = Owned::kKnown;
                 save_ownership(places_.owned_cache(), *out);
             } else {
                 owned_error_ = answer.error;
                 owned_state_ = Owned::kFailed;
             }
             show_characters();
             settled();
         }},
        &error);
    if (!process) {
        owned_error_ = error;
        owned_state_ = Owned::kFailed;
    }
    show_characters();
}

// ---------------------------------------------------------------- the window

gboolean Launcher::on_close(GtkWindow*, gpointer data) {
    auto* self = static_cast<Launcher*>(data);
    if (!self->task_) return FALSE;
    // (An uninstall cannot be stopped half-way, and a task stopping already
    // is cleaning up: the window closes once it is done.)
    if (self->task_->kind == Kind::kUninstall || self->task_->cancelled) {
        self->quit_after_task_ = true;
        return TRUE;
    }
    GtkAlertDialog* dialog = gtk_alert_dialog_new("Stop and quit?");
    const std::string detail = self->task_->title + " is not finished. Stop it and close the launcher?";
    gtk_alert_dialog_set_detail(dialog, detail.c_str());
    const char* buttons[] = {"Keep Running", "Stop and Quit", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(self->window_), nullptr, stop_and_quit, self);
    g_object_unref(dialog);
    return TRUE;
}

void Launcher::stop_and_quit(GObject* dialog, GAsyncResult* result, gpointer data) {
    auto* self = static_cast<Launcher*>(data);
    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, nullptr) == 1 && self->task_) {
        self->quit_after_task_ = true;
        self->cancel_task();
    }
}

void Launcher::on_active(GObject* window, GParamSpec*, gpointer data) {
    // Back in the launcher (after starting Steam, say): look again.
    auto* self = static_cast<Launcher*>(data);
    if (!gtk_window_is_active(GTK_WINDOW(window))) {
        // (Its keys' releases go elsewhere now; a key chosen is not.)
        self->held_.clear();
        if (!self->testing_) self->stop_picking();
        return;
    }
    // (GitHub is asked again after hours, not each time.)
    if (std::chrono::steady_clock::now() - self->release_time_ > std::chrono::hours(6)) self->check_release();
    if (self->busy() || self->testing_) return;
    if (std::chrono::steady_clock::now() - self->status_time_ > std::chrono::seconds(5)) self->refresh();
}

// ---------------------------------------------------------------- tests

void Launcher::settled() {
    if (busy() || settle_pending_ ||
        (!testing_ && !test_running_ && (options_.screenshot.empty() || screenshot_taken_))) {
        return;
    }
    // A moment for the window to lay out and draw what changed.
    settle_pending_ = true;
    g_timeout_add(400, +[](gpointer data) -> gboolean {
        auto* self = static_cast<Launcher*>(data);
        if (self->frames_ < 2) return G_SOURCE_CONTINUE; // (not on the screen yet)
        self->settle_pending_ = false;
        if (self->busy()) return G_SOURCE_REMOVE; // (another settled() comes)
        if (!self->options_.screenshot.empty() && !self->screenshot_taken_ && !self->testing_) {
            self->screenshot_taken_ = true;
            self->picture(self->window_, self->options_.screenshot,
                          [self] { g_application_quit(G_APPLICATION(self->app_)); });
        } else if (self->testing_ && !self->test_running_) {
            if (self->next_test_ == 0 && !self->test_running_) {
                self->test_running_ = true; // (until the first picture is saved)
                self->save_shot(self->window_, "start", [self] {
                    self->test_running_ = false;
                    self->run_next_test();
                });
            } else {
                self->run_next_test();
            }
        }
        return G_SOURCE_REMOVE;
    }, this);
}

namespace {

struct Picture {
    GtkWidget* window;
    GtkWidget* page; // also saved, as <file>-page.png
    fs::path file;
    std::function<void()> done;
    GdkFrameClock* clock = nullptr;
    gulong handler = 0;
    int tries = 0;
};

void painted(GdkFrameClock*, gpointer data);

void after_next_frame(Picture* p) {
    p->clock = gtk_widget_get_frame_clock(p->window);
    p->handler = g_signal_connect(p->clock, "after-paint", G_CALLBACK(painted), p);
    gtk_widget_queue_draw(p->window);
}

// The picture right after a frame is painted: then every widget's drawing
// is in place (between frames, one changed is not until it is drawn again).
void painted(GdkFrameClock* clock, gpointer data) {
    auto* p = static_cast<Picture*>(data);
    g_signal_handler_disconnect(clock, p->handler);
    // TTT2_LAUNCHER_TEST_SCALE renders it larger (2: as on a display at
    // twice the scale); TTT2_LAUNCHER_TEST_PAGE=1 also saves all of the
    // main window's content, scrolled into view or not.
    const char* scale = std::getenv("TTT2_LAUNCHER_TEST_SCALE");
    const int s = scale ? std::atoi(scale) : 0;
    if (!ui::save_png(p->window, p->file.string(), s)) {
        if (++p->tries < 5) return after_next_frame(p);
        std::fprintf(stderr, "ttt2-launcher: cannot save %s\n", p->file.c_str());
    } else if (const char* page = std::getenv("TTT2_LAUNCHER_TEST_PAGE"); page && *page == '1' && p->page) {
        // (The page draws no background of its own: a dark or a light one
        // behind it, as the theme's text is light or dark.)
        GdkRGBA text;
        gtk_widget_get_color(p->page, &text);
        const bool dark = text.red + text.green + text.blue > 1.5f;
        const GdkRGBA background = dark ? GdkRGBA{0.13f, 0.13f, 0.14f, 1} : GdkRGBA{0.96f, 0.96f, 0.95f, 1};
        fs::path page_file = p->file;
        page_file.replace_extension();
        ui::save_png(p->page, page_file.string() + "-page.png", s, &background);
    }
    const std::function<void()> done = std::move(p->done);
    delete p;
    if (done) done();
}

} // namespace

void Launcher::picture(GtkWidget* window, const fs::path& file, std::function<void()> done) {
    if (!gtk_widget_get_mapped(window)) {
        std::fprintf(stderr, "ttt2-launcher: cannot save %s (no window)\n", file.c_str());
        if (done) done();
        return;
    }
    GtkWidget* page = window != window_ ? nullptr : in_settings() ? settings_content_ : content_;
    after_next_frame(new Picture{window, page, file, std::move(done)});
}

void Launcher::save_shot(GtkWidget* window, const std::string& label, std::function<void()> done) {
    if (shots_.empty()) {
        if (done) done();
        return;
    }
    char name[16];
    std::snprintf(name, sizeof name, "%02d-", shot_number_);
    picture(window, shots_ / (name + label + ".png"), std::move(done));
}

void Launcher::run_next_test() {
    if (next_test_ >= tests_.size()) {
        testing_ = false;
        if (!options_.screenshot.empty() && !screenshot_taken_) settled();
        return;
    }
    const TestAction& a = tests_[next_test_++];
    shot_number_ = static_cast<int>(next_test_);
    test_running_ = true;
    test_reported_ = false;
    std::fprintf(stderr, "@test %s started\n", a.text.c_str());
    if (a.name == "install") {
        start_install(a.args[0], a.args[1]);
    } else if (a.name == "update") {
        if (install_.state != InstallState::kInstalled) test_done(false, "no complete install to update");
        else start_update();
    } else if (a.name == "update-launcher") {
        update_launcher();
    } else if (a.name == "add") {
        start_import(a.args);
    } else if (a.name == "add-all-base" || a.name == "add-all-owned") {
        const auto names = a.name == "add-all-base" ? base_to_add() : owned_to_add();
        if (names.empty()) test_done(false, "no characters to add");
        else start_import(names);
    } else if (a.name == "remove") {
        start_remove(a.args);
    } else if (a.name == "uninstall") {
        if (install_.state == InstallState::kNone) test_done(false, "nothing is installed");
        else start_uninstall({.saves = a.args[0] == "all", .characters = a.args[0] == "all"});
    } else if (a.name == "play") {
        play();
        if (!game_process_) {
            test_done(false, game_error_);
            return;
        }
        // It runs on: the test goes on once it has started (and not failed
        // at once).
        g_timeout_add_seconds(3, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Launcher*>(data);
            self->test_done(self->game_error_.empty(), self->game_error_);
            return G_SOURCE_REMOVE;
        }, this);
    } else if (a.name == "quit") {
        std::fprintf(stderr, "@test quit ok\n");
        g_application_quit(G_APPLICATION(app_));
    } else if (a.name == "expand-dlc") {
        dlc_expanded_ = true;
        show_characters();
        test_done(true, "");
    } else if (a.name == "expand-log") {
        gtk_expander_set_expanded(GTK_EXPANDER(log_expander_), TRUE);
        test_done(true, "");
    } else if (a.name == "show-uninstall") {
        ask_uninstall();
    } else if (a.name == "focus") {
        GtkWidget* widget = ui::find_id(window_, a.args[0]);
        const bool ok = widget && gtk_widget_grab_focus(widget);
        gtk_window_set_focus_visible(GTK_WINDOW(window_), TRUE); // (as after a key press: the picture shows it)
        test_done(ok, ok ? "" : "no " + a.args[0] + " that can take the focus");
    } else if (a.name == "refresh") {
        check_again();
        // (Once all it starts has ended: which widget has the focus then.)
        g_timeout_add(200, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Launcher*>(data);
            if (self->busy()) return G_SOURCE_CONTINUE;
            const std::string focus = ui::focused_id(self->window_);
            self->test_done(true, "focus " + (focus.empty() ? std::string("none") : focus));
            return G_SOURCE_REMOVE;
        }, this);
    } else if (a.name == "settings" || a.name == "back") {
        // As the buttons are clicked: Settings in the game's card, Back in the
        // header bar.
        GtkWidget* button = a.name == "settings" ? ui::find_id(game_card_, "settings") : back_button_;
        if (!button || !gtk_widget_get_visible(button)) {
            test_done(false,
                      a.name == "settings" ? "no Settings button (the game is not installed)" : "no Back button");
            return;
        }
        g_signal_emit_by_name(button, "clicked");
        // (Once the page has slid in.)
        g_timeout_add(gtk_stack_get_transition_duration(GTK_STACK(stack_)) + 100, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Launcher*>(data);
            const bool settings = self->tests_[self->next_test_ - 1].name == "settings";
            self->test_done(self->in_settings() == settings, self->in_settings() ? "Settings" : "the main page");
            return G_SOURCE_REMOVE;
        }, this);
    } else if (a.name == "bind") {
        // The row's key button clicked (unless it waits for a key already:
        // a click would stop that), then those keys pressed and let go,
        // through the window's key handler (no event: no layout).
        const int input = input_index(a.args[0]);
        if (!in_settings()) {
            test_done(false, "Settings is not open");
            return;
        }
        if (picking_ != input) g_signal_emit_by_name(key_rows_[input].button, "clicked");
        if (picking_ != input) {
            test_done(false, "the key button does not wait for a key");
            return;
        }
        std::vector<guint> codes;
        for (size_t from = 0; from <= a.args[1].size();) {
            const size_t plus = std::min(a.args[1].find('+', from), a.args[1].size());
            codes.push_back(static_cast<guint>(std::stoul(a.args[1].substr(from, plus - from))));
            from = plus + 1;
        }
        taken_.reset();
        for (const guint code : codes) key_down(code, 0, GdkModifierType(0), std::nullopt);
        for (auto code = codes.rbegin(); code != codes.rend(); ++code) key_up(*code);
        if (taken_) test_done(taken_->ok, taken_->text);
        else test_done(false, picking_ < 0 ? "choosing a key stopped: a shortcut" : "no key was taken yet");
    } else if (a.name == "reset-keys") {
        if (!in_settings()) {
            test_done(false, "Settings is not open");
            return;
        }
        restore_keys();
        test_done(keymap_.is_default(), keymap_.is_default() ? "the default keys" : "not restored");
    } else if (a.name == "fullscreen" || a.name == "resolution" || a.name == "interpolation") {
        // The control set as a click sets it (its handler saves display.ini),
        // then the file read back as the game reads it.
        if (!in_settings()) {
            test_done(false, "Settings is not open");
            return;
        }
        display::Settings wanted = display_;
        if (a.name == "fullscreen") {
            wanted.fullscreen = a.args[0] == "yes";
            gtk_switch_set_active(GTK_SWITCH(fullscreen_switch_), wanted.fullscreen);
        } else if (a.name == "interpolation") {
            wanted.interpolation = a.args[0] == "yes";
            gtk_switch_set_active(GTK_SWITCH(interpolation_switch_), wanted.interpolation);
        } else {
            wanted.resolution = display::resolution_named(a.args[0]).value_or(display::Resolution::kAuto);
            const auto& all = display::kResolutions;
            const auto at = std::find(std::begin(all), std::end(all), wanted.resolution) - std::begin(all);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(resolution_list_), static_cast<guint>(at));
        }
        const display::Loaded loaded = display::load(places_.display());
        const display::Settings& s = loaded.settings;
        std::string text = std::string("display.ini: fullscreen ") + (s.fullscreen ? "yes" : "no") + ", resolution " +
                           std::string(display::name_of(s.resolution)) + ", interpolation " +
                           (s.interpolation ? "yes" : "no");
        if (!loaded.error.empty()) text = loaded.error;
        if (gtk_widget_get_visible(display_error_)) text = gtk_label_get_text(GTK_LABEL(display_error_));
        test_done(loaded.error.empty() && s == wanted && display_ == wanted, text);
    } else if (a.name == "wait") {
        g_timeout_add(static_cast<guint>(std::stod(a.args[0]) * 1000), +[](gpointer data) -> gboolean {
            static_cast<Launcher*>(data)->test_done(true, "");
            return G_SOURCE_REMOVE;
        }, this);
    }
}

void Launcher::test_done(bool ok, const std::string& text) {
    if (!test_running_ || test_reported_) return;
    test_reported_ = true;
    const TestAction& a = tests_[next_test_ - 1];
    std::string line = text;
    for (char& c : line) {
        if (c == '\n') c = ' ';
    }
    std::fprintf(stderr, "@test %s %s%s%s\n", a.text.c_str(), ok ? "ok" : "failed", line.empty() ? "" : " ",
                 line.c_str());
    // The picture once the window shows the result (a refresh may follow).
    g_timeout_add(400, +[](gpointer data) -> gboolean {
        auto* self = static_cast<Launcher*>(data);
        if (self->busy()) return G_SOURCE_CONTINUE;
        // (show-uninstall's picture is of its dialog, closed after it.)
        GtkWidget* window = self->shot_window_ ? self->shot_window_ : self->window_;
        self->save_shot(window, self->tests_[self->next_test_ - 1].label(), [self] {
            if (self->shot_window_) gtk_window_destroy(GTK_WINDOW(self->shot_window_));
            self->shot_window_ = nullptr;
            self->test_running_ = false;
            self->settled();
        });
        return G_SOURCE_REMOVE;
    }, this);
}

} // namespace launcher
