// Where things are (see paths.h).

#include "paths.h"

#include <dirent.h>
#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace launcher {
namespace {

// A variable that is set and not empty.
const char* variable(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

std::string trim(std::string_view s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1));
}

bool is_there(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

bool executable(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && ::access(path.c_str(), X_OK) == 0;
}

// A new or empty folder, or one with an install (finished or not), as
// install.cpp's usable().
bool usable(const fs::path& folder) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(folder, ec))) return true;
    if (!fs::is_directory(folder, ec)) return false;
    return fs::exists(folder / kMarker, ec) || fs::exists(folder / kStarted, ec) || fs::is_empty(folder, ec);
}

bool starts_with(const fs::path& path, const fs::path& folder) {
    auto p = path.begin();
    for (auto f = folder.begin(); f != folder.end(); ++f, ++p) {
        if (p == path.end() || *p != *f) return false;
    }
    return true;
}

} // namespace

Places Places::from_environment() {
    Places p;
    p.home = variable("HOME") ? fs::path(variable("HOME")) : fs::path("/");
    const char* config = variable("XDG_CONFIG_HOME");
    p.config = config && *config == '/' ? fs::path(config) : p.home / ".config";
    const char* data = variable("XDG_DATA_HOME");
    p.data = data && *data == '/' ? fs::path(data) : p.home / ".local/share";
    p.cache = variable("XDG_CACHE_HOME") ? fs::path(variable("XDG_CACHE_HOME")) : p.home / ".cache";
    const char* saves = std::getenv("TTT2_SAVE_DIR");
    p.saves = saves ? fs::path(saves) : p.default_saves();
    if (variable("TTT2_CHARACTERS")) p.packs = variable("TTT2_CHARACTERS");
    else if (data) p.packs = fs::path(data) / "ttt2/characters";
    else p.packs = p.home / ".local/share/ttt2/characters";
    p.tekken7_data = p.home / ".local/share/ttt2/tekken7";
    return p;
}

std::string first_line(const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

bool write_started_record(const Places& places, const fs::path& folder, const fs::path& game) {
    const fs::path file = places.started_record();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::trunc);
        out << folder.string() << '\n' << game.string() << '\n';
        if (!out) return false;
    }
    fs::rename(part, file, ec);
    return !ec;
}

std::optional<Install> read_started_record(const Places& places) {
    std::ifstream in(places.started_record());
    std::string folder, game;
    if (!in || !std::getline(in, folder) || folder.empty()) return std::nullopt;
    std::getline(in, game);
    return Install{InstallState::kUnfinished, folder, game};
}

Install find_install(const Places& places) {
    std::error_code ec;
    if (const auto started = read_started_record(places)) {
        if (fs::exists(started->folder / kStarted, ec) && !fs::exists(started->folder / kMarker, ec)) return *started;
    }
    const std::string recorded = first_line(places.record());
    if (!recorded.empty()) {
        if (fs::exists(fs::path(recorded) / kMarker, ec)) return {InstallState::kInstalled, recorded, {}};
        if (fs::exists(fs::path(recorded) / kStarted, ec)) return {InstallState::kUnfinished, recorded, {}};
    }
    return {};
}

fs::path normal(const fs::path& path) {
    std::error_code ec;
    fs::path out = fs::absolute(path, ec).lexically_normal();
    if (!out.has_filename() && out.has_relative_path()) out = out.parent_path(); // a trailing slash
    return out;
}

bool inside(const fs::path& path, const fs::path& folder) {
    if (starts_with(normal(path), normal(folder))) return true;
    std::error_code ec, ec2;
    const fs::path real_path = fs::weakly_canonical(path, ec), real_folder = fs::weakly_canonical(folder, ec2);
    return !ec && !ec2 && starts_with(normal(real_path), normal(real_folder));
}

bool program_running(const fs::path& folder) {
    // The kernel names a process's program by its real path (through any
    // link in the folder's path), with " (deleted)" added once that file is
    // gone or another is renamed over it, as an update does.
    std::error_code ec;
    const fs::path program = normal(folder) / kProgramName;
    fs::path real = fs::weakly_canonical(folder, ec);
    real = ec ? program : normal(real) / kProgramName;
    DIR* proc = ::opendir("/proc");
    if (!proc) return false;
    bool running = false;
    while (const dirent* e = ::readdir(proc)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char target[PATH_MAX + 16];
        const std::string exe = std::string("/proc/") + e->d_name + "/exe";
        const ssize_t n = ::readlink(exe.c_str(), target, sizeof target);
        if (n <= 0 || static_cast<size_t>(n) == sizeof target) continue; // (not ours to read, or gone)
        std::string_view path(target, static_cast<size_t>(n));
        if (path.ends_with(" (deleted)")) path.remove_suffix(10);
        if (path == program.native() || path == real.native()) {
            running = true;
            break;
        }
    }
    ::closedir(proc);
    return running;
}

std::optional<fs::path> install_target(const fs::path& chosen, std::string* error) {
    const fs::path folder = normal(chosen);
    if (usable(folder)) return folder;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        if (error) *error = "is not a folder";
        return std::nullopt;
    }
    if (usable(folder / kProgramName)) return folder / kProgramName;
    if (error) *error = std::string("holds a \"") + kProgramName + "\" that is not an install of the game";
    return std::nullopt;
}

std::optional<fs::path> desktop_program(std::string_view entry) {
    std::istringstream in{std::string(entry)};
    std::string line, value;
    bool main_group = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with('[')) main_group = line == "[Desktop Entry]";
        else if (main_group && line.starts_with("Exec")) {
            const size_t equals = line.find('=');
            if (equals != std::string::npos && trim(line.substr(4, equals - 4)).empty()) {
                value = trim(line.substr(equals + 1));
                break;
            }
        }
    }
    if (value.empty()) return std::nullopt;
    // First the escapes of any string value, then the Exec key's own.
    std::string text;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char c = value[++i];
            text += c == 's' ? ' ' : c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
        } else {
            text += value[i];
        }
    }
    std::string program;
    size_t i = 0;
    if (text[0] == '"') {
        const std::string_view escaped = "\"`$\\";
        for (i = 1; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size() && escaped.find(text[i + 1]) != escaped.npos) ++i;
            program += text[i];
        }
        if (i >= text.size()) return std::nullopt; // not closed
    } else {
        program = text.substr(0, text.find_first_of(" \t"));
    }
    std::string out;
    for (size_t k = 0; k < program.size(); ++k) {
        out += program[k];
        if (program[k] == '%' && k + 1 < program.size() && program[k + 1] == '%') ++k;
    }
    if (out.empty()) return std::nullopt;
    return fs::path(out);
}

std::string kit_version(const fs::path& kit) { return trim(first_line(kit / "VERSION")); }

std::string built_version(const fs::path& folder) {
    std::ifstream in(folder / kBuildRecord);
    std::string line;
    while (in && std::getline(in, line)) {
        if (line.starts_with("kit ")) return trim(line.substr(4));
    }
    return {};
}

std::optional<fs::path> find_in_path(std::string_view name) {
    if (name.find('/') != std::string_view::npos) {
        return executable(fs::path(name)) ? std::optional(fs::path(name)) : std::nullopt;
    }
    const char* path = variable("PATH");
    std::string_view dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    while (!dirs.empty()) {
        const size_t colon = std::min(dirs.find(':'), dirs.size());
        const std::string_view dir = dirs.substr(0, colon);
        dirs.remove_prefix(std::min(colon + 1, dirs.size()));
        if (dir.empty()) continue;
        const fs::path candidate = fs::path(dir) / name;
        if (executable(candidate)) return candidate;
    }
    return std::nullopt;
}

fs::path find_kit(const fs::path& program_dir, const fs::path& build_tree_kit) {
    if (const char* kit = variable("TTT2_KIT")) return kit;
    for (const fs::path& kit : {program_dir / "../lib/ttt2/kit", build_tree_kit}) {
        if (!kit.empty() && is_there(kit / "bin/ttt2-build")) return normal(kit);
    }
    return {};
}

fs::path find_importer(const fs::path& program_dir, const fs::path& source_importer) {
    if (const char* importer = variable("TTT2_IMPORTER")) return importer;
    for (const fs::path& importer : {program_dir / "../share/ttt2/characters", source_importer}) {
        if (!importer.empty() && is_there(importer / "launcher.py")) return normal(importer);
    }
    return {};
}

fs::path find_python(const fs::path& program_dir) {
    if (const char* python = variable("TTT2_PYTHON")) return python;
    const fs::path bundled = program_dir / "../lib/ttt2/python/bin/python3";
    if (executable(bundled)) return normal(bundled);
    return find_in_path("python3").value_or(fs::path());
}

} // namespace launcher
