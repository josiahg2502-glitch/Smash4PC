#pragma once

// What installing needs of the player's PC, checked before the builder
// starts so that a missing piece is named with the command that installs it:
// the processor, a C++ compiler, free space, and the distribution's package
// manager for the hints.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

namespace fs = std::filesystem;

// The processor features the game program is compiled for (x86-64-v3:
// AVX2, FMA, BMI1/2, MOVBE, LZCNT ...) that /proc/cpuinfo's flags lack, by
// their usual names. All of them when there are no x86 flags at all.
std::vector<std::string> missing_cpu_features(std::string_view cpuinfo);
std::vector<std::string> missing_cpu_features(); // this PC's

struct Compiler {
    fs::path path;
    std::string name; // "clang++ 21.1.6"
};

// The C++ compiler the builder takes without --compiler: $CXX, else Clang
// (13 or newer: [[clang::musttail]]), else GCC 15 or newer, each as clang++
// or g++ or by a versioned name (clang++-18). None, with the reason, when
// there is none.
std::optional<Compiler> find_compiler(std::string* why = nullptr);

// Bytes free for new files in `folder` (or the nearest of its parents that
// exists), and the file system that is on; none when unknown.
std::optional<uint64_t> free_space(const fs::path& folder);
uint64_t file_system(const fs::path& folder);
// The bytes of the files below `folder` (not following symbolic links).
uint64_t size_of(const fs::path& folder);

// The distribution, from os-release.
struct Distro {
    std::string id, name, variant; // ID, NAME, VARIANT_ID
    std::vector<std::string> like; // ID_LIKE, and the id itself first
    bool ostree = false;           // booted from an OSTree image (/run/ostree-booted)
};
Distro parse_os_release(std::string_view text);
Distro this_distro();

// Whether the system's own files come as one image that its package manager
// cannot add to (SteamOS, Fedora Atomic and Bazzite, openSUSE MicroOS and
// Aeon, NixOS, Guix), by the same rule as the builder's hints
// (launcher/builder/src/host.cpp): /run/ostree-booted exists, ID is steamos,
// nixos or guix, VARIANT_ID is silverblue, kinoite, sericea, onyx, atomic,
// iot or coreos, or ID or ID_LIKE contains microos, aeon, kalpa or bazzite.
bool image_based(const Distro& distro);
// What to tell there instead of a command: where `what` ("a C++ compiler")
// can come from.
std::string image_based_hint(const std::string& what);

enum class Package { kCompiler, kPython, kNumpy, kPillow, kGamescope, kPactl, kMingw, kCurl };

// The command that installs these packages on that distribution
// ("sudo pacman -S --needed clang"); empty when its package manager is not
// one this launcher knows, or cannot add to the system (image_based).
std::string install_command(const Distro& distro, const std::vector<Package>& packages);

// A program's standard output; none when it could not start or failed.
std::optional<std::string> run_output(const std::vector<std::string>& argv);

} // namespace launcher
