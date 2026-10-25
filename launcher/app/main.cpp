// ttt2-launcher [GAME.wua] [--screenshot FILE]: the players' launcher (see
// launcher.h). One window per user; starting it again shows that window
// (with the archive given, chosen in it). ttt2-launcher --version: its
// version, which an update checks the new launcher by (self_update.cpp).

#include "launcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

struct App {
    launcher::Options options;
    std::unique_ptr<launcher::Launcher> window;
};

void activate(GtkApplication* app, gpointer data) {
    auto* a = static_cast<App*>(data);
    if (!a->window) a->window = std::make_unique<launcher::Launcher>(app, a->options);
    a->window->present();
}

void open(GtkApplication* app, GFile** files, int count, const char*, gpointer data) {
    auto* a = static_cast<App*>(data);
    activate(app, data);
    if (count > 0) {
        if (char* path = g_file_get_path(files[0])) {
            a->window->open_game(path);
            g_free(path);
        }
    }
}

void usage() {
    std::printf("usage: ttt2-launcher [GAME.wua] [--screenshot FILE] | --version\n"
                "  Installs Tekken Tag Tournament 2 from your own .wua (the game program is built on this PC),\n"
                "  starts it, and adds Tekken 7's characters from your own Tekken 7.\n"
                "  --screenshot FILE  saves the window as a PNG once it has loaded, then quits (for tests)\n"
                "  --version          prints its version (\"ttt2-launcher " TTT2_LAUNCHER_VERSION "\")\n");
}

} // namespace

int main(int argc, char** argv) {
    App a;
    std::vector<char*> args = {argv[0]};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            a.options.screenshot = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("ttt2-launcher %s\n", TTT2_LAUNCHER_VERSION);
            return 0;
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "ttt2-launcher: unknown option %s\n", argv[i]);
            usage();
            return 2;
        } else {
            a.options.game = launcher::normal(argv[i]);
            args.push_back(argv[i]); // (to the running launcher, when there is one)
        }
    }
    // Test runs are launchers of their own, beside the player's; one with
    // actions it cannot do stops at once.
    const char* actions = std::getenv("TTT2_LAUNCHER_TEST_ACTIONS");
    const bool test = !a.options.screenshot.empty() || (actions && *actions);
    if (std::string error; actions && *actions && !launcher::parse_test_actions(actions, &error)) {
        std::fprintf(stderr, "@test actions failed %s\n", error.c_str());
        return 2;
    }
    // Opened by an update of the launcher: once the one it replaces has
    // ended (it closes at once; 10 s at most), whose window would be shown
    // instead of this one's.
    if (const char* replaces = std::getenv(launcher::kReplacesVariable)) {
        const int pid = std::atoi(replaces);
        g_unsetenv(launcher::kReplacesVariable); // (not for what it starts)
        for (int i = 0; i < 100 && launcher::process_alive(pid); ++i) g_usleep(100'000);
    }
    // The program's name is the window's class on X11, which its menu entry
    // names (StartupWMClass) so that docks match the two; GApplication would
    // take argv[0] ("ttt2-launcher") otherwise.
    g_set_prgname(TTT2_LAUNCHER_APP_ID);
    GtkApplication* app = gtk_application_new(
        TTT2_LAUNCHER_APP_ID, GApplicationFlags(G_APPLICATION_HANDLES_OPEN | (test ? G_APPLICATION_NON_UNIQUE : 0)));
    g_set_application_name(launcher::kName);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &a);
    g_signal_connect(app, "open", G_CALLBACK(open), &a);
    const int status = g_application_run(G_APPLICATION(app), static_cast<int>(args.size()), args.data());
    a.window.reset();
    g_object_unref(app);
    return status;
}
