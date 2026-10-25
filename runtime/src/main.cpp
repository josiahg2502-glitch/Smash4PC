#include "cafe/export.h"
#include "cafe/generated.h"
#include "cafe/layout.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"
#include "cafe/vfs.h"

#include "host/audio.h"
#include "host/window.h"
#include "install.h"
#include "title/presentation.h"
#include "title/roster.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace cafe::os {
void init_heaps(uint32_t mem2_begin);
int run_main_thread(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size,
                    uint32_t sda_base, uint32_t sda2_base);
} // namespace cafe::os

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [GAME]\n"
                 "       %s --install [GAME [FOLDER]] [--no-launcher]\n"
                 "       %s --update [FOLDER]\n"
                 "  GAME is the title's .wua archive, or a folder with code/,\n"
                 "  content/ and meta/; without it the installed game runs.\n"
                 "  --install copies the game, this executable and its icon into\n"
                 "  FOLDER (chosen in a dialog when not given) and adds it to the\n"
                 "  application menu; --update copies this executable into the\n"
                 "  install after a rebuild. Saves go to $TTT2_SAVE_DIR, default\n"
                 "  ~/.local/share/ttt2/save. Fullscreen, the resolution and 120 fps\n"
                 "  come from $XDG_CONFIG_HOME/ttt2/display.ini (default\n"
                 "  ~/.config/ttt2/display.ini; the launcher's Settings write it),\n"
                 "  read when the game starts. TTT2_FULLSCREEN, TTT2_SCALE and\n"
                 "  TTT2_FPS win over it.\n",
                 argv0, argv0, argv0);
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
            std::fprintf(stderr, "ttt2: warning: data import %s:%s is not provided\n", import.module,
                         import.name);
        }
    }
}

uint32_t guest_strdup(const char* text) {
    const uint32_t address = cafe::system_alloc(static_cast<uint32_t>(std::strlen(text) + 1));
    std::memcpy(cafe::guest_pointer(address), text, std::strlen(text) + 1);
    return address;
}

} // namespace

int main(int argc, char** argv) {
    using namespace cafe;
    if (argc >= 2 && (std::strcmp(argv[1], "--install") == 0 || std::strcmp(argv[1], "--update") == 0)) {
        return install::run(std::vector<std::string>(argv + 1, argv + argc));
    }
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        usage(argv[0]);
        return 0;
    }
    std::filesystem::path game;
    if (argc == 2) {
        game = argv[1];
    } else if (const auto installed = install::installed_game(); argc == 1 && installed) {
        game = *installed;
    } else {
        usage(argv[0]);
        return 2;
    }
    install_fault_handler();
    vfs::open_game(game);
    if (const char* save = std::getenv("TTT2_SAVE_DIR")) {
        vfs::set_save_root(save);
    } else {
        const char* home = std::getenv("HOME");
        vfs::set_save_root(std::filesystem::path(home ? home : ".") / ".local/share/ttt2/save");
    }
    const auto rpx = vfs::read_whole("/vol/code/Tekken.rpx");
    if (!rpx) fatal("the game source has no code/Tekken.rpx");
    reserve_guest_memory();
    init_system_heap();
    const LoadedImage image = load_image(*rpx);
    build_dispatch_table();
    os::init_heaps(image.data_end);
    init_data_imports();
    title::prepare_presentation();
    title::prepare_roster();
    std::fprintf(stderr, "ttt2: loaded image, entry 0x%08X, %zu/%zu guest entries compiled\n",
                 image.entry_point, cafe_program_info.compiled_entries,
                 cafe_program_info.total_entries);

    // argv[0] is the executable name, as the Cafe loader passes it.
    const uint32_t guest_argv = system_alloc(8);
    *reinterpret_cast<uint32_t*>(guest_pointer(guest_argv)) = __builtin_bswap32(guest_strdup("Tekken.rpx"));
    const auto run = [&] {
        return os::run_main_thread(image.entry_point, 1, guest_argv, image.stack_size, image.sda_base,
                                   image.sda2_base);
    };
    // The window's event loop needs the process's main thread; the title
    // runs beside it. TTT2_GPU=null runs headless.
    const char* gpu = std::getenv("TTT2_GPU");
    const bool headless = (gpu && std::strcmp(gpu, "null") == 0) || !host::open_window("Tekken Tag Tournament 2");
    if (!headless) {
        std::vector<uint8_t> icon;
        uint32_t width = 0, height = 0;
        if (const auto tga = vfs::read_whole("/vol/meta/iconTex.tga"); tga && install::decode_tga(*tga, icon, width, height)) {
            host::set_window_icon(icon, width, height);
        }
    }
    host::open_audio();
    if (headless) return run();
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
