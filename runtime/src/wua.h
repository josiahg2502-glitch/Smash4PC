#pragma once

// The game in a .wua archive (Cemu's ZArchive of installed titles), which
// holds a folder "<title id>_v<version>" per title: the game (title id
// 00050000xxxxxxxx), its update (0005000Exxxxxxxx: all of code/ and meta/,
// only the changed files of content/) and DLC (0005000Cxxxxxxxx). An
// archive has the update merged into the game's folder or the two side by
// side; side by side, the game's files are the update's over the base's.
// DLC is left out: the console mounts it apart (/vol/aoc...), never over
// /vol/content, and the title does not use it.

// Third-party header; not held to this project's warning flags.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wnested-anon-types"
#endif
#include <zarchive/zarchivereader.h>
#pragma GCC diagnostic pop

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cafe::wua {

// The folders that make up the game (the title with code/Tekken.rpx), the
// first taking precedence: updates before the base game, higher versions
// before lower. Empty if the archive holds no such title.
std::vector<std::string> game_layers(ZArchiveReader& archive);

// A path in the game ("content/hdd/x.bin", any case) in the first layer
// that has it.
ZArchiveNodeHandle find(ZArchiveReader& archive, const std::vector<std::string>& layers, std::string_view path);

struct Entry {
    std::string name;
    bool directory = false;
    uint64_t size = 0;
};
// The folder's entries in every layer that has it as a folder, each name
// once (case-insensitively), as the first layer with that name has it.
// False if an entry could not be read (the others are listed).
bool list(ZArchiveReader& archive, const std::vector<std::string>& layers, std::string_view path,
          std::vector<Entry>& entries);

struct File {
    std::string path; // each name spelled as the first layer with it has it
    uint64_t size = 0;
    ZArchiveNodeHandle node = ZARCHIVE_INVALID_NODE;
    size_t layer = 0; // the index in `layers` it comes from
};
// All of the game's files, each from the first layer that has it: layer by
// layer, in the archive's order within each. Throws std::runtime_error for a
// damaged archive or a name unsafe as a host file name.
std::vector<File> game_files(ZArchiveReader& archive, const std::vector<std::string>& layers);

} // namespace cafe::wua
