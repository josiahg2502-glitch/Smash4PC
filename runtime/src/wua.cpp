#include "wua.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace cafe::wua {
namespace {

// A title id's upper half says what the title is.
constexpr uint32_t kGame = 0x00050000;
constexpr uint32_t kUpdate = 0x0005000E;

struct Title {
    uint64_t id = 0;
    uint32_t version = 0;
};

// "<16 hex digits>_v<version>", as Cemu names a title's folder.
std::optional<Title> parse_title(std::string_view name) {
    if (name.size() < 19 || name[16] != '_' || (name[17] != 'v' && name[17] != 'V')) return std::nullopt;
    Title title;
    const char* end = name.data() + name.size();
    const auto id = std::from_chars(name.data(), name.data() + 16, title.id, 16);
    const auto version = std::from_chars(name.data() + 18, end, title.version);
    if (id.ec != std::errc() || id.ptr != name.data() + 16 || version.ec != std::errc() || version.ptr != end) {
        return std::nullopt;
    }
    return title;
}

bool layered(const Title& title) {
    const uint32_t kind = static_cast<uint32_t>(title.id >> 32);
    return kind == kGame || kind == kUpdate;
}

// As the archive compares names: ASCII letters only.
std::string lower(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

ZArchiveNodeHandle find_layer(ZArchiveReader& archive, const std::vector<std::string>& layers, std::string_view path,
                              size_t& layer) {
    for (layer = 0; layer < layers.size(); ++layer) {
        const ZArchiveNodeHandle node = archive.LookUp(layers[layer] + "/" + std::string(path));
        if (node != ZARCHIVE_INVALID_NODE) return node;
    }
    return ZARCHIVE_INVALID_NODE;
}

void check_name(std::string_view name) {
    const bool unsafe = name.empty() || name == "." || name == ".." ||
                        std::any_of(name.begin(), name.end(), [](char c) {
                            return c == '/' || c == '\\' || static_cast<unsigned char>(c) < 32;
                        });
    if (unsafe) throw std::runtime_error("the game holds a file with an unsafe name: " + std::string(name));
}

void walk(ZArchiveReader& archive, const std::vector<std::string>& layers, const std::string& folder, int depth,
          std::vector<File>& files) {
    std::vector<Entry> entries;
    if (depth > 64 || !list(archive, layers, folder, entries)) throw std::runtime_error("the archive's folders are damaged");
    for (const Entry& e : entries) {
        check_name(e.name);
        const std::string path = folder.empty() ? e.name : folder + "/" + e.name;
        if (e.directory) {
            walk(archive, layers, path, depth + 1, files);
            continue;
        }
        File file{path, e.size};
        file.node = find_layer(archive, layers, path, file.layer);
        if (file.node == ZARCHIVE_INVALID_NODE) throw std::runtime_error("the archive's folders are damaged");
        files.push_back(std::move(file));
    }
}

} // namespace

std::vector<std::string> game_layers(ZArchiveReader& archive) {
    struct Folder {
        std::string name;
        std::optional<Title> title;
    };
    std::vector<Folder> folders;
    const ZArchiveNodeHandle root = archive.LookUp("", false, true);
    ZArchiveReader::DirEntry entry;
    for (uint32_t i = 0; i < archive.GetDirEntryCount(root); ++i) {
        if (archive.GetDirEntry(root, i, entry) && entry.isDirectory) {
            folders.push_back({std::string(entry.name), parse_title(entry.name)});
        }
    }
    const auto game = std::find_if(folders.begin(), folders.end(), [&](const Folder& f) {
        return archive.LookUp(f.name + "/code/Tekken.rpx") != ZARCHIVE_INVALID_NODE;
    });
    if (game == folders.end()) return {};
    // A folder not named as the game or its update holds all of it.
    if (!game->title || !layered(*game->title)) return {game->name};
    // The base game and its updates share the lower half of the title id.
    const uint32_t unique = static_cast<uint32_t>(game->title->id);
    std::vector<const Folder*> parts;
    for (const Folder& f : folders) {
        if (f.title && layered(*f.title) && static_cast<uint32_t>(f.title->id) == unique) parts.push_back(&f);
    }
    std::stable_sort(parts.begin(), parts.end(), [](const Folder* a, const Folder* b) {
        const bool a_update = (a->title->id >> 32) == kUpdate, b_update = (b->title->id >> 32) == kUpdate;
        if (a_update != b_update) return a_update;
        return a->title->version > b->title->version;
    });
    std::vector<std::string> layers;
    for (const Folder* f : parts) layers.push_back(f->name);
    return layers;
}

ZArchiveNodeHandle find(ZArchiveReader& archive, const std::vector<std::string>& layers, std::string_view path) {
    size_t layer;
    return find_layer(archive, layers, path, layer);
}

bool list(ZArchiveReader& archive, const std::vector<std::string>& layers, std::string_view path,
          std::vector<Entry>& entries) {
    bool complete = true, merged = false;
    std::unordered_set<std::string> names;
    for (const std::string& layer : layers) {
        const ZArchiveNodeHandle folder = archive.LookUp(layer + "/" + std::string(path));
        if (!archive.IsDirectory(folder)) continue;
        merged = !names.empty();
        ZArchiveReader::DirEntry entry;
        for (uint32_t i = 0; i < archive.GetDirEntryCount(folder); ++i) {
            if (!archive.GetDirEntry(folder, i, entry)) {
                complete = false;
                continue;
            }
            if (names.insert(lower(entry.name)).second) {
                entries.push_back({std::string(entry.name), entry.isDirectory, entry.isFile ? entry.size : 0});
            }
        }
    }
    // In the order the archive keeps each folder, so that the files of each
    // layer come in that layer's order.
    if (merged) {
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); });
    }
    return complete;
}

std::vector<File> game_files(ZArchiveReader& archive, const std::vector<std::string>& layers) {
    std::vector<File> files;
    walk(archive, layers, "", 0, files);
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.layer < b.layer; });
    return files;
}

} // namespace cafe::wua
