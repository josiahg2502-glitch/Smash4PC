#pragma once

// Where things are: the game's install and the user's folders by the same
// rules as the game (runtime/src/install.cpp, main.cpp, title/roster.cpp,
// gpu/vulkan/draw.cpp) and the importer (tools/characters), and the
// launcher's own pieces (the builder's kit, the importer, Python).

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace launcher {

namespace fs = std::filesystem;

// What the user sees: the installed program, the folder made for it.
inline constexpr const char* kProgramName = "Tekken Tag Tournament 2";
inline constexpr const char* kMarker = ".ttt2-install";      // the install is complete
inline constexpr const char* kStarted = ".ttt2-installing"; // an install not finished yet
inline constexpr const char* kBuildRecord = ".ttt2-build";  // "kit <version>": what built the program

// The user's folders. Each follows the rule of the code that uses it: the
// installer takes $XDG_CONFIG_HOME and $XDG_DATA_HOME only when absolute,
// the shader cache and the character packs take any that is set, saves and
// Tekken 7's saved data are always below the home folder.
struct Places {
    fs::path home;
    fs::path config;       // $XDG_CONFIG_HOME, else ~/.config
    fs::path data;         // $XDG_DATA_HOME, else ~/.local/share
    fs::path cache;        // $XDG_CACHE_HOME, else ~/.cache
    fs::path saves;        // $TTT2_SAVE_DIR, else default_saves()
    fs::path packs;        // $TTT2_CHARACTERS, else $XDG_DATA_HOME/ttt2/characters, else ~/.local/share/...
    fs::path tekken7_data; // ~/.local/share/ttt2/tekken7: movesets and files saved from Tekken 7

    fs::path record() const { return config / "ttt2" / "installed"; }
    // The launcher's own record of an install it started that has not
    // finished: the folder and the game it is installed from (the game's
    // record is written only once an install is complete).
    fs::path started_record() const { return config / "ttt2" / "installing"; }
    fs::path desktop_entry() const { return data / "applications" / "ttt2.desktop"; }
    // The game's keyboard keys (keymap.h): a setting, kept by an uninstall.
    fs::path keyboard() const { return config / "ttt2" / "keyboard.ini"; }
    // The game's display settings (display.h): a setting, kept by an uninstall.
    fs::path display() const { return config / "ttt2" / "display.ini"; }
    fs::path shader_cache() const { return cache / "ttt2"; }
    fs::path build_work() const { return cache / "ttt2" / "build"; } // ttt2-build's default --work
    // Steam's last answer to which DLC the account owns (launcher.py owned),
    // shown at once on the next start while Steam is asked again.
    fs::path owned_cache() const { return cache / "ttt2" / "dlc-owned.json"; }
    fs::path default_saves() const { return home / ".local/share/ttt2/save"; }

    static Places from_environment();
};

enum class InstallState { kNone, kUnfinished, kInstalled };

struct Install {
    InstallState state = InstallState::kNone;
    fs::path folder;
    fs::path game; // what an unfinished install was installed from, when the launcher started it
};

// The install the launcher shows: one it started and that has not finished,
// else the game's recorded install (complete, or unfinished when an install
// over it stopped).
Install find_install(const Places& places);

// The launcher's record of the install it starts (see Places).
bool write_started_record(const Places& places, const fs::path& folder, const fs::path& game);
std::optional<Install> read_started_record(const Places& places);

// The folder an install into `chosen` goes to, as ttt2 --install picks it: a
// new or empty folder, or one with an install, is the install folder; any
// other gets a "Tekken Tag Tournament 2" folder inside. None, with the
// reason as said of the chosen folder ("is not a folder"), when that one is
// taken too.
std::optional<fs::path> install_target(const fs::path& chosen, std::string* error = nullptr);

// The same path, absolute, without "." and ".." parts or a trailing slash.
fs::path normal(const fs::path& path);
// Whether `path` is `folder` or inside it (also through their real paths,
// when one of them goes through a symbolic link).
bool inside(const fs::path& path, const fs::path& folder);

// Whether the program of the install in `folder` runs, whoever started it
// (this launcher, another one, the menu entry, a terminal): a process whose
// /proc/<pid>/exe is that file, also once it is deleted or replaced.
bool program_running(const fs::path& folder);

// The program a desktop entry's Exec key starts (its first argument,
// unquoted and unescaped as the specification says).
std::optional<fs::path> desktop_program(std::string_view entry);

// The kit's version (kit/VERSION) and the one that built an install's program
// (<folder>/.ttt2-build), empty when unknown.
std::string kit_version(const fs::path& kit);
std::string built_version(const fs::path& folder);

// A program found in $PATH, as a shell would; none when not found.
std::optional<fs::path> find_in_path(std::string_view name);

// The builder's kit (with bin/ttt2-build), the importer (with launcher.py)
// and Python: the first found of $TTT2_KIT / $TTT2_IMPORTER / $TTT2_PYTHON,
// next to the launcher's program (as installed: <prefix>/bin/ttt2-launcher),
// in the build tree it was built in (or the source tree; not looked in when
// that path is empty, as in a package), or for Python in $PATH. A variable
// that is set wins even when it names nothing usable, so a mistake shows
// instead of another kit being used.
fs::path find_kit(const fs::path& program_dir, const fs::path& build_tree_kit);
fs::path find_importer(const fs::path& program_dir, const fs::path& source_importer);
fs::path find_python(const fs::path& program_dir);

// The first line of a file, without its line break; empty when unreadable.
std::string first_line(const fs::path& file);

} // namespace launcher
