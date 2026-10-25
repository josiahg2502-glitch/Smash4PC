// Uninstalling (see uninstall.h). Folders are deleted through descriptors
// (openat, unlinkat with O_NOFOLLOW), so a symbolic link met anywhere below
// is deleted as a link, even one made while the deletion runs.

#include "uninstall.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace launcher {
namespace {

constexpr int kMaxDepth = 64;

class Deleter {
public:
    explicit Deleter(std::vector<UninstallProblem>& problems) : problems_(problems) {}

    // `name` in the open folder `dir` (shown to the user as `shown`): a file
    // or link as it is, a folder with all in it. False when something stays.
    bool remove_at(int dir, const std::string& name, const fs::path& shown, dev_t device, int depth = 0) {
        struct stat s {};
        if (::fstatat(dir, name.c_str(), &s, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT || problem(shown);
        if (!S_ISDIR(s.st_mode)) return ::unlinkat(dir, name.c_str(), 0) == 0 || errno == ENOENT || problem(shown);
        if (s.st_dev != device) return problem(shown, "another file system is mounted there");
        if (depth > kMaxDepth) return problem(shown, "folders nested too deep");
        const int fd = ::openat(dir, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return problem(shown);
        DIR* folder = ::fdopendir(fd);
        if (!folder) {
            ::close(fd);
            return problem(shown);
        }
        // (The names first: deleting while reading a folder may skip some.)
        std::vector<std::string> names;
        while (const dirent* e = ::readdir(folder)) {
            if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names.emplace_back(e->d_name);
        }
        bool all = true;
        for (const std::string& n : names) all = remove_at(fd, n, shown / n, device, depth + 1) && all;
        ::closedir(folder);
        if (!all) return false;
        return ::unlinkat(dir, name.c_str(), AT_REMOVEDIR) == 0 || errno == ENOENT || problem(shown);
    }

    // A file or folder by its path (the folders above it are the user's
    // own: those are followed).
    bool remove(const fs::path& path) {
        const fs::path parent = path.parent_path();
        const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) return errno == ENOENT || problem(path);
        struct stat s {};
        bool ok = true;
        if (::fstatat(dir, path.filename().c_str(), &s, AT_SYMLINK_NOFOLLOW) == 0) {
            ok = remove_at(dir, path.filename().string(), path, s.st_dev);
        }
        ::close(dir);
        return ok;
    }

    bool problem(const fs::path& what, const std::string& why = {}) {
        problems_.push_back({what, why.empty() ? std::strerror(errno) : why});
        return false;
    }

private:
    std::vector<UninstallProblem>& problems_;
};

bool same_folder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return normal(a) == normal(b) || fs::equivalent(a, b, ec);
}

// A folder that must never be deleted whatever names it: the root, the home
// folder, or one holding the home folder; and one not named by its whole
// path.
bool precious(const fs::path& folder, const Places& places) {
    if (folder.empty() || !folder.is_absolute()) return true;
    const fs::path f = normal(folder);
    return !f.has_relative_path() || inside(places.home, f);
}

void remove_if_empty(const fs::path& folder) {
    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(folder, ec)) && fs::is_empty(folder, ec)) fs::remove(folder, ec);
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

bool regular_at(int dir, const char* name) {
    struct stat s {};
    return ::fstatat(dir, name, &s, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(s.st_mode);
}

// What the game writes in its save folder (vfs.cpp maps /vol/save/<parts>
// below it): "common", and a folder per user named by their persistent id
// ("80000001").
bool game_save(const char* name) {
    if (std::strcmp(name, "common") == 0) return true;
    if (std::strlen(name) != 8) return false;
    for (const char* c = name; *c; ++c) {
        if (!std::isxdigit(static_cast<unsigned char>(*c))) return false;
    }
    return true;
}

// The importer's folders beside a pack it makes (pack.py _working): the new
// pack, ".<code>.part", and the earlier one it replaces, ".<code>.old"; a
// run killed at the wrong moment leaves them. (A pack's code is three
// lower-case letters, as the game takes it.)
bool pack_leftover(const std::string& name) {
    const std::string end = name.size() > 4 ? name.substr(4) : std::string();
    return name[0] == '.' && (end == ".part" || end == ".old") &&
           std::all_of(name.begin() + 1, name.begin() + 4, [](char c) { return c >= 'a' && c <= 'z'; });
}

// The saves in `root`. The folder may be one of the player's
// ($TTT2_SAVE_DIR) with more in it: only the game's own folder goes when it
// is left empty.
void remove_saves(Deleter& deleter, const fs::path& root, bool own_folder) {
    const int dir = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        if (errno != ENOENT) deleter.problem(root);
        return;
    }
    std::vector<std::string> names;
    if (DIR* listing = ::fdopendir(::dup(dir))) {
        while (const dirent* e = ::readdir(listing)) {
            struct stat s {};
            if (game_save(e->d_name) && ::fstatat(dir, e->d_name, &s, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(s.st_mode)) {
                names.emplace_back(e->d_name);
            }
        }
        ::closedir(listing);
    }
    struct stat s {};
    ::fstat(dir, &s);
    for (const std::string& name : names) deleter.remove_at(dir, name, root / name, s.st_dev);
    ::close(dir);
    if (own_folder) remove_if_empty(root);
}

} // namespace

UninstallReport uninstall(const Places& places, const fs::path& folder_path, const UninstallOptions& options) {
    UninstallReport report;
    const fs::path folder = normal(folder_path);
    // (The reasons leave the folder out: the window shows it.)
    if (precious(folder_path, places)) {
        report.refused = "This is not a folder the game can be uninstalled from.";
        return report;
    }
    const int dir = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        report.refused = std::string("The folder cannot be opened: ") + std::strerror(errno) + ".";
        return report;
    }
    if (!regular_at(dir, kMarker) && !regular_at(dir, kStarted)) {
        ::close(dir);
        report.refused = "The folder holds no install of the game.";
        return report;
    }
    // (Its files are read as it plays: deleted under it, it would fail at
    // its next load and write its saves into a folder made again.)
    if (program_running(folder)) {
        ::close(dir);
        report.refused = "The game is running: quit it, then uninstall.";
        return report;
    }
    struct stat s {};
    ::fstat(dir, &s);
    Deleter deleter(report.problems);

    // The game's folder, the program and what the builder adds; the marker
    // that it is an install last, so that a stop half-way still shows it.
    // (Of the files written as "<name>.part" first, only the program and the
    // builder's record are at the top: the game's are inside game/. Other
    // ".part" files there are another program's downloads.)
    deleter.remove_at(dir, "game", folder / "game", s.st_dev);
    const std::string program_part = std::string(kProgramName) + ".part";
    const std::string record_part = std::string(kBuildRecord) + ".part";
    for (const char* name :
         {program_part.c_str(), record_part.c_str(), kProgramName, "icon.png", kBuildRecord, kStarted, kMarker}) {
        struct stat e {};
        // (A folder of the user's with one of these names is theirs.)
        if (::fstatat(dir, name, &e, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(e.st_mode)) {
            deleter.remove_at(dir, name, folder / name, s.st_dev);
        }
    }
    if (DIR* listing = ::fdopendir(::dup(dir))) {
        ::rewinddir(listing);
        while (const dirent* e = ::readdir(listing)) {
            if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) {
                report.left.emplace_back(e->d_name);
            }
        }
        ::closedir(listing);
    }
    std::sort(report.left.begin(), report.left.end());
    if (report.left.empty()) {
        // The folder reached through a link (~/Games/TTT2 to a folder on
        // another disk) goes with the link; one that is a file system's top
        // (a drive chosen as the install folder) stays.
        struct stat l {}, r {};
        std::error_code ec;
        const bool link = ::lstat(folder.c_str(), &l) == 0 && S_ISLNK(l.st_mode);
        const fs::path real = link ? fs::canonical(folder, ec) : folder;
        if (ec || ::stat(real.c_str(), &r) != 0 || r.st_dev != s.st_dev || r.st_ino != s.st_ino) {
            deleter.problem(folder, "it is no longer the folder that was emptied");
        } else if (::rmdir(real.c_str()) == 0) {
            report.folder_removed = !link || ::unlink(folder.c_str()) == 0 || deleter.problem(folder);
        } else if (errno != EBUSY) {
            deleter.problem(real);
        }
    }
    ::close(dir);

    // Its menu entry and records, when they are this install's.
    if (const auto program = desktop_program(read_file(places.desktop_entry())); program && inside(*program, folder)) {
        deleter.remove(places.desktop_entry());
    }
    if (const std::string recorded = first_line(places.record()); !recorded.empty() && same_folder(recorded, folder)) {
        deleter.remove(places.record());
    }
    if (const auto started = read_started_record(places); started && same_folder(started->folder, folder)) {
        deleter.remove(places.started_record());
    }
    remove_if_empty(places.record().parent_path());

    // The compiled shaders, the builder's work folder and Steam's answer of
    // the DLC owned: made again when needed.
    deleter.remove(places.shader_cache() / "pipelines.bin");
    deleter.remove(places.shader_cache() / "spirv");
    deleter.remove(places.build_work());
    deleter.remove(places.owned_cache());

    if (options.saves) {
        if (precious(places.saves, places)) {
            deleter.problem(places.saves, "not deleted: not a folder of saves alone");
        } else {
            remove_saves(deleter, normal(places.saves), same_folder(places.saves, places.default_saves()));
        }
    }
    if (options.characters) {
        // The packs (folders with a character.ini) and what they share (with
        // the importer's shared.part and shared.old, which a run killed while
        // it made shared/ again leaves), and the packs a killed run was
        // making; the folder may be one of the user's ($TTT2_CHARACTERS) with
        // more in it.
        const fs::path packs = normal(places.packs);
        if (const int pd = ::open(packs.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); pd >= 0) {
            std::vector<std::string> names;
            if (DIR* listing = ::fdopendir(::dup(pd))) {
                while (const dirent* e = ::readdir(listing)) {
                    struct stat p {};
                    const std::string name = e->d_name;
                    if (name == "." || name == ".." || ::fstatat(pd, name.c_str(), &p, AT_SYMLINK_NOFOLLOW) != 0 ||
                        !S_ISDIR(p.st_mode)) {
                        continue;
                    }
                    if (name == "shared" || name == "shared.part" || name == "shared.old" || pack_leftover(name) ||
                        regular_at(pd, (name + "/character.ini").c_str())) {
                        names.push_back(name);
                    }
                }
                ::closedir(listing);
            }
            struct stat p {};
            ::fstat(pd, &p);
            for (const std::string& name : names) deleter.remove_at(pd, name, packs / name, p.st_dev);
            ::close(pd);
            remove_if_empty(packs);
        }
        deleter.remove(places.tekken7_data);
        // The importer's helper for Tekken 7, with its runs' logs: in the
        // cache folder, or in ~/.cache, where importers that did not follow
        // $XDG_CACHE_HOME kept it.
        deleter.remove(places.shader_cache() / "tekken7");
        deleter.remove(places.home / ".cache/ttt2/tekken7");
    }
    remove_if_empty(places.shader_cache());
    remove_if_empty(places.home / ".cache/ttt2");
    remove_if_empty(places.home / ".local/share/ttt2");
    remove_if_empty(places.data / "ttt2");
    return report;
}

} // namespace launcher
