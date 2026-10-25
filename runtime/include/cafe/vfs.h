#pragma once

// The title's view of storage. /vol/code, /vol/content and /vol/meta come
// read-only from the game source: the original .wua archive (read in place)
// or an extracted title directory. /vol/save is a host directory.
// Guest paths are case-insensitive, as on the console.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cafe::vfs {

// Results use the console's FSStatus values.
enum Status : int32_t {
    kOk = 0,
    kEnd = -2,
    kMax = -3,
    kAlreadyOpen = -4,
    kExists = -5,
    kNotFound = -6,
    kNotFile = -7,
    kNotDir = -8,
    kAccessError = -9,
    kPermissionError = -10,
    kStorageFull = -12,
    kMediaError = -17,
    kFatal = -0x400,
};

struct Stat {
    bool directory = false;
    uint64_t size = 0;
};

struct DirEntry {
    std::string name;
    Stat stat;
};

// `source` is a .wua file or a directory holding code/, content/, meta/.
void open_game(const std::filesystem::path& source);
// Host folders holding code/, content/, meta/, layered: the first that has a file serves it (an update over its
// base game).
void open_game(const std::vector<std::filesystem::path>& layers);
void set_save_root(const std::filesystem::path& root);
// A DLC title (its content/ folder) at /vol/aoc<title id, 16 hex digits>, read-only; aoc_path() is that path, empty
// when the title is not mounted.
void mount_aoc(uint64_t title_id, const std::filesystem::path& content);
std::string aoc_path(uint64_t title_id);

// The title's archive `archive` ("content/hdd/data003": its .ofs and .bin
// files) with the entries named in `stored` (by name hash, as stored: see
// title/archive.h): ones it holds replaced, others added. Calls add up, a
// later one winning for the same entry. Call before the title runs; false
// if the archive cannot take them.
bool set_archive_entries(const std::string& archive, const std::map<uint32_t, std::vector<uint8_t>>& stored);

// Calls `reader` with the name hash of each archive entry the title reads
// (by where it reads in a dataNNN.bin), from the reading thread; for test
// scripts that wait for a screen (TTT2_INPUT_SCRIPT's anchors).
void watch_entry_reads(std::function<void(uint32_t)> reader);

std::optional<std::vector<uint8_t>> read_whole(std::string_view path);

Status open_file(std::string_view path, std::string_view mode, int32_t& handle);
Status close_file(int32_t handle);
// Reads up to `size` bytes at the file position (or at `position` when
// given), advancing it; `transferred` receives the byte count.
Status read_file(int32_t handle, void* buffer, uint64_t size, std::optional<uint64_t> position,
                 uint64_t& transferred);
Status write_file(int32_t handle, const void* buffer, uint64_t size, std::optional<uint64_t> position,
                  uint64_t& transferred);
Status seek_file(int32_t handle, uint64_t position);
Status tell_file(int32_t handle, uint64_t& position);
Status stat_file(int32_t handle, Stat& stat);
Status flush_file(int32_t handle);

Status stat_path(std::string_view path, Stat& stat);
Status make_dir(std::string_view path);
// Only in /vol/save (and the SD card): a file or an empty folder.
Status remove(std::string_view path);
Status rename(std::string_view from, std::string_view to);
Status open_dir(std::string_view path, int32_t& handle);
Status read_dir(int32_t handle, DirEntry& entry);
Status close_dir(int32_t handle);

} // namespace cafe::vfs
