// ttt2 --install (see install.h). The title is copied file by file, each
// to "<name>.part" and renamed once complete, so an interrupted install
// resumes where it stopped. Files already in place are kept: one replaced
// for a mod stays, and deleting one then installing again restores it.

#include "install.h"

#include "cafe/generated.h"
#include "cafe/runtime.h"
#include "gpu/vulkan/png.h"
#include "wua.h"

#include <SDL3/SDL.h>

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace cafe::install {
namespace {

namespace fs = std::filesystem;

// What the user sees: the installed program, its folder, its menu entry.
constexpr const char* kName = "Tekken Tag Tournament 2";
constexpr const char* kMarker = ".ttt2-install";      // written last: the install is complete
constexpr const char* kStarted = ".ttt2-installing"; // written first: an install not finished yet

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const std::string& message) { throw Failure(message); }
[[noreturn]] void fail_errno(const std::string& what) { fail(what + ": " + std::strerror(errno)); }

fs::path home() {
    const char* h = std::getenv("HOME");
    return h && *h ? fs::path(h) : fs::path("/");
}

// $XDG_CONFIG_HOME and $XDG_DATA_HOME (absolute paths only, as the
// specification says).
fs::path xdg(const char* variable, const char* fallback) {
    const char* v = std::getenv(variable);
    return v && *v == '/' ? fs::path(v) : home() / fallback;
}

fs::path config_file() { return xdg("XDG_CONFIG_HOME", ".config") / "ttt2" / "installed"; }

// This executable, readable even when its file has been replaced (a
// rebuild during an install).
const fs::path kSelf = "/proc/self/exe";

std::string gigabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

// With TTT2_INSTALL_PROGRESS=1 (the launcher's ttt2-build runs it so), also
// lines for a program on stdout: "@folder <path>", "@progress <done>
// <total>" in bytes, "@error <text>". The text for people stays on stderr.
bool machine_output() {
    static const bool on = [] {
        const char* v = std::getenv("TTT2_INSTALL_PROGRESS");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

void machine_line(const std::string& text) {
    if (!machine_output()) return;
    std::string line = text;
    std::replace(line.begin(), line.end(), '\n', ' ');
    std::fprintf(stdout, "%s\n", line.c_str());
    std::fflush(stdout);
}

void machine_progress(uint64_t done, uint64_t total) {
    machine_line("@progress " + std::to_string(done) + " " + std::to_string(total));
}

// ---------------------------------------------------------------- source

struct Entry {
    std::string path; // in the title: "content/hdd/data005.bin"
    uint64_t size = 0;
    ZArchiveNodeHandle node = ZARCHIVE_INVALID_NODE;
};

// The title's files: in a .wua archive (see wua.h: the update's files over
// the base game's) or in a title folder (code/, content/, meta/, or an
// install folder with them in game/).
class Source {
public:
    explicit Source(const fs::path& path) {
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            directory_ = fs::exists(path / "code/Tekken.rpx", ec) ? path : path / "game";
            if (!fs::exists(directory_ / "code/Tekken.rpx", ec)) fail(path.string() + " holds no code/Tekken.rpx");
            // (Linked files and folders count as what they link to.)
            for (auto it = fs::recursive_directory_iterator(directory_, fs::directory_options::follow_directory_symlink, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it.depth() > 32) fail(directory_.string() + " has folders too deep (a link loop?)");
                if (it->is_directory(ec)) continue;
                const std::string relative = it->path().lexically_relative(directory_).string();
                if (!it->is_regular_file(ec)) fail(it->path().string() + " is not a file");
                if (relative.ends_with(".part")) continue;
                files_.push_back({relative, it->file_size(ec)});
                if (ec) fail("cannot read " + it->path().string() + ": " + ec.message());
            }
            if (ec) fail("cannot list " + directory_.string() + ": " + ec.message());
            std::sort(files_.begin(), files_.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
            return;
        }
        archive_.reset(ZArchiveReader::OpenFromFile(path));
        if (!archive_) fail("cannot open " + path.string() + " as a .wua archive");
        const std::vector<std::string> layers = wua::game_layers(*archive_);
        if (layers.empty()) fail(path.string() + " holds no title with code/Tekken.rpx");
        for (const wua::File& f : wua::game_files(*archive_, layers)) files_.push_back({f.path, f.size, f.node});
    }

    const std::vector<Entry>& files() const { return files_; }

    uint64_t total() const {
        uint64_t bytes = 0;
        for (const Entry& e : files_) bytes += e.size;
        return bytes;
    }

    void read(const Entry& e, uint64_t offset, uint64_t length, uint8_t* out) {
        if (archive_) {
            if (archive_->ReadFromFile(e.node, offset, length, out) != length) fail("cannot read " + e.path + " from the archive");
            return;
        }
        if (open_path_ != e.path) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = ::open((directory_ / e.path).c_str(), O_RDONLY | O_CLOEXEC);
            if (fd_ < 0) fail_errno("cannot read " + (directory_ / e.path).string());
            open_path_ = e.path;
        }
        for (uint64_t done = 0; done < length;) {
            const ssize_t n = ::pread(fd_, out + done, length - done, static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) fail("cannot read " + (directory_ / e.path).string());
            done += static_cast<uint64_t>(n);
        }
    }

    std::optional<std::vector<uint8_t>> read_all(const std::string& path) {
        const auto it = std::find_if(files_.begin(), files_.end(), [&](const Entry& e) { return e.path == path; });
        if (it == files_.end()) return std::nullopt;
        std::vector<uint8_t> data(it->size);
        read(*it, 0, data.size(), data.data());
        return data;
    }

    ~Source() {
        if (fd_ >= 0) ::close(fd_);
    }

private:
    std::unique_ptr<ZArchiveReader> archive_;
    fs::path directory_;
    std::vector<Entry> files_;
    int fd_ = -1;
    std::string open_path_;
};

// ---------------------------------------------------------------- choosing

struct Choice {
    std::atomic<bool> done{false};
    bool failed = false;
    std::string path;
};

void SDLCALL chosen(void* user, const char* const* list, int) {
    auto* choice = static_cast<Choice*>(user);
    if (list == nullptr) choice->failed = true;
    else if (*list != nullptr) choice->path = *list;
    choice->done = true;
}

// The desktop's file or folder dialog: the path chosen, "" when the user
// cancelled, none without a dialog (no display, no desktop portal).
std::optional<std::string> dialog(SDL_FileDialogType type, const char* title, const fs::path& start) {
    static const bool video = [] {
        SDL_SetAppMetadata("Tekken Tag Tournament 2", nullptr, "ttt2");
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // Ctrl+C still stops the install
        return SDL_Init(SDL_INIT_VIDEO);
    }();
    if (!video) return std::nullopt;
    static const SDL_DialogFileFilter kFilters[] = {{"Wii U game archive (.wua)", "wua"}, {"All files", "*"}};
    Choice choice;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, start.c_str());
    if (type == SDL_FILEDIALOG_OPENFILE) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(kFilters));
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    }
    SDL_ShowFileDialogWithProperties(type, chosen, &choice, props);
    while (!choice.done) {
        SDL_Event event;
        SDL_WaitEventTimeout(&event, 100);
    }
    SDL_DestroyProperties(props);
    if (choice.failed) {
        std::fprintf(stderr, "ttt2: no file dialog (%s)\n", SDL_GetError());
        return std::nullopt;
    }
    return choice.path;
}

// Asks on the terminal (Enter takes the suggestion); none without one.
std::optional<std::string> ask(const std::string& question, const std::string& suggestion) {
    if (!isatty(STDIN_FILENO)) return std::nullopt;
    if (suggestion.empty()) std::fprintf(stderr, "%s: ", question.c_str());
    else std::fprintf(stderr, "%s [%s]: ", question.c_str(), suggestion.c_str());
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    const auto first = line.find_first_not_of(" \t");
    line = first == std::string::npos ? "" : line.substr(first, line.find_last_not_of(" \t") - first + 1);
    // (Terminals quote a dropped file's path.)
    if (line.size() >= 2 && (line.front() == '\'' || line.front() == '"') && line.back() == line.front()) {
        line = line.substr(1, line.size() - 2);
    }
    if (line.empty()) return suggestion;
    if (line == "~") return home().string();
    // (string_view's starts_with is always inline; std::string's, called,
    // would make the program need GCC 16's libstdc++, which exports it.)
    if (std::string_view(line).starts_with("~/")) return (home() / line.substr(2)).string();
    return line;
}

fs::path choose_game() {
    std::fprintf(stderr, "ttt2: choose the game: TEKKEN TAG 2 Wii U EDITION (EU) as a .wua archive\n");
    std::optional<std::string> path =
        dialog(SDL_FILEDIALOG_OPENFILE, "Choose the Tekken Tag Tournament 2 Wii U Edition .wua", home());
    if (!path) path = ask("The game's .wua archive (or title folder)", "");
    if (!path) fail("no game given: ttt2 --install GAME [FOLDER]");
    if (path->empty()) fail("no game chosen");
    return *path;
}

fs::path choose_folder() {
    std::error_code ec;
    const fs::path start = fs::is_directory(home() / "Games", ec) ? home() / "Games" : home();
    std::fprintf(stderr, "ttt2: choose where to install it\n");
    std::optional<std::string> path =
        dialog(SDL_FILEDIALOG_OPENFOLDER, "Choose where to install Tekken Tag Tournament 2", start);
    if (!path) path = ask("Install into", (start / kName).string());
    if (!path) fail("no install folder given: ttt2 --install GAME FOLDER");
    if (path->empty()) fail("no folder chosen");
    return *path;
}

fs::path full_path(const fs::path& path) {
    std::error_code ec;
    fs::path folder = fs::absolute(path, ec).lexically_normal();
    if (!folder.has_filename()) folder = folder.parent_path(); // a trailing slash
    return folder;
}

// A new or empty folder, or one with an install of the port (finished or
// not).
bool usable(const fs::path& folder) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(folder, ec))) return true;
    if (!fs::is_directory(folder, ec)) return false;
    return fs::exists(folder / kMarker, ec) || fs::exists(folder / kStarted, ec) || fs::is_empty(folder, ec);
}

// A chosen folder that can hold the install is the install folder; any
// other gets a folder of the port's own inside (never one of another
// program's).
fs::path install_folder(const fs::path& chosen) {
    const fs::path folder = full_path(chosen);
    if (usable(folder)) return folder;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) fail(folder.string() + " is not a folder");
    if (usable(folder / kName)) return folder / kName;
    fail((folder / kName).string() + " already exists and is not an install of the port: choose another folder");
}

uint64_t free_space(fs::path folder) {
    std::error_code ec;
    while (folder.has_parent_path() && folder != folder.parent_path() && !fs::exists(folder, ec)) {
        folder = folder.parent_path();
    }
    struct statvfs s {};
    if (statvfs(folder.c_str(), &s) != 0) return UINT64_MAX;
    return static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
}

// ---------------------------------------------------------------- copying

class Progress {
public:
    explicit Progress(uint64_t total) : total_(total), tty_(isatty(STDERR_FILENO)) {}

    void kept(uint64_t bytes) {
        done_ += bytes;
        tell_machine();
    }

    void copied(uint64_t bytes) {
        done_ += bytes;
        copied_ += bytes;
        tell_machine();
        const auto now = Clock::now();
        if (now - printed_ < std::chrono::milliseconds(tty_ ? 250 : 10000)) return;
        printed_ = now;
        const double seconds = std::chrono::duration<double>(now - start_).count();
        const double rate = seconds > 0 ? static_cast<double>(copied_) / seconds : 0;
        const double left = rate > 0 ? static_cast<double>(total_ - done_) / rate : 0;
        std::fprintf(stderr, "%sttt2: copying the game: %s of %s (%.0f%%), %.0f MB/s, %.0f:%02.0f left   %s",
                     tty_ ? "\r" : "", gigabytes(done_).c_str(), gigabytes(total_).c_str(),
                     100.0 * static_cast<double>(done_) / static_cast<double>(std::max<uint64_t>(total_, 1)), rate / 1e6,
                     std::floor(left / 60), std::fmod(left, 60), tty_ ? "" : "\n");
    }

    void finish() {
        machine_progress(done_, total_);
        const double seconds = std::chrono::duration<double>(Clock::now() - start_).count();
        if (copied_ == 0) std::fprintf(stderr, "ttt2: the game's files are all in place\n");
        else std::fprintf(stderr, "%sttt2: copied %s in %.0f s%s\n", tty_ ? "\r" : "", gigabytes(copied_).c_str(), seconds,
                          tty_ ? "                                        " : "");
    }

private:
    using Clock = std::chrono::steady_clock;

    // At most ten lines a second.
    void tell_machine() {
        if (!machine_output()) return;
        const auto now = Clock::now();
        if (now - told_ < std::chrono::milliseconds(100)) return;
        told_ = now;
        machine_progress(done_, total_);
    }

    uint64_t total_, done_ = 0, copied_ = 0;
    bool tty_;
    Clock::time_point start_ = Clock::now(), printed_ = start_, told_ = start_ - std::chrono::seconds(1);
};

void write_all(int fd, const uint8_t* data, uint64_t size, const fs::path& path) {
    for (uint64_t done = 0; done < size;) {
        const ssize_t n = ::write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail_errno("cannot write " + path.string());
        done += static_cast<uint64_t>(n);
    }
}

// Copies the title into `game`; counts the files kept that differ in size
// from the game's (changed by the user).
void copy_title(Source& source, const fs::path& game, uint32_t& changed) {
    std::vector<uint8_t> buffer(8u << 20);
    Progress progress(source.total());
    for (const Entry& e : source.files()) {
        const fs::path target = game / e.path;
        std::error_code ec;
        if (fs::exists(fs::symlink_status(target, ec))) {
            if (!fs::is_regular_file(target, ec)) fail(target.string() + " is in the way");
            if (fs::file_size(target, ec) != e.size) ++changed;
            progress.kept(e.size);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) fail("cannot create " + target.parent_path().string() + ": " + ec.message());
        fs::path part = target;
        part += ".part";
        const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) fail_errno("cannot write " + part.string());
        try {
            for (uint64_t offset = 0; offset < e.size;) {
                const uint64_t length = std::min<uint64_t>(buffer.size(), e.size - offset);
                source.read(e, offset, length, buffer.data());
                write_all(fd, buffer.data(), length, part);
                offset += length;
                progress.copied(length);
            }
        } catch (...) {
            ::close(fd);
            throw;
        }
        // On disk before it gets its name: a file with its name is complete.
        if (::fdatasync(fd) != 0) {
            ::close(fd);
            fail_errno("cannot write " + part.string());
        }
        if (::close(fd) != 0) fail_errno("cannot write " + part.string());
        if (::rename(part.c_str(), target.c_str()) != 0) fail_errno("cannot rename " + part.string());
    }
    progress.finish();
}

void install_executable(const fs::path& folder) {
    const fs::path target = folder / kName;
    std::error_code ec;
    if (fs::equivalent(kSelf, target, ec)) return; // the installed copy is running
    fs::path part = target;
    part += ".part";
    fs::copy_file(kSelf, part, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        fs::permissions(part,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec,
                        ec);
    }
    if (!ec) fs::rename(part, target, ec);
    if (ec) fail("cannot copy the executable to " + target.string() + ": " + ec.message());
    // (Installs before it had its name called it ttt2.)
    if (fs::is_regular_file(fs::symlink_status(folder / "ttt2", ec))) fs::remove(folder / "ttt2", ec);
}

void install_icon(const std::optional<std::vector<uint8_t>>& tga, const fs::path& folder) {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    if (!tga || !decode_tga(*tga, rgba, width, height) ||
        !gpu::vk::write_png((folder / "icon.png").string(), rgba.data(), width, height)) {
        std::fprintf(stderr, "ttt2: warning: no icon (meta/iconTex.tga)\n");
    }
}

// A desktop entry's string value, escaped.
std::string desktop_string(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// The Exec key's program: quoted (", `, $ and \ escaped in the quotes),
// then escaped as a string, % doubled.
std::string desktop_exec(const std::string& path) {
    std::string quoted = "\"";
    for (char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    std::string out;
    for (char c : desktop_string(quoted)) {
        if (c == '%') out += "%%";
        else out += c;
    }
    return out;
}

fs::path launcher_file() { return xdg("XDG_DATA_HOME", ".local/share") / "applications" / "ttt2.desktop"; }

void install_launcher(const fs::path& folder) {
    const fs::path file = launcher_file();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path part = file;
    part += ".part";
    {
        std::ofstream out(part);
        out << "[Desktop Entry]\n"
               "Type=Application\n"
               "Name="
            << kName
            << "\n"
               "Comment=Tekken Tag Tournament 2 Wii U Edition, native port\n"
               "Exec="
            << desktop_exec((folder / kName).string())
            << "\n"
               "Icon="
            << desktop_string((folder / "icon.png").string())
            << "\n"
               "Terminal=false\n"
               "Categories=Game;ActionGame;\n"
               "Keywords=Tekken;fighting;\n"
               "StartupWMClass=ttt2\n";
        if (!out) fail("cannot write " + part.string());
    }
    fs::rename(part, file, ec);
    if (ec) fail("cannot write " + file.string() + ": " + ec.message());
}

void record(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(config_file().parent_path(), ec);
    std::ofstream out(config_file());
    out << folder.string() << '\n';
    if (!out) fail("cannot write " + config_file().string());
}

std::optional<fs::path> recorded_folder() {
    std::ifstream in(config_file());
    std::string line;
    if (in && std::getline(in, line) && !line.empty()) return fs::path(line);
    return std::nullopt;
}

int install(const std::vector<std::string>& positional, bool launcher) {
    if (positional.size() > 2) fail("too many arguments: ttt2 --install [GAME [FOLDER]]");
    const fs::path game = positional.size() >= 1 ? fs::path(positional[0]) : choose_game();
    Source source(game);
    const auto rpx = source.read_all("code/Tekken.rpx");
    if (!rpx || sha256_hex(*rpx) != cafe_program_info.rpx_sha256) {
        fail(game.string() + " is not the game this port was made from: TEKKEN TAG 2 Wii U EDITION, EU, version 16");
    }
    const fs::path folder = install_folder(positional.size() >= 2 ? fs::path(positional[1]) : choose_folder());
    machine_line("@folder " + folder.string());

    // Space for what is not there yet, and the executable.
    std::error_code ec;
    uint64_t needed = fs::file_size(kSelf, ec);
    if (ec) fail("cannot read this executable: " + ec.message());
    for (const Entry& e : source.files()) {
        if (!fs::exists(fs::symlink_status(folder / "game" / e.path, ec))) needed += e.size;
    }
    const uint64_t available = free_space(folder);
    std::fprintf(stderr, "ttt2: installing into %s: %s to copy, %s free\n", folder.c_str(), gigabytes(needed).c_str(),
                 gigabytes(available).c_str());
    if (needed > available) fail("not enough free space in " + folder.string());

    fs::create_directories(folder / "game", ec);
    if (ec) fail("cannot create " + (folder / "game").string() + ": " + ec.message());
    std::ofstream(folder / kStarted) << "an install of Tekken Tag Tournament 2 in progress\n";
    if (!fs::exists(folder / kStarted, ec)) fail("cannot write in " + folder.string());
    fs::remove(folder / kMarker, ec); // complete again at the end
    uint32_t changed = 0;
    copy_title(source, folder / "game", changed);
    install_executable(folder);
    install_icon(source.read_all("meta/iconTex.tga"), folder);
    if (const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); fd >= 0) {
        ::syncfs(fd); // (the renames too)
        ::close(fd);
    }
    {
        std::ofstream marker(folder / kMarker);
        marker << "TEKKEN TAG 2 Wii U EDITION (EU) v16\nrpx-sha256 " << cafe_program_info.rpx_sha256 << '\n';
        if (!marker) fail("cannot write " + (folder / kMarker).string());
    }
    fs::remove(folder / kStarted, ec);
    record(folder);
    if (launcher) install_launcher(folder);

    if (changed > 0) {
        std::fprintf(stderr, "ttt2: kept %u %s from the game's (delete one and install again to restore it)\n", changed,
                     changed == 1 ? "file that differs" : "files that differ");
    }
    std::fprintf(stderr, "ttt2: installed. Start %s%s\"%s\"\n", kName,
                 launcher ? " from the application menu or with " : " with ", (folder / kName).c_str());
    std::fprintf(stderr, "ttt2: the .wua is no longer needed to play. After rebuilding the port, ttt2 --update copies the new "
                         "executable into the install.\n");
    return 0;
}

// The executable into the install (after a rebuild): FOLDER, or the last
// install. (Its menu entry starts the installed executable as it is.)
int update(const std::vector<std::string>& positional) {
    if (positional.size() > 1) fail("too many arguments: ttt2 --update [FOLDER]");
    std::optional<fs::path> folder = positional.empty() ? recorded_folder() : std::optional(full_path(positional[0]));
    std::error_code ec;
    if (!folder || !fs::exists(*folder / kMarker, ec)) fail("no complete install to update: run ttt2 --install first");
    machine_line("@folder " + folder->string());
    install_executable(*folder);
    const uint64_t size = fs::file_size(*folder / kName, ec);
    machine_progress(size, size);
    std::fprintf(stderr, "ttt2: updated the executable in %s\n", folder->c_str());
    return 0;
}

} // namespace

int run(const std::vector<std::string>& args) {
    try {
        bool launcher = true, updating = false;
        std::vector<std::string> positional;
        for (const std::string& arg : args) {
            if (arg == "--install") continue;
            if (arg == "--update") updating = true;
            else if (arg == "--no-launcher") launcher = false;
            else if (std::string_view(arg).starts_with("--")) fail("unknown option " + arg);
            else positional.push_back(arg);
        }
        return updating ? update(positional) : install(positional, launcher);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%sttt2: install: %s\n", isatty(STDERR_FILENO) ? "\n" : "", e.what());
        machine_line(std::string("@error ") + e.what());
        return 1;
    }
}

std::optional<fs::path> installed_game() {
    std::vector<fs::path> folders;
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) folders.push_back(self.parent_path());
    if (const auto recorded = recorded_folder()) folders.push_back(*recorded);
    for (const fs::path& folder : folders) {
        if (fs::exists(folder / kMarker, ec)) return folder / "game";
        if (fs::exists(folder / kStarted, ec)) {
            std::fprintf(stderr, "ttt2: the install in %s is incomplete: run ttt2 --install again to finish it\n",
                         folder.c_str());
        }
    }
    return std::nullopt;
}

bool decode_tga(const std::vector<uint8_t>& tga, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    if (tga.size() < 18) return false;
    const uint32_t w = tga[12] | uint32_t{tga[13]} << 8, h = tga[14] | uint32_t{tga[15]} << 8;
    const uint32_t type = tga[2], bits = tga[16], descriptor = tga[17];
    // Uncompressed or run-length true colour, 24 or 32 bits.
    if (tga[1] != 0 || (type != 2 && type != 10) || (bits != 24 && bits != 32) || w == 0 || h == 0) return false;
    const size_t bytes = bits / 8, count = size_t{w} * h;
    size_t at = 18 + tga[0];
    // (A run-length packet holds at most 128 pixels in 1 + bytes bytes.)
    if (at > tga.size() || count > (tga.size() - at) / bytes * (type == 10 ? 128 : 1)) return false;
    std::vector<uint8_t> pixels;
    pixels.reserve(count * 4);
    const auto put = [&](size_t from) { // BGR(A)
        pixels.insert(pixels.end(), {tga[from + 2], tga[from + 1], tga[from], bytes == 4 ? tga[from + 3] : uint8_t{255}});
    };
    while (pixels.size() < count * 4) {
        size_t run = 1;
        bool repeat = false;
        if (type == 10) {
            if (at >= tga.size()) return false;
            run = (tga[at] & 0x7F) + 1;
            repeat = tga[at++] & 0x80;
        }
        if (pixels.size() / 4 + run > count || at + (repeat ? 1 : run) * bytes > tga.size()) return false;
        for (size_t i = 0; i < run; ++i) {
            put(at);
            if (!repeat) at += bytes;
        }
        if (repeat) at += bytes;
    }
    // Rows bottom up unless the descriptor says top down; columns left to
    // right unless it says otherwise.
    rgba.resize(pixels.size());
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t row = (descriptor & 0x20) ? y : h - 1 - y;
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t column = (descriptor & 0x10) ? w - 1 - x : x;
            std::memcpy(&rgba[(size_t{y} * w + x) * 4], &pixels[(size_t{row} * w + column) * 4], 4);
        }
    }
    width = w;
    height = h;
    return true;
}

} // namespace cafe::install
