// The game in a .wua archive, with the update merged into the game's folder
// or beside it (as Cemu stores them): every file must come from the right
// title folder, the update's over the base game's.

#include "wua.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <zarchive/zarchivewriter.h>
#pragma GCC diagnostic pop

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void new_part(int32_t, void*) {}
void write_data(const void* data, size_t length, void* out) {
    static_cast<std::ofstream*>(out)->write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
}

std::unique_ptr<ZArchiveReader> make_archive(const std::string& name, const std::map<std::string, std::string>& files) {
    {
        std::ofstream out(name, std::ios::binary | std::ios::trunc);
        ZArchiveWriter writer(new_part, write_data, &out);
        for (const auto& [path, data] : files) {
            if (const size_t slash = path.rfind('/'); slash != std::string::npos) {
                writer.MakeDir(path.substr(0, slash).c_str(), true);
            }
            writer.StartNewFile(path.c_str());
            writer.AppendData(data.data(), data.size());
        }
        writer.Finalize();
    }
    return std::unique_ptr<ZArchiveReader>(ZArchiveReader::OpenFromFile(name));
}

std::string contents(ZArchiveReader& archive, ZArchiveNodeHandle node) {
    if (node == ZARCHIVE_INVALID_NODE || !archive.IsFile(node)) return "<none>";
    std::string data(archive.GetFileSize(node), '\0');
    archive.ReadFromFile(node, 0, data.size(), data.data());
    return data;
}

std::string read(ZArchiveReader& archive, const std::vector<std::string>& layers, const std::string& path) {
    return contents(archive, cafe::wua::find(archive, layers, path));
}

std::set<std::string> names(ZArchiveReader& archive, const std::vector<std::string>& layers, const std::string& path) {
    std::vector<cafe::wua::Entry> entries;
    check(cafe::wua::list(archive, layers, path, entries), "list " + path);
    std::set<std::string> out;
    for (const auto& e : entries) out.insert(e.name);
    check(out.size() == entries.size(), "no duplicate names in " + path);
    return out;
}

void merged() {
    auto archive = make_archive("wua_layers_merged.wua", {
        {"000500001010f800_v16/code/Tekken.rpx", "rpx16"},
        {"000500001010f800_v16/content/a.bin", "A16"},
        {"000500001010f800_v16/meta/meta.xml", "m16"},
    });
    check(archive != nullptr, "merged: archive opens");
    if (!archive) return;
    const auto layers = cafe::wua::game_layers(*archive);
    check(layers == std::vector<std::string>{"000500001010f800_v16"}, "merged: one layer");
    check(read(*archive, layers, "code/Tekken.rpx") == "rpx16", "merged: executable");
    check(read(*archive, layers, "CONTENT/A.BIN") == "A16", "merged: any case");
    check(cafe::wua::game_files(*archive, layers).size() == 3, "merged: three files");
}

void split() {
    auto archive = make_archive("wua_layers_split.wua", {
        {"000500001010f800_v0/code/Tekken.rpx", "rpx0"},
        {"000500001010f800_v0/content/a.bin", "A0"},
        {"000500001010f800_v0/content/only_base.bin", "B"},
        {"000500001010f800_v0/content/Sub/deep.bin", "D"},
        {"000500001010f800_v0/meta/meta.xml", "m0"},
        {"0005000e1010f800_v16/code/Tekken.rpx", "rpx16"},
        {"0005000e1010f800_v16/content/A.bin", "A16"},
        {"0005000e1010f800_v16/content/sub/new.bin", "N"},
        {"0005000e1010f800_v16/meta/meta.xml", "m16"},
        {"0005000c1010f800_v1/content/dlc.bin", "X"},
        {"0005000010101000_v0/code/Other.rpx", "other"},
    });
    check(archive != nullptr, "split: archive opens");
    if (!archive) return;
    const auto layers = cafe::wua::game_layers(*archive);
    check(layers == std::vector<std::string>{"0005000e1010f800_v16", "000500001010f800_v0"}, "split: update over base");
    check(read(*archive, layers, "code/Tekken.rpx") == "rpx16", "split: the update's executable");
    check(read(*archive, layers, "meta/meta.xml") == "m16", "split: the update's metadata");
    check(read(*archive, layers, "content/a.bin") == "A16", "split: an updated file, other case");
    check(read(*archive, layers, "content/only_base.bin") == "B", "split: a file only the base has");
    check(read(*archive, layers, "content/sub/deep.bin") == "D", "split: through a folder both have");
    check(read(*archive, layers, "content/sub/new.bin") == "N", "split: a file only the update has");
    check(read(*archive, layers, "content/dlc.bin") == "<none>", "split: no DLC");
    check(names(*archive, layers, "content").size() == 3, "split: content lists a.bin, only_base.bin and one sub");
    check(names(*archive, layers, "content/sub") == std::set<std::string>{"deep.bin", "new.bin"}, "split: sub's union");

    std::map<std::string, std::string> files;
    for (const auto& f : cafe::wua::game_files(*archive, layers)) {
        check(f.size == archive->GetFileSize(f.node), "split: size of " + f.path);
        check(files.emplace(f.path, contents(*archive, f.node)).second, "split: once: " + f.path);
    }
    const std::map<std::string, std::string> expected{
        {"code/Tekken.rpx", "rpx16"}, {"content/A.bin", "A16"}, {"content/sub/new.bin", "N"},
        {"meta/meta.xml", "m16"},     {"content/only_base.bin", "B"}, {"content/sub/deep.bin", "D"},
    };
    check(files == expected, "split: the merged file list");
}

void other_name() {
    auto archive = make_archive("wua_layers_named.wua", {
        {"TTT2/code/Tekken.rpx", "rpx"},
        {"TTT2/content/a.bin", "A"},
    });
    check(archive != nullptr, "named: archive opens");
    if (!archive) return;
    const auto layers = cafe::wua::game_layers(*archive);
    check(layers == std::vector<std::string>{"TTT2"}, "named: a folder not named as a title holds all");
    check(read(*archive, layers, "content/a.bin") == "A", "named: its file");
}

} // namespace

int main() {
    merged();
    split();
    other_name();
    std::printf("wua layers: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
