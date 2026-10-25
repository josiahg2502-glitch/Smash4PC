// ttt2-build's checks of the PC that do not need a game: what the hints say
// on which distribution (image-based ones and those without /usr/lib too),
// and where libraries are found (in a folder of its own, made and deleted
// here: argv[1]).

#include "children.h"
#include "host.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace build;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void test_systems() {
    System s = parse_system("NAME=\"Example Linux\"\nID=example\nID_LIKE=arch\n", false, true);
    check(s.family == Family::arch && !s.image_based, "os-release: like arch");
    s = parse_system("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n", false, true);
    check(s.family == Family::debian && !s.image_based, "os-release: like ubuntu");
    s = parse_system("ID='opensuse-tumbleweed'\nID_LIKE=\"opensuse suse\"\n", false, true);
    check(s.family == Family::suse && !s.image_based, "os-release: tumbleweed");
    check(parse_system("ID=plan9\n", false, true).family == Family::other, "os-release: unknown");

    // Image-based: by their ids, variants and names, and by rpm-ostree's mark.
    s = parse_system("NAME=\"SteamOS\"\nID=steamos\nID_LIKE=arch\nVARIANT_ID=steamdeck\n", false, true);
    check(s.family == Family::arch && s.image_based, "os-release: steamos");
    check(parse_system("ID=bazzite\nID_LIKE=\"fedora\"\nVARIANT_ID=bazzite-deck\n", false, true).image_based,
          "os-release: bazzite");
    check(parse_system("ID=fedora\nVARIANT_ID=silverblue\n", false, true).image_based, "os-release: silverblue");
    check(parse_system("ID=fedora\nVARIANT_ID=kinoite\n", false, true).image_based, "os-release: kinoite");
    check(!parse_system("ID=fedora\nVARIANT_ID=workstation\n", false, true).image_based, "os-release: workstation");
    check(parse_system("ID=fedora\nVARIANT_ID=workstation\n", true, true).image_based, "os-release: ostree-booted");
    check(parse_system("ID=\"opensuse-microos\"\nID_LIKE=\"suse opensuse opensuse-tumbleweed microos\"\n", false, true)
              .image_based,
          "os-release: microos");
    check(parse_system("ID=opensuse-aeon\nID_LIKE=\"suse opensuse\"\n", false, true).image_based, "os-release: aeon");
    check(parse_system("ID=nixos\n", false, false).image_based, "os-release: nixos");
    check(parse_system("ID=guix\n", false, false).image_based, "os-release: guix");
    check(!parse_system("ID=ubuntu\nVARIANT_ID=desktop\n", false, true).image_based, "os-release: ubuntu desktop");
}

void test_texts() {
    const System arch = parse_system("ID=arch\n", false, true);
    std::string text = missing_libraries_text({"libSDL3.so.0"}, arch);
    check(text == "SDL 3 (libSDL3.so.0) is not installed: install it with: sudo pacman -S --needed sdl3",
          "missing: one, arch: " + text);
    text = missing_libraries_text({"libSDL3.so.0", "libshaderc_shared.so.1"}, parse_system("ID=ubuntu\n", false, true));
    check(text == "SDL 3 (libSDL3.so.0) and shaderc (libshaderc_shared.so.1) are not installed: install them with: "
                  "sudo apt install libsdl3-0 libshaderc1",
          "missing: two, debian: " + text);
    text = missing_libraries_text({"libz.so.1"}, parse_system("ID=plan9\n", false, true));
    check(has(text, "install it with your distribution's package manager"), "missing: unknown distribution: " + text);
    text = missing_libraries_text({"libfoo.so.7"}, arch);
    check(text == "libfoo.so.7 is not installed: install the library that provides it", "missing: unknown: " + text);

    // On an image-based system no command, but where it can come from.
    const System deck = parse_system("ID=steamos\nID_LIKE=arch\n", false, true);
    text = missing_libraries_text({"libSDL3.so.0", "libshaderc_shared.so.1"}, deck);
    check(text == "SDL 3 (libSDL3.so.0) and shaderc (libshaderc_shared.so.1) are not installed. This system's own files "
                  "cannot be changed with a package manager: SDL 3 and shaderc must come from a toolbox or distrobox "
                  "container, or from the system's own way of adding software.",
          "missing: steamos: " + text);
    check(!has(text, "sudo"), "missing: steamos, no sudo");
    text = missing_compiler_text(deck);
    check(has(text, "No C++ compiler") && has(text, "a C++ compiler must come from a toolbox or distrobox container") &&
              !has(text, "pacman"),
          "compiler: steamos: " + text);
    text = missing_compiler_text(parse_system("ID=fedora\n", false, true));
    check(has(text, "It needs Clang (or GCC 15 or newer): install it with: sudo dnf install clang gcc-c++"),
          "compiler: fedora: " + text);

    // Without /usr/lib (NixOS): not found where it was looked for, not "not installed".
    text = missing_libraries_text({"libSDL3.so.0"}, parse_system("ID=nixos\n", false, false));
    check(has(text, "SDL 3 (libSDL3.so.0) was not found") && has(text, "LD_LIBRARY_PATH") &&
              has(text, "toolbox or distrobox") && !has(text, "not installed"),
          "missing: nixos: " + text);
    text = missing_libraries_text({"libSDL3.so.0"}, parse_system("ID=gobolinux\n", false, false));
    check(has(text, "was not found") && has(text, "LD_LIBRARY_PATH") && has(text, "package manager"),
          "missing: other without /usr/lib: " + text);
}

void test_libraries(const fs::path& scratch) {
    std::error_code ec;
    fs::remove_all(scratch, ec);
    const fs::path folder = scratch / "lib", other = scratch / "other";
    fs::create_directories(folder, ec);
    fs::create_directories(other, ec);
    // An x86-64 file by a soname no system has, and one that is no library.
    fs::copy_file("/proc/self/exe", folder / "libttt2-host-test.so.9", ec);
    std::ofstream(other / "libttt2-host-test.so.9") << "not a library";
    check(!find_library("libttt2-host-test.so.9", {}), "library: not in the usual folders");
    std::optional<Library> found = find_library("libttt2-host-test.so.9", {other, folder});
    check(found && found->file == folder / "libttt2-host-test.so.9" && !found->cached, "library: in a folder given");

    // The folders: LD_LIBRARY_PATH's, LIBRARY_PATH's, then the compiler's.
    ::setenv("LD_LIBRARY_PATH", (folder.string() + "::relative:" + folder.string()).c_str(), 1);
    ::setenv("LIBRARY_PATH", other.c_str(), 1);
    const fs::path compiler = scratch / "compiler";
    std::ofstream(compiler) << "#!/bin/sh\necho 'install: /x/'\necho 'libraries: =/opt/a/lib/:/opt/b/../lib'\n";
    fs::permissions(compiler, fs::perms::owner_all, ec);
    const std::vector<fs::path> folders = library_folders({compiler.string()});
    check(folders.size() == 4 && folders[0] == folder && folders[1] == other && folders[2] == "/opt/a/lib" &&
              folders[3] == "/opt/lib",
          "library folders");
    found = find_library("libttt2-host-test.so.9", folders);
    check(found && found->file == folder / "libttt2-host-test.so.9", "library: through LD_LIBRARY_PATH");
    ::unsetenv("LD_LIBRARY_PATH");
    ::unsetenv("LIBRARY_PATH");
    check(library_folders({"/bin/false"}).empty(), "library folders: a compiler that says nothing");

    check(same_file_system(folder, other / "not yet/made"), "file system: the same");
    check(!same_file_system(folder, "/proc/self"), "file system: another");
    fs::remove_all(scratch, ec);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s SCRATCH_FOLDER\n", argv[0]);
        return 2;
    }
    watch_signals(); // (as ttt2-build does before it runs anything)
    test_systems();
    test_texts();
    test_libraries(argv[1]);
    std::printf(g_failures ? "%d failed\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
