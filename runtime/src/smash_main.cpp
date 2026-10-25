// Smash 4 PC: Super Smash Bros. for Wii U on the cafe runtime. Like main.cpp (Tekken),
// without Tekken's install, presentation and roster code.

#include "cafe/export.h"
#include "cafe/generated.h"
#include "cafe/layout.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"
#include "cafe/vfs.h"

#include "gpu/gpu.h"
#include "host/audio.h"
#include "host/display_settings.h"
#include "host/window.h"
#include "os/act_server.h"
#include "os/online_identity.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace cafe::os {
void init_heaps(uint32_t mem2_begin);
int run_main_thread(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size,
                    uint32_t sda_base, uint32_t sda2_base);
void set_preinit_user(uint32_t function);
void set_font_folder(const std::filesystem::path& folder);
void register_aoc(uint64_t id, uint32_t group, uint16_t version, const std::filesystem::path& content);
bool test_friends_login(); // os/friends.cpp
} // namespace cafe::os

namespace smash {
void register_title_logo(const std::filesystem::path& png);
void start_overlay(const std::filesystem::path& dir);
} // namespace smash

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--mod FOLDER]... [--dlc FOLDER] [UPDATE] BASE\n"
                 "  Each --mod FOLDER holds content/ (and maybe code/) laid over the game's;\n"
                 "  the first mod given wins.\n"
                 "  UPDATE and BASE are the title folders (code/, content/, meta/) of\n"
                 "  update 0005000E10144F00 and game 0005000010144F00; files in UPDATE\n"
                 "  win. The program must be the one built from the code it runs: the\n"
                 "  update's (smash.exe) or the game's own (smash-base.exe, BASE only).\n"
                 "  Saves go to $SMASH_SAVE_DIR, default save\\ next to this\n"
                 "  executable. The system fonts are fonts\\CafeStd.ttf (CafeCn, CafeKr,\n"
                 "  CafeTw) next to this executable, else Segoe UI. TTT2_GPU=null runs\n"
                 "  without a window.\n",
                 argv0);
}

// Objects the game imports as data are created by the library that owns
// them; anything the runtime does not provide yet stays zeroed.
void init_data_imports() {
    for (size_t i = 0; i < cafe_guest_import_count; ++i) {
        const cafe::GuestImport& import = cafe_guest_imports[i];
        if (!import.is_data) continue;
        if (const cafe::DataExport* data = cafe::find_data_export(import.module, import.name)) {
            data->initialize(import.address);
        } else {
            std::fprintf(stderr, "smash: warning: data import %s:%s is not provided\n", import.module,
                         import.name);
        }
    }
}

// The DLC title in `folder`: its id, version and group from code/app.xml (hex values), its items in content/.
void register_dlc(const std::filesystem::path& folder) {
    std::ifstream in(folder / "code" / "app.xml", std::ios::binary);
    const std::string xml((std::istreambuf_iterator<char>(in)), {});
    const auto tag = [&](const char* name) -> uint64_t {
        const size_t open = xml.find(std::string("<") + name);
        const size_t start = open == std::string::npos ? open : xml.find('>', open);
        return start == std::string::npos ? 0 : std::strtoull(xml.c_str() + start + 1, nullptr, 16);
    };
    const uint64_t id = tag("title_id");
    if ((id >> 32) != 0x0005000Cu) {
        std::fprintf(stderr, "smash: %s is not a DLC title (code/app.xml), ignored\n", folder.string().c_str());
        return;
    }
    cafe::os::register_aoc(id, static_cast<uint32_t>(tag("group_id")), static_cast<uint16_t>(tag("title_version")),
                           folder / "content");
    std::fprintf(stderr, "smash: DLC %016llX v%llu from %s\n", static_cast<unsigned long long>(id),
                 static_cast<unsigned long long>(tag("title_version")), folder.string().c_str());
}

uint32_t guest_strdup(const char* text) {
    const uint32_t address = cafe::system_alloc(static_cast<uint32_t>(std::strlen(text) + 1));
    std::memcpy(cafe::guest_pointer(address), text, std::strlen(text) + 1);
    return address;
}

} // namespace

int main(int argc, char** argv) {
    using namespace cafe;
    // --mod FOLDER (repeatable, the first wins): a folder holding content/ laid over the game's.
    // --dlc FOLDER: the DLC title (code/app.xml, content/<item>/), reported to the game through nn_aoc.
    std::vector<std::filesystem::path> layers;
    while (argc >= 3 && (std::strcmp(argv[1], "--mod") == 0 || std::strcmp(argv[1], "--dlc") == 0)) {
        if (std::strcmp(argv[1], "--mod") == 0) {
            layers.emplace_back(argv[2]);
            std::fprintf(stderr, "smash: mod %s\n", argv[2]);
        } else {
            register_dlc(argv[2]);
        }
        argv[2] = argv[0];
        argv += 2;
        argc -= 2;
    }
    if (argc != 2 && argc != 3) {
        usage(argv[0]);
        return 2;
    }
    // SMASH_LOG (the launcher sets it): the messages go to that file; with nowhere else for them
    // (started from Explorer or a shortcut), to smash.log beside this executable.
    std::string log_path;
    if (const char* log = std::getenv("SMASH_LOG"); log && *log) log_path = log;
#ifdef _WIN32
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (log_path.empty() && (err == nullptr || err == INVALID_HANDLE_VALUE || GetFileType(err) == FILE_TYPE_UNKNOWN)) {
        log_path = (std::filesystem::absolute(argv[0]).parent_path() / "smash.log").string();
    }
#endif
    if (!log_path.empty() && std::freopen(log_path.c_str(), "w", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    // 1 ms timer resolution: Windows rounds sleeps and timed waits up to
    // 15.6 ms otherwise, which slows the title's loading threads and the
    // GPU command processor's polling.
    timeBeginPeriod(1);
#endif
    install_fault_handler();
    layers.insert(layers.end(), argv + 1, argv + argc);
    vfs::open_game(layers);
    if (const char* save = std::getenv("SMASH_SAVE_DIR")) {
        vfs::set_save_root(save);
    } else {
        vfs::set_save_root(std::filesystem::absolute(argv[0]).parent_path() / "save");
    }
    const auto rpx = vfs::read_whole("/vol/code/cross_f.rpx");
    if (!rpx) fatal("the game folders have no code/cross_f.rpx");
    reserve_guest_memory();
    init_system_heap();
    const LoadedImage image = load_image(*rpx);
    build_dispatch_table();
    os::init_heaps(image.data_end);
    init_data_imports();
    os::set_preinit_user(image.preinit_user);
    os::set_font_folder(std::filesystem::absolute(argv[0]).parent_path() / "fonts");
    gpu::set_cache_directory(std::filesystem::absolute(argv[0]).parent_path() / "cache");
    smash::register_title_logo(std::filesystem::absolute(argv[0]).parent_path() / "title-logo.png");
    host::display::set_file(std::filesystem::absolute(argv[0]).parent_path() / "display.ini", "Smash 4 PC");
    // Nintendo Network (Pretendo): the console's files and account in online/ beside this executable.
    os::online::load(std::filesystem::absolute(argv[0]).parent_path() / "online");
    if (std::getenv("SMASH_ONLINE_TEST")) { // sign in to the friends server, then quit
        const bool ok = os::test_friends_login();
        std::fprintf(stderr, "smash: online test %s\n", ok ? "passed (friends server online)" : "failed");
        std::_Exit(ok ? 0 : 1);
    }
    std::fprintf(stderr, "smash: loaded image, entry 0x%08X, %zu/%zu guest entries compiled\n",
                 image.entry_point, cafe_program_info.compiled_entries, cafe_program_info.total_entries);

    // argv[0] is the executable name, as the Cafe loader passes it.
    const uint32_t guest_argv = system_alloc(8);
    *reinterpret_cast<uint32_t*>(guest_pointer(guest_argv)) = __builtin_bswap32(guest_strdup("cross_f.rpx"));
    const auto run = [&] {
        return os::run_main_thread(image.entry_point, 1, guest_argv, image.stack_size, image.sda_base,
                                   image.sda2_base);
    };
    // The window's event loop needs the process's main thread; the title
    // runs beside it.
    const char* gpu = std::getenv("TTT2_GPU");
    const bool headless =
        (gpu && std::strcmp(gpu, "null") == 0) || !host::open_window("Smash 4 PC");
    host::open_audio();
    if (headless) return run();
    smash::start_overlay(std::filesystem::absolute(argv[0]).parent_path());
    std::atomic<bool> finished{false};
    int result = 0;
    std::thread title([&] {
        result = run();
        finished = true;
    });
    if (!host::run_event_loop([&] { return finished.load(); })) std::_Exit(0); // window closed
    title.join();
    return result;
}
