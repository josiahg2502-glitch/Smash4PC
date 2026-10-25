#pragma once

// What the importer tells about the player's PC (launcher.py status and
// owned, see tools/characters/launcher.py), read into plain structures.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

struct Fighter {
    std::string name;    // the importer's name: "kazumi"
    std::string display; // "Kazumi Mishima"
    int64_t dlc = 0;     // the Steam app id of the DLC that has them, 0 for Tekken 7's own
    bool installed = false; // their pack is there
    // Their pack is an older importer's: the game skips it, and adding them
    // again replaces it. (Only with `installed`.)
    bool outdated = false;
    // Their pack is a newer importer's: the game skips it too (it needs
    // updating), and this importer does not replace it. (Only with
    // `installed`, never with `outdated`.)
    bool newer = false;
    bool moveset_saved = false;

    // In the game: a pack it loads.
    bool added() const { return installed && !outdated && !newer; }
};

struct ImporterStatus {
    std::string python_version;
    bool numpy = false, pillow = false;
    std::string gamescope, pactl; // their paths, empty when missing
    std::string helper;           // "prebuilt", "mingw", or empty: no way to make Tekken 7's helper
    bool steam_installed = false, steam_running = false;
    bool tekken7_found = false;
    std::string tekken7_path;
    std::string tekken7_error; // why Tekken 7 is not usable, empty when it is
    std::string packs_folder;
    bool heihachi_saved = true; // his moveset, which every import needs (unknown: taken as saved)
    std::string game_error;     // with --game: why the importer cannot read that install
    std::vector<Fighter> fighters;

    // Whether Tekken 7 can be used: found, nothing wrong with it.
    bool tekken7_usable() const { return tekken7_found && tekken7_error.empty(); }
};

std::optional<ImporterStatus> parse_status(std::string_view json, std::string* error = nullptr);

// launcher.py owned: the DLC fighters' ownership, or why it is not known.
struct Ownership {
    std::map<std::string, bool> owned;
    std::string error;
};

Ownership parse_owned(std::string_view json);

// Steam's last answer, kept (Places::owned_cache) so that the next start
// shows it at once: Steam's first answer after a boot takes most of a
// minute. Only an answer without an error is saved; none when there is none
// or it is not readable.
bool save_ownership(const std::filesystem::path& file, std::string_view json);
std::optional<Ownership> saved_ownership(const std::filesystem::path& file);

} // namespace launcher
