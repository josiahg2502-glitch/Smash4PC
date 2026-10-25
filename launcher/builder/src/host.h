#pragma once

// What building the game program needs from this computer: an x86-64-v3
// processor, a C++ compiler, the shared libraries the program links, memory
// and free space; and how to tell the player to install what is missing,
// for their distribution.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace build {

// The x86-64-v3 features this processor lacks ("AVX2", "FMA"), as
// /proc/cpuinfo lists them; empty when it has all. The generated code is
// compiled for x86-64-v3.
std::vector<std::string> missing_processor_features();

unsigned processors(); // the ones this process may run on
uint64_t memory_available(); // MemAvailable, in bytes
uint64_t free_space(std::filesystem::path folder); // of the nearest existing folder
// Whether the two folders (or their nearest existing parents) are on one
// file system, so that what is in one takes the other's free space too.
bool same_file_system(const std::filesystem::path& a, const std::filesystem::path& b);

// The programs to try as the compiler when none is named: clang++, then
// versioned clang++-N (newest first), then g++ and g++-N, as found in PATH.
// Each by the name it is called by, even where several are links to one
// program: ccache's and distcc's folders hold links that run the compiler
// they are named after.
std::vector<std::string> compiler_candidates();
// A program in PATH (or a path), as an absolute path.
std::optional<std::filesystem::path> find_program(const std::string& name);

// A shared library by its soname ("libSDL3.so.0"), x86-64, as the dynamic
// loader finds it (`cached`: in its cache), else in `folders`, else in the
// usual folders. The game program finds one that is not in the cache only
// through a run path to its folder.
struct Library {
    std::filesystem::path file;
    bool cached = false;
};
std::optional<Library> find_library(const std::string& soname, const std::vector<std::filesystem::path>& folders);
// Where else `compiler` (the program and the arguments it is given with)
// finds libraries: LD_LIBRARY_PATH's folders, LIBRARY_PATH's and its own
// library path (-print-search-dirs), for systems whose libraries are not in
// the loader's cache (NixOS, Guix, or one built by hand).
std::vector<std::filesystem::path> library_folders(const std::vector<std::string>& compiler);

// The distribution, as far as the hints need it.
enum class Family { other, arch, debian, fedora, suse };
struct System {
    Family family = Family::other;
    // An image-based system (SteamOS, Fedora Atomic, openSUSE MicroOS, NixOS
    // ...): its own files cannot be changed with its package manager.
    bool image_based = false;
    // Its libraries are in /usr/lib and the like (not on NixOS or Guix).
    bool usual_folders = true;
};
// From os-release's text, whether /run/ostree-booted exists and whether the
// C library is in the loader's cache or a usual folder.
System parse_system(std::string_view os_release, bool ostree_booted, bool usual_folders);
const System& this_system();

// "SDL 3 (libSDL3.so.0) is not installed: install it with:
// sudo pacman -S --needed sdl3", with this distribution's command, for all
// of them at once.
std::string missing_libraries_text(const std::vector<std::string>& sonames, const System& system = this_system());
// The same for a C++ compiler.
std::string missing_compiler_text(const System& system = this_system());

} // namespace build
