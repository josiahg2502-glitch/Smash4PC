#include "cafe/vfs.h"

#include "cafe/runtime.h"
#include "title/archive.h"
#include "wua.h"
#include "host/files.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <functional>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace cafe::vfs {
namespace {

namespace fs = std::filesystem;

struct Source {
    std::unique_ptr<ZArchiveReader> archive; // null: host directory
    std::vector<std::string> layers;         // the game's folders in the archive, the first winning
    std::vector<fs::path> directories; // host folders, the first holding a path winning
};

Source g_source;
fs::path g_save_root;
std::mutex g_mutex;

// Archives served with entries replaced or added (set_archive_entries), by
// the relative path without extension, lower case: the entries asked for so
// far, and the archive rebuilt with them.
std::map<std::string, std::map<uint32_t, std::vector<uint8_t>>> g_overlay_entries;
std::map<std::string, std::shared_ptr<const title::archive::Rebuilt>> g_overlays;

struct File {
    bool archive = false;
    ZArchiveNodeHandle node = ZARCHIVE_INVALID_NODE;
    int fd = -1;
    uint64_t size = 0;
    uint64_t position = 0;
    bool readable = false;
    bool writable = false;
    bool append = false;
    // An overlaid archive's index (served from memory) or data (pieces of
    // the file opened above and of the replacements).
    std::shared_ptr<const title::archive::Rebuilt> overlay;
    bool overlay_index = false;
    std::string archive_bin; // "content/hdd/data003" for an archive's .bin (watch_entry_reads)
};
struct Dir {
    std::vector<DirEntry> entries;
    size_t next = 0;
};
std::unordered_map<int32_t, File> g_files;

// watch_entry_reads: the callback, and each archive's entries as {start,
// end, name hash} in its .bin, sorted.
std::function<void(uint32_t)> g_entry_reader;
// TTT2_TRACE_ENTRIES=1: every archive entry the title reads, with the time.
const bool g_trace_entries = std::getenv("TTT2_TRACE_ENTRIES") != nullptr;
const auto g_trace_start = std::chrono::steady_clock::now();
struct EntrySpan {
    uint64_t start, end;
    uint32_t hash;
};
std::map<std::string, std::vector<EntrySpan>> g_entry_spans;
std::unordered_map<int32_t, Dir> g_dirs;
int32_t g_next_handle = 1;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The overlay serving `relative` ("content/hdd/data003.bin"), if any;
// `index` tells the .ofs from the .bin.
std::shared_ptr<const title::archive::Rebuilt> overlay_of(const std::string& relative, bool& index) {
    if (g_overlays.empty() || relative.size() < 4) return nullptr;
    const std::string name = lower(relative);
    const std::string extension = name.substr(name.size() - 4);
    if (extension != ".ofs" && extension != ".bin") return nullptr;
    const auto it = g_overlays.find(name.substr(0, name.size() - 4));
    if (it == g_overlays.end()) return nullptr;
    index = extension == ".ofs";
    return it->second;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// "/vol/content/./a//b/../c" -> {"vol", "content", "a", "c"}
std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t j = i;
        while (j < path.size() && path[j] != '/') ++j;
        const std::string_view part = path.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.emplace_back(part);
        }
        i = j;
    }
    return parts;
}

// kAoc: a DLC title's content folder (mount_aoc), read-only, at /vol/aoc<title id>.
enum class Area { kGame, kSave, kAoc, kNone };

struct Resolved {
    Area area = Area::kNone;
    std::string relative;              // inside the game source: "content/hdd/x.bin"
    std::vector<std::string> save_parts; // inside the save root, or the DLC folder for kAoc
};

std::map<std::string, fs::path> g_aoc; // "aoc0005000c10144f00" -> the DLC's content folder

// The host folder a save or DLC path is in.
fs::path area_root(const Resolved& r, std::string_view path) {
    if (r.area != Area::kAoc) return g_save_root;
    std::string name = components(path)[1];
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return g_aoc.at(name);
}

Resolved resolve(std::string_view path) {
    const auto parts = components(path);
    Resolved r;
    if (parts.size() < 2 || !iequals(parts[0], "vol")) return r;
    if (iequals(parts[1], "save")) {
        r.area = Area::kSave;
        r.save_parts.assign(parts.begin() + 2, parts.end());
        return r;
    }
    // The SD card (FSMount's /vol/external01): a folder beside the saves.
    // shortcut: it lives at <save root>/sdcard, so /vol/save/sdcard is the same folder; give it its own root if a
    // title saves a folder of that name.
    if (iequals(parts[1], "external01")) {
        r.area = Area::kSave;
        r.save_parts = {"sdcard"};
        r.save_parts.insert(r.save_parts.end(), parts.begin() + 2, parts.end());
        return r;
    }
    std::string lower = parts[1];
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (g_aoc.count(lower)) {
        r.area = Area::kAoc;
        r.save_parts.assign(parts.begin() + 2, parts.end());
        return r;
    }
    if (iequals(parts[1], "content") || iequals(parts[1], "code") || iequals(parts[1], "meta")) {
        r.area = Area::kGame;
        for (size_t i = 1; i < parts.size(); ++i) {
            if (!r.relative.empty()) r.relative += '/';
            std::string part = parts[i];
            if (i == 1) std::transform(part.begin(), part.end(), part.begin(), ::tolower);
            r.relative += part;
        }
    }
    return r;
}

// Finds a host path component case-insensitively; keeps `name` if none
// exists yet (for creating files).
fs::path host_resolve(const fs::path& root, const std::vector<std::string>& parts) {
    fs::path current = root;
    for (const std::string& part : parts) {
        fs::path exact = current / part;
        std::error_code ec;
        if (fs::exists(exact, ec)) {
            current = exact;
            continue;
        }
        fs::path found;
        for (const auto& entry : fs::directory_iterator(current, ec)) {
            if (iequals(entry.path().filename().string(), part)) {
                found = entry.path();
                break;
            }
        }
        current = found.empty() ? exact : found;
    }
    return current;
}

fs::path game_host_path(const std::string& relative) {
    const auto parts = components(relative);
    for (const fs::path& directory : g_source.directories) {
        fs::path path = host_resolve(directory, parts);
        std::error_code ec;
        if (fs::exists(path, ec)) return path;
    }
    return host_resolve(g_source.directories.front(), parts);
}

Status from_errno(int error) {
    switch (error) {
    case ENOENT: return kNotFound;
    case EISDIR: return kNotFile;
    case ENOTDIR: return kNotDir;
    case EACCES: case EPERM: case EROFS: return kPermissionError;
    case EEXIST: return kExists;
    case ENOSPC: return kStorageFull;
    case EMFILE: case ENFILE: return kMax;
    default: return kMediaError;
    }
}

Status open_host(const fs::path& path, std::string_view mode, int32_t& handle) {
    File file;
    int flags = 0;
    const bool plus = mode.find('+') != std::string_view::npos;
    switch (mode.empty() ? 'r' : mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; file.readable = true; file.writable = plus; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; file.writable = true; file.readable = plus; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT; file.writable = true; file.append = true; file.readable = plus; break;
    default: return kAccessError;
    }
    host::FileStat st;
    if (host::file_stat(path, st) == 0 && S_ISDIR(st.st_mode)) return kNotFile;
    file.fd = host::file_open(path, flags);
    if (file.fd < 0) return from_errno(errno);
    host::file_fstat(file.fd, st);
    file.size = static_cast<uint64_t>(st.st_size);
    handle = g_next_handle++;
    g_files[handle] = file;
    return kOk;
}

Status stat_host(const fs::path& path, Stat& stat) {
    host::FileStat st;
    if (host::file_stat(path, st) != 0) return from_errno(errno);
    stat.directory = S_ISDIR(st.st_mode);
    stat.size = stat.directory ? 0 : static_cast<uint64_t>(st.st_size);
    return kOk;
}

Status list_host(const fs::path& path, Dir& dir) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return fs::exists(path, ec) ? kNotDir : kNotFound;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        DirEntry e;
        e.name = entry.path().filename().string();
        stat_host(entry.path(), e.stat);
        dir.entries.push_back(std::move(e));
    }
    return kOk;
}

// Opens a file of the game source for reading, without overlays.
Status open_underlying(const std::string& relative, File& file) {
    if (!g_source.archive) {
        const fs::path path = game_host_path(relative);
        host::FileStat st;
        if (host::file_stat(path, st) == 0 && S_ISDIR(st.st_mode)) return kNotFile;
        file.fd = host::file_open(path, O_RDONLY);
        if (file.fd < 0) return from_errno(errno);
        host::file_fstat(file.fd, st);
        file.size = static_cast<uint64_t>(st.st_size);
        return kOk;
    }
    const ZArchiveNodeHandle node = wua::find(*g_source.archive, g_source.layers, relative);
    if (node == ZARCHIVE_INVALID_NODE) return kNotFound;
    if (!g_source.archive->IsFile(node)) return kNotFile;
    file.archive = true;
    file.node = node;
    file.size = g_source.archive->GetFileSize(node);
    return kOk;
}

uint64_t read_underlying(const File& file, uint64_t position, uint64_t size, void* buffer) {
    if (file.archive) return g_source.archive->ReadFromFile(file.node, position, size, buffer);
    uint64_t transferred = 0;
    while (transferred < size) {
        const int64_t n = host::file_pread(file.fd, static_cast<uint8_t*>(buffer) + transferred, size - transferred,
                                           position + transferred);
        if (n <= 0) break;
        transferred += static_cast<uint64_t>(n);
    }
    return transferred;
}

// The whole of a game file, or its start, without overlays.
std::optional<std::vector<uint8_t>> read_underlying(const std::string& relative, uint64_t limit) {
    File file;
    if (open_underlying(relative, file) != kOk) return std::nullopt;
    std::vector<uint8_t> data(std::min(file.size, limit));
    const uint64_t n = read_underlying(file, 0, data.size(), data.data());
    if (file.fd >= 0) host::file_close(file.fd);
    if (n != data.size()) return std::nullopt;
    return data;
}

// The entries of `archive` ("content/hdd/data003") a read of its .bin from
// `start` to `end` touches. Called with g_mutex held.
std::vector<uint32_t> entries_read(const std::string& archive, uint64_t start, uint64_t end) {
    auto it = g_entry_spans.find(archive);
    if (it == g_entry_spans.end()) {
        std::vector<EntrySpan> spans;
        std::optional<std::vector<uint8_t>> ofs;
        if (const auto overlay = g_overlays.find(archive); overlay != g_overlays.end()) {
            ofs = overlay->second->ofs;
        } else {
            ofs = read_underlying(archive + ".ofs", UINT64_MAX);
        }
        if (const auto index = ofs ? title::archive::parse_index(*ofs) : std::nullopt) {
            for (const title::archive::Entry& e : index->entries) {
                spans.push_back({title::archive::kPayload + e.offset, title::archive::kPayload + e.offset + e.size, e.hash});
            }
            std::sort(spans.begin(), spans.end(), [](const EntrySpan& a, const EntrySpan& b) { return a.start < b.start; });
        }
        it = g_entry_spans.emplace(archive, std::move(spans)).first;
    }
    std::vector<uint32_t> hashes;
    const auto& spans = it->second;
    auto span = std::upper_bound(spans.begin(), spans.end(), start, [](uint64_t at, const EntrySpan& s) { return at < s.start; });
    if (span != spans.begin()) --span;
    for (; span != spans.end() && span->start < end; ++span) {
        if (span->end > start) hashes.push_back(span->hash);
    }
    return hashes;
}

} // namespace

void watch_entry_reads(std::function<void(uint32_t)> reader) {
    std::lock_guard lock(g_mutex);
    g_entry_reader = std::move(reader);
}

bool set_archive_entries(const std::string& archive, const std::map<uint32_t, std::vector<uint8_t>>& stored) {
    std::lock_guard lock(g_mutex);
    const auto ofs = read_underlying(archive + ".ofs", UINT64_MAX);
    const auto header = read_underlying(archive + ".bin", title::archive::kPayload);
    if (!ofs || !header) return false;
    std::map<uint32_t, std::vector<uint8_t>> entries = g_overlay_entries[lower(archive)];
    for (const auto& [hash, data] : stored) entries[hash] = data;
    auto rebuilt = title::archive::rebuild(*ofs, *header, entries);
    if (!rebuilt) return false;
    g_overlay_entries[lower(archive)] = std::move(entries);
    g_overlays[lower(archive)] = std::make_shared<const title::archive::Rebuilt>(std::move(*rebuilt));
    return true;
}

void open_game(const std::vector<fs::path>& layers) {
    g_source = Source{};
    g_source.directories = layers;
}

void open_game(const fs::path& source) {
    g_source = Source{};
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        g_source.directories = {source};
        return;
    }
    g_source.archive.reset(ZArchiveReader::OpenFromFile(source));
    if (!g_source.archive) fatal("cannot open game archive %s", source.string().c_str());
    g_source.layers = wua::game_layers(*g_source.archive);
    if (g_source.layers.empty()) fatal("%s does not contain a title with code/Tekken.rpx", source.string().c_str());
    if (g_source.layers.size() > 1) {
        std::string order;
        for (const std::string& layer : g_source.layers) order += (order.empty() ? "" : " over ") + layer;
        std::fprintf(stderr, "ttt2: the game's files from %s\n", order.c_str());
    }
}

void set_save_root(const fs::path& root) {
    g_save_root = root;
    std::error_code ec;
    fs::create_directories(root, ec);
}

void mount_aoc(uint64_t title_id, const fs::path& content) {
    char name[32];
    std::snprintf(name, sizeof(name), "aoc%016llx", static_cast<unsigned long long>(title_id));
    g_aoc[name] = content;
}

std::string aoc_path(uint64_t title_id) {
    char path[32];
    std::snprintf(path, sizeof(path), "/vol/aoc%016llx", static_cast<unsigned long long>(title_id));
    return g_aoc.count(path + 5) ? std::string(path) : std::string();
}

std::optional<std::vector<uint8_t>> read_whole(std::string_view path) {
    int32_t handle;
    if (open_file(path, "r", handle) != kOk) return std::nullopt;
    Stat stat;
    stat_file(handle, stat);
    std::vector<uint8_t> data(stat.size);
    uint64_t transferred = 0;
    read_file(handle, data.data(), data.size(), 0, transferred);
    close_file(handle);
    data.resize(transferred);
    return data;
}

Status open_file_impl(std::string_view path, std::string_view mode, int32_t& handle) {
    const Resolved r = resolve(path);
    if (r.area == Area::kSave) return open_host(host_resolve(g_save_root, r.save_parts), mode, handle);
    if (r.area == Area::kAoc) {
        if (mode.find_first_of("wa+") != std::string_view::npos) return kPermissionError; // DLC is read-only
        return open_host(host_resolve(area_root(r, path), r.save_parts), mode, handle);
    }
    if (r.area != Area::kGame) return kNotFound;
    const bool writing = !mode.empty() && (mode[0] != 'r' || mode.find('+') != std::string_view::npos);
    bool index = false;
    if (auto overlay = overlay_of(r.relative, index)) {
        if (writing) return kPermissionError;
        File file;
        if (!index) { // the original data, read through
            const Status status = open_underlying(r.relative, file);
            if (status != kOk) return status;
        }
        file.overlay = std::move(overlay);
        file.overlay_index = index;
        file.size = index ? file.overlay->ofs.size() : file.overlay->bin_size;
        file.readable = true;
        handle = g_next_handle++;
        g_files[handle] = std::move(file);
        return kOk;
    }
    if (!g_source.archive) {
        if (writing) return kPermissionError;
        return open_host(game_host_path(r.relative), "r", handle);
    }
    const ZArchiveNodeHandle node = wua::find(*g_source.archive, g_source.layers, r.relative);
    if (node == ZARCHIVE_INVALID_NODE) return kNotFound;
    if (!g_source.archive->IsFile(node)) return kNotFile;
    if (writing) return kPermissionError;
    File file;
    file.archive = true;
    file.node = node;
    file.size = g_source.archive->GetFileSize(node);
    file.readable = true;
    handle = g_next_handle++;
    g_files[handle] = file;
    return kOk;
}

Status open_file(std::string_view path, std::string_view mode, int32_t& handle) {
    std::lock_guard lock(g_mutex);
    const Status status = open_file_impl(path, mode, handle);
    if (status == kOk && (g_entry_reader || g_trace_entries)) {
        const Resolved r = resolve(path);
        const std::string name = lower(r.relative);
        if (r.area == Area::kGame && name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0 &&
            name.compare(0, 16, "content/hdd/data") == 0) {
            g_files[handle].archive_bin = name.substr(0, name.size() - 4);
        }
    }
    return status;
}

Status close_file(int32_t handle) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    if (it->second.fd >= 0) host::file_close(it->second.fd);
    g_files.erase(it);
    return kOk;
}

Status read_file(int32_t handle, void* buffer, uint64_t size, std::optional<uint64_t> position,
                 uint64_t& transferred) {
    File file;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_files.find(handle);
        if (it == g_files.end()) return kFatal;
        if (!it->second.readable) return kAccessError;
        if (position) it->second.position = *position;
        file = it->second;
    }
    transferred = 0;
    if (file.position < file.size) {
        const uint64_t want = std::min(size, file.size - file.position);
        if (file.overlay && file.overlay_index) {
            std::memcpy(buffer, file.overlay->ofs.data() + file.position, want);
            transferred = want;
        } else if (file.overlay) {
            auto* out = static_cast<uint8_t*>(buffer);
            for (const title::archive::Piece& piece : file.overlay->bin) {
                const uint64_t at = file.position + transferred;
                if (transferred >= want) break;
                if (piece.at + piece.size <= at || piece.at > at) continue;
                const uint64_t n = std::min(want - transferred, piece.at + piece.size - at);
                if (piece.data) {
                    std::memcpy(out + transferred, piece.data->data() + (at - piece.at), n);
                } else if (read_underlying(file, piece.source + (at - piece.at), n, out + transferred) != n) {
                    break;
                }
                transferred += n;
            }
        } else {
            transferred = read_underlying(file, file.position, want, buffer);
        }
    }
    std::function<void(uint32_t)> reader;
    std::vector<uint32_t> read_entries;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_files.find(handle);
        if (it != g_files.end()) it->second.position = file.position + transferred;
        if ((g_entry_reader || g_trace_entries) && !file.archive_bin.empty() && transferred != 0) {
            reader = g_entry_reader;
            read_entries = entries_read(file.archive_bin, file.position, file.position + transferred);
        }
    }
    for (uint32_t hash : read_entries) {
        if (g_trace_entries) {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_trace_start).count();
            std::fprintf(stderr, "entry: %8.3f %s %08x\n", t, file.archive_bin.c_str() + 12, hash);
        }
        if (reader) reader(hash);
    }
    return kOk;
}

Status write_file(int32_t handle, const void* buffer, uint64_t size, std::optional<uint64_t> position,
                  uint64_t& transferred) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    File& file = it->second;
    if (!file.writable || file.fd < 0) return kAccessError;
    if (position) file.position = *position;
    if (file.append) file.position = file.size;
    transferred = 0;
    while (transferred < size) {
        const int64_t n = host::file_pwrite(file.fd, static_cast<const uint8_t*>(buffer) + transferred,
                                            size - transferred, file.position + transferred);
        if (n < 0) return from_errno(errno);
        transferred += static_cast<uint64_t>(n);
    }
    file.position += transferred;
    file.size = std::max(file.size, file.position);
    return kOk;
}

Status seek_file(int32_t handle, uint64_t position) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    it->second.position = position;
    return kOk;
}

Status tell_file(int32_t handle, uint64_t& position) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    position = it->second.position;
    return kOk;
}

Status stat_file(int32_t handle, Stat& stat) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    stat.directory = false;
    stat.size = it->second.size;
    return kOk;
}

Status flush_file(int32_t handle) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    if (it->second.fd >= 0) host::file_sync(it->second.fd);
    return kOk;
}

Status stat_path(std::string_view path, Stat& stat) {
    std::lock_guard lock(g_mutex);
    const Resolved r = resolve(path);
    if (r.area == Area::kSave || r.area == Area::kAoc) return stat_host(host_resolve(area_root(r, path), r.save_parts), stat);
    if (r.area != Area::kGame) return kNotFound;
    bool index = false;
    if (const auto overlay = overlay_of(r.relative, index)) {
        stat.directory = false;
        stat.size = index ? overlay->ofs.size() : overlay->bin_size;
        return kOk;
    }
    if (!g_source.archive) return stat_host(game_host_path(r.relative), stat);
    const ZArchiveNodeHandle node = wua::find(*g_source.archive, g_source.layers, r.relative);
    if (node == ZARCHIVE_INVALID_NODE) return kNotFound;
    stat.directory = g_source.archive->IsDirectory(node);
    stat.size = stat.directory ? 0 : g_source.archive->GetFileSize(node);
    return kOk;
}

Status make_dir(std::string_view path) {
    std::lock_guard lock(g_mutex);
    const Resolved r = resolve(path);
    if (r.area != Area::kSave) return kPermissionError;
    const fs::path host = host_resolve(g_save_root, r.save_parts);
    std::error_code ec;
    if (fs::exists(host, ec)) return kExists;
    if (!fs::create_directory(host, ec)) return from_errno(ec.default_error_condition().value());
    return kOk;
}

// A file or an empty folder in the writable areas.
Status remove(std::string_view path) {
    std::lock_guard lock(g_mutex);
    const Resolved r = resolve(path);
    if (r.area != Area::kSave) return kPermissionError;
    std::error_code ec;
    if (!fs::remove(host_resolve(g_save_root, r.save_parts), ec)) {
        return ec ? from_errno(ec.default_error_condition().value()) : kNotFound;
    }
    return kOk;
}

Status rename(std::string_view from, std::string_view to) {
    std::lock_guard lock(g_mutex);
    const Resolved a = resolve(from), b = resolve(to);
    if (a.area != Area::kSave || b.area != Area::kSave) return kPermissionError;
    std::error_code ec;
    fs::rename(host_resolve(g_save_root, a.save_parts), host_resolve(g_save_root, b.save_parts), ec);
    return ec ? from_errno(ec.default_error_condition().value()) : kOk;
}

Status open_dir(std::string_view path, int32_t& handle) {
    std::lock_guard lock(g_mutex);
    const Resolved r = resolve(path);
    Dir dir;
    Status status = kNotFound;
    if (r.area == Area::kSave || r.area == Area::kAoc) {
        status = list_host(host_resolve(area_root(r, path), r.save_parts), dir);
    } else if (r.area == Area::kGame && !g_source.archive) {
        // Every layer's folder, merged (a mod or update replaces part of one); the first layer's entry wins.
        const auto parts = components(r.relative);
        for (const fs::path& directory : g_source.directories) {
            Dir layer;
            if (list_host(host_resolve(directory, parts), layer) != kOk) continue;
            for (DirEntry& e : layer.entries) {
                const bool seen = std::any_of(dir.entries.begin(), dir.entries.end(),
                                              [&](const DirEntry& d) { return d.name == e.name; });
                if (!seen) dir.entries.push_back(std::move(e));
            }
            status = kOk;
        }
    } else if (r.area == Area::kGame) {
        const ZArchiveNodeHandle node = wua::find(*g_source.archive, g_source.layers, r.relative);
        std::vector<wua::Entry> entries;
        if (node == ZARCHIVE_INVALID_NODE) {
            status = kNotFound;
        } else if (!g_source.archive->IsDirectory(node)) {
            status = kNotDir;
        } else {
            wua::list(*g_source.archive, g_source.layers, r.relative, entries);
            for (wua::Entry& entry : entries) {
                DirEntry e;
                e.name = std::move(entry.name);
                e.stat.directory = entry.directory;
                e.stat.size = entry.size;
                dir.entries.push_back(std::move(e));
            }
            status = kOk;
        }
    }
    if (status != kOk) return status;
    for (DirEntry& e : dir.entries) {
        bool index = false;
        if (r.area != Area::kGame || e.stat.directory) continue;
        if (const auto overlay = overlay_of(r.relative + "/" + e.name, index)) {
            e.stat.size = index ? overlay->ofs.size() : overlay->bin_size;
        }
    }
    handle = g_next_handle++;
    g_dirs[handle] = std::move(dir);
    return kOk;
}

Status read_dir(int32_t handle, DirEntry& entry) {
    std::lock_guard lock(g_mutex);
    const auto it = g_dirs.find(handle);
    if (it == g_dirs.end()) return kFatal;
    if (it->second.next >= it->second.entries.size()) return kEnd;
    entry = it->second.entries[it->second.next++];
    return kOk;
}

Status close_dir(int32_t handle) {
    std::lock_guard lock(g_mutex);
    return g_dirs.erase(handle) ? kOk : kFatal;
}

} // namespace cafe::vfs
