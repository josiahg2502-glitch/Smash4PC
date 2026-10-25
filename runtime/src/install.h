#pragma once

// Installing the port like any other program: `ttt2 --install` copies the
// title out of its .wua archive (or a title folder) into a folder the user
// picks, with this executable, the title's icon and an application-menu
// entry. The installed executable, or this one started without arguments,
// then finds the game by itself.
//
// <install folder>/Tekken Tag Tournament 2   the executable
// <install folder>/icon.png                  meta/iconTex.tga
// <install folder>/game/                     code/, content/, meta/ of the title
// <install folder>/.ttt2-install             written last: the install is complete
// $XDG_CONFIG_HOME/ttt2/installed            the install folder
// $XDG_DATA_HOME/applications/ttt2.desktop   "Tekken Tag Tournament 2" in the menu
//                                            (ttt2 is the window's app id)

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cafe::install {

// ttt2 --install [GAME [FOLDER]] [--no-launcher], ttt2 --update; `args`
// excludes the program name. Returns the process exit code. With
// TTT2_INSTALL_PROGRESS=1 it also prints "@folder <path>", "@progress <done>
// <total>" (bytes) and "@error <text>" lines on stdout for the launcher.
int run(const std::vector<std::string>& args);

// The installed game's title folder: next to this executable, else where
// the last install went. None if there is no complete install.
std::optional<std::filesystem::path> installed_game();

// A TGA image (the title's icon and boot images) as RGBA rows, top first.
bool decode_tga(const std::vector<uint8_t>& tga, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);

} // namespace cafe::install
