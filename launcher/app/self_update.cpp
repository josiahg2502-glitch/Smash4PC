// The launcher's own update (see launcher.h): GitHub's newest release, its
// AppImage downloaded with the system's curl or wget beside the AppImage the
// launcher runs from, checked, renamed over it, and opened as this launcher
// closes. (The running AppImage keeps reading the old file, which stays until
// it ends.) And the notice that a newer version is out, from asking GitHub in
// the background.

#include "launcher.h"

#include "json.h"
#include "system.h"
#include "widgets.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace launcher {
namespace {

constexpr const char* kUserAgent = "ttt2-launcher/" TTT2_LAUNCHER_VERSION;
// How long the new launcher may take to say its version (its AppImage is
// mounted first; it opens no window for that).
constexpr guint kVersionSeconds = 60;

// GitHub's newest release, or the answer tests give instead.
bool custom_releases() {
    const char* url = std::getenv("TTT2_LAUNCHER_RELEASES");
    return url && *url;
}
std::string releases_url() { return custom_releases() ? std::getenv("TTT2_LAUNCHER_RELEASES") : kLatestRelease; }

// The last line curl or wget wrote about what went wrong, without their
// names ("curl: (6) Could not resolve host: api.github.com").
std::string last_error(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    const size_t start = text.rfind('\n');
    std::string line = start == std::string::npos ? text : text.substr(start + 1);
    for (const char* name : {"curl: ", "wget: ", "wget2: "}) {
        if (line.starts_with(name)) line.erase(0, std::string(name).size());
    }
    if (line.starts_with("(")) {
        if (const size_t close = line.find(") "); close != std::string::npos) line.erase(0, close + 2);
    }
    return line;
}

std::string sentence(std::string text) {
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') text[0] = static_cast<char>(text[0] - 'a' + 'A');
    if (!text.empty() && text.back() != '.') text += '.';
    return text;
}

// A file's SHA-256 (lowercase hex); empty when it cannot be read.
std::string sha256_of(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    GChecksum* sum = g_checksum_new(G_CHECKSUM_SHA256);
    char buffer[1 << 16];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        g_checksum_update(sum, reinterpret_cast<const guchar*>(buffer), in.gcount());
    }
    std::string out = in.bad() ? "" : g_checksum_get_string(sum);
    g_checksum_free(sum);
    return out;
}

Line step_line(const std::string& text) {
    Line line;
    line.kind = Line::Kind::kStep;
    line.word = "update";
    line.text = text;
    return line;
}

} // namespace

void Launcher::update_launcher() {
    if (!appimage_) {
        return refuse("Cannot update the launcher",
                      "Only the launcher's AppImage updates itself, and this launcher does not run from one.");
    }
    if (task_) return refuse("Another task is running", "Wait for it to end, then update the launcher.");
    if (const std::string why = cannot_replace(*appimage_); !why.empty()) {
        return refuse("Cannot update the launcher",
                      "Its folder, " + ui::shown(appimage_->parent_path()) + ", cannot be written to (" + why +
                          "): move the AppImage to a folder of yours, then update it.");
    }
    const auto downloader = find_downloader();
    if (!downloader) {
        const std::string command = install_command(this_distro(), {Package::kCurl});
        return refuse("Cannot update the launcher",
                      "Downloading needs curl or wget: install curl" +
                          (command.empty() ? std::string(" with your distribution's packages.") : " with " + command));
    }
    begin_task(Kind::kLauncherUpdate, "Updating the launcher");
    update_ = LauncherUpdate{};
    update_->downloader = *downloader;
    task_->progress.feed(step_line("Looking for a newer version"));
    show_task();
    update_step(download_command(*downloader, releases_url(), true, {}, kUserAgent), false,
                [this](int code, const std::string& out, const std::string& err) { release_answer(code, out, err); });
}

void Launcher::update_step(const std::vector<std::string>& argv, bool log_out,
                           std::function<void(int, const std::string&, const std::string&)> then) {
    log("$ " + command_text(argv));
    auto out = std::make_shared<std::string>();
    auto err = std::make_shared<std::string>();
    std::string error;
    task_->process = Process::start(argv, {},
                                    {[this, out, err, log_out](const std::string& line, bool is_error) {
                                         (is_error ? *err : *out) += line + "\n";
                                         if (is_error || log_out) log(line);
                                     },
                                     [this, out, err, then](int code) {
                                         if (!task_ || task_->kind != Kind::kLauncherUpdate) return;
                                         if (task_->cancelled) {
                                             return update_failed("Update cancelled", "The launcher is as it was.",
                                                                  true);
                                         }
                                         then(code, *out, *err);
                                     }},
                                    &error);
    if (!task_->process) update_failed("Update failed", "Cannot start " + argv[0] + ": " + error);
}

void Launcher::release_answer(int code, const std::string& out, const std::string& err) {
    if (code != 0 && !json::parse(out)) {
        return update_failed("Update failed",
                             "GitHub cannot be reached: " +
                                 (err.empty() ? "the download ended with code " + std::to_string(code) : last_error(err)) +
                                 ".");
    }
    const ReleaseAnswer answer = parse_release(out);
    release_known(answer);
    if (answer.none || (answer.release && *compare_versions(answer.release->version, TTT2_LAUNCHER_VERSION) <= 0)) {
        log(answer.none ? "No release is published yet." : "The newest release is " + answer.release->tag + ".");
        update_.reset();
        finish_task(true, "The launcher is up to date", "Version " TTT2_LAUNCHER_VERSION " is the newest.");
        return;
    }
    if (!answer.release) return update_failed("Update failed", sentence(answer.error));
    if (!custom_releases() && !answer.release->url.starts_with("https://")) {
        return update_failed("Update failed", "Version " + answer.release->version + " is not offered over HTTPS.");
    }
    update_->release = *answer.release;
    download_release();
}

void Launcher::download_release() {
    LauncherUpdate& u = *update_;
    u.file = update_file(*appimage_);
    std::error_code ec;
    fs::remove(u.file, ec); // (one stopped half-way)
    task_->title = "Updating the launcher to " + u.release.version;
    ui::set_text(task_title_, task_->title);
    task_->progress.feed(step_line("Downloading version " + u.release.version));
    show_task();
    if (u.release.size > 0) {
        u.watch = g_timeout_add(250, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Launcher*>(data);
            if (!self->update_ || !self->task_) return G_SOURCE_REMOVE;
            std::error_code ec;
            const uintmax_t size = fs::file_size(self->update_->file, ec);
            Line line;
            line.kind = Line::Kind::kProgress;
            line.done = ec ? 0 : size;
            line.total = self->update_->release.size;
            if (self->task_->progress.feed(line)) self->show_task();
            return G_SOURCE_CONTINUE;
        }, this);
    }
    update_step(download_command(u.downloader, u.release.url, false, u.file, kUserAgent), true,
                [this](int code, const std::string&, const std::string& err) { release_downloaded(code, err); });
}

void Launcher::release_downloaded(int code, const std::string& err) {
    LauncherUpdate& u = *update_;
    if (u.watch) g_source_remove(u.watch);
    u.watch = 0;
    const std::string version = u.release.version;
    if (code != 0) {
        return update_failed("Update failed", "Version " + version + " cannot be downloaded: " +
                                                  (err.empty() ? "code " + std::to_string(code) : last_error(err)) +
                                                  ".");
    }
    std::error_code ec;
    const uintmax_t size = fs::file_size(u.file, ec);
    if (ec || (u.release.size > 0 && size != u.release.size)) {
        return update_failed("Update failed", "The download of version " + version + " is incomplete (" +
                                                  std::to_string(ec ? 0 : size) + " of " +
                                                  std::to_string(u.release.size) + " bytes).");
    }
    if (!u.release.sha256.empty() && sha256_of(u.file) != u.release.sha256) {
        return update_failed("Update failed",
                             "The download of version " + version + " is damaged: its SHA-256 is not GitHub's.");
    }
    if (!is_appimage(u.file)) {
        return update_failed("Update failed", "What was downloaded for version " + version + " is not an AppImage.");
    }
    if (const std::string why = make_executable(u.file, *appimage_); !why.empty()) {
        return update_failed("Update failed", "The download cannot be made executable (" + why + ").");
    }
    // It runs on this PC, and is the version it was released as (else each
    // update would download it again).
    task_->progress.feed(step_line("Checking version " + version));
    show_task();
    u.timeout = g_timeout_add_seconds(kVersionSeconds, +[](gpointer data) -> gboolean {
        auto* self = static_cast<Launcher*>(data);
        self->update_->timeout = 0;
        self->update_->timed_out = true;
        self->task_->process->terminate();
        return G_SOURCE_REMOVE;
    }, this);
    update_step({u.file.string(), "--version"}, true,
                [this](int code, const std::string& out, const std::string&) { release_started(code, out); });
}

void Launcher::release_started(int code, const std::string& out) {
    LauncherUpdate& u = *update_;
    if (u.timeout) g_source_remove(u.timeout);
    u.timeout = 0;
    const std::string version = u.release.version, said = version_in(out);
    if (u.timed_out) {
        return update_failed("Update failed", "Version " + version + " does not start: it did not answer within " +
                                                  std::to_string(kVersionSeconds) + " seconds.");
    }
    if (code != 0 || said.empty()) {
        return update_failed("Update failed", "Version " + version + " does not start on this PC (code " +
                                                  std::to_string(code) + ", see Details).");
    }
    if (*compare_versions(said, version) != 0) {
        return update_failed("Update failed", "The AppImage released as version " + version + " is version " + said +
                                                  ".");
    }
    if (const std::string why = replace_appimage(u.file, *appimage_); !why.empty()) {
        return update_failed("Update failed", "It cannot replace " + ui::shown(*appimage_) + " (" + why + ").");
    }
    update_.reset();
    log("Version " + version + " is in " + ui::shown(*appimage_) + ".");
    // The new launcher waits for this one to end before it opens its window.
    std::string error;
    const auto opened = Process::start({appimage_->string()}, {{kReplacesVariable, std::to_string(::getpid())}}, {},
                                       &error, false);
    if (!opened) {
        finish_task(false, "Updated to " + version + ", but it cannot be opened",
                    sentence(error) + " Start the launcher again.");
        return;
    }
    log("Opening version " + version + "…");
    test_done(true, "updated to " + version + ", opening it");
    quit_after_task_ = true;
    finish_task(true, "Updated to " + version, "Opening it…");
}

void Launcher::check_release() {
    if (!appimage_ || release_checking_) return;
    const auto downloader = find_downloader();
    if (!downloader) return; // (Update says what is missing)
    release_checking_ = true;
    release_time_ = std::chrono::steady_clock::now();
    auto out = std::make_shared<std::string>();
    auto err = std::make_shared<std::string>();
    std::string error;
    const auto process = Process::start(
        download_command(*downloader, releases_url(), true, {}, kUserAgent), {},
        {[out, err](const std::string& line, bool is_error) { (is_error ? *err : *out) += line + "\n"; },
         [this, out, err](int code) {
             release_checking_ = false;
             // (Quietly: no notice when GitHub cannot be asked now.)
             if (code != 0 && !json::parse(*out)) {
                 std::fprintf(stderr, "ttt2-launcher: GitHub cannot be asked for a newer version: %s\n",
                              last_error(*err).c_str());
             } else {
                 const ReleaseAnswer answer = parse_release(*out);
                 if (!answer.error.empty()) std::fprintf(stderr, "ttt2-launcher: %s\n", answer.error.c_str());
                 release_known(answer);
             }
             settled();
         }},
        &error);
    if (!process) release_checking_ = false;
}

void Launcher::release_known(const ReleaseAnswer& answer) {
    // (A newer release without its AppImage yet is not offered.)
    if (answer.none) newer_.reset();
    else if (answer.release) {
        if (*compare_versions(answer.release->version, TTT2_LAUNCHER_VERSION) > 0) newer_ = answer.release;
        else newer_.reset();
    }
    show_update_notice();
}

void Launcher::show_update_notice() {
    const bool shown = appimage_ && newer_;
    gtk_widget_set_visible(update_notice_, shown);
    if (shown) {
        gtk_widget_add_css_class(update_button_, "update-available");
        ui::set_text(update_notice_title_, "Version " + newer_->version + " of the launcher is out");
        ui::set_text(update_notice_detail_,
                     "You have " TTT2_LAUNCHER_VERSION ". Update replaces this AppImage with it and opens it.");
        gtk_widget_set_visible(update_notice_page_,
                               newer_->page.starts_with("https://") || (custom_releases() && !newer_->page.empty()));
        gtk_widget_set_sensitive(update_notice_button_, !task_);
        ui::set_tooltip(update_button_, "Update the launcher to version " + newer_->version +
                                            " (this is version " TTT2_LAUNCHER_VERSION ")");
    } else {
        gtk_widget_remove_css_class(update_button_, "update-available");
        gtk_widget_set_tooltip_text(update_button_, "Update the launcher (this is version " TTT2_LAUNCHER_VERSION ")");
    }
}

void Launcher::open_release_page() {
    if (!newer_ || newer_->page.empty()) return;
    GtkUriLauncher* page = gtk_uri_launcher_new(newer_->page.c_str());
    gtk_uri_launcher_launch(page, GTK_WINDOW(window_), nullptr, nullptr, nullptr);
    g_object_unref(page);
}

void Launcher::update_failed(const std::string& title, const std::string& why, bool cancelled) {
    if (update_) {
        if (update_->watch) g_source_remove(update_->watch);
        if (update_->timeout) g_source_remove(update_->timeout);
        std::error_code ec;
        if (!update_->file.empty()) fs::remove(update_->file, ec);
        update_.reset();
    }
    finish_task(false, title, why, cancelled);
}

} // namespace launcher
