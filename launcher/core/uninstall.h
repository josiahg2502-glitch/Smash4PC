#pragma once

// Uninstalling: only what the install made is deleted. In the install
// folder that is the program, icon.png, game/, the install's markers and
// records and the unfinished program and record ("<name>.part"); then the
// folder when nothing else is left in it (and the link to it, when it was
// reached through one). Outside it: the menu entry when it starts the
// program in that folder, the install record when it names that folder, the
// compiled shaders, the builder's work folder and the DLC answer kept, and
// when asked the saves the game wrote (its "common" and per-user folders,
// not anything else in a save folder the player named), and the character
// packs with what was saved from Tekken 7 for them. Nothing is ever deleted
// through a symbolic link (a link is deleted itself, not what it points
// to), nor in another file system mounted inside a folder that is deleted,
// nor while the game runs from that folder.

#include "paths.h"

#include <string>
#include <vector>

namespace launcher {

struct UninstallOptions {
    bool saves = false;
    bool characters = false; // the packs, and the movesets and files saved from Tekken 7
};

// What could not be deleted, and why. (The path apart, as its bytes: the
// window shows it as GLib shows file names.)
struct UninstallProblem {
    fs::path path;
    std::string why;
    std::string text() const { return path.string() + ": " + why; }
};

struct UninstallReport {
    std::string refused;           // why nothing was deleted (not an install, the game runs); empty: it ran
    bool folder_removed = false;   // the install folder is gone
    std::vector<std::string> left; // the names of what is left in the install folder (not the game's)
    std::vector<UninstallProblem> problems;
};

// Takes a while for the game's 16 GB on some file systems: not for the
// window's thread.
UninstallReport uninstall(const Places& places, const fs::path& folder, const UninstallOptions& options);

} // namespace launcher
