#include "host.h"

#include "children.h"

#include <sched.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace build {
namespace {

namespace fs = std::filesystem;

std::vector<std::string> words(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> out;
    for (std::string word; in >> word;) out.push_back(word);
    return out;
}

// The nearest of the folder and its parents that exists.
fs::path existing(fs::path folder) {
    std::error_code ec;
    while (folder.has_parent_path() && folder != folder.parent_path() && !fs::exists(folder, ec)) {
        folder = folder.parent_path();
    }
    return folder;
}

// Where distributions keep their x86-64 libraries.
constexpr const char* kLibraryFolders[] = {"/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib64", "/lib64",
                                           "/usr/lib", "/lib", "/usr/local/lib64", "/usr/local/lib"};

// ---------------------------------------------------------------- distributions

// An os-release value: quoted or not, with \ escapes.
std::string os_release_value(std::string_view value) {
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) ++i;
        out += value[i];
    }
    return out;
}

struct Package {
    const char* soname; // "" for the compiler
    const char* what;
    const char* arch;
    const char* debian;
    const char* fedora;
    const char* suse;
};

// (Debian's clang leaves the C++ library's headers to build-essential.)
constexpr Package kPackages[] = {
    {"", "Clang (or GCC 15 or newer)", "clang", "clang build-essential", "clang gcc-c++", "clang gcc-c++"},
    {"libSDL3.so.0", "SDL 3", "sdl3", "libsdl3-0", "SDL3", "libSDL3-0"},
    {"libvulkan.so.1", "the Vulkan loader", "vulkan-icd-loader", "libvulkan1", "vulkan-loader", "libvulkan1"},
    {"libshaderc_shared.so.1", "shaderc", "shaderc", "libshaderc1", "libshaderc", "libshaderc_shared1"},
    {"libcrypto.so.3", "OpenSSL 3", "openssl", "libssl3", "openssl-libs", "libopenssl3"},
    {"libzstd.so.1", "zstd", "zstd", "libzstd1", "libzstd", "libzstd1"},
    {"libz.so.1", "zlib", "zlib", "zlib1g", "zlib", "libz1"},
};

// "a", "a and b", "a, b and c".
std::string listed(const std::vector<std::string>& items) {
    std::string text;
    for (size_t i = 0; i < items.size(); ++i) {
        text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    }
    return text;
}

// The end of the sentence that names `what` is missing: how to install
// those packages here (": install it with: sudo pacman -S --needed sdl3").
// An image-based system's package manager cannot add them to the system
// (the same words as the launcher's, launcher/core/system.cpp).
std::string install_text(const std::string& what, const std::vector<const Package*>& packages, bool several,
                         const System& system) {
    if (system.image_based) {
        return ". This system's own files cannot be changed with a package manager: " + what +
               " must come from a toolbox or distrobox container, or from the system's own way of adding software.";
    }
    const std::string it = several ? "them" : "it";
    const char* Package::*names = nullptr;
    const char* command = nullptr;
    switch (system.family) {
    case Family::arch:
        names = &Package::arch;
        command = "sudo pacman -S --needed";
        break;
    case Family::debian:
        names = &Package::debian;
        command = "sudo apt install";
        break;
    case Family::fedora:
        names = &Package::fedora;
        command = "sudo dnf install";
        break;
    case Family::suse:
        names = &Package::suse;
        command = "sudo zypper install";
        break;
    case Family::other: break;
    }
    if (!command) return ": install " + it + " with your distribution's package manager";
    std::string text = ": install " + it + " with: " + command;
    std::set<std::string> named;
    for (const Package* package : packages) {
        for (const std::string& name : words(package->*names)) {
            if (named.insert(name).second) text += " " + name;
        }
    }
    return text;
}

// ---------------------------------------------------------------- libraries

bool x86_64_elf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char header[20] = {};
    if (!in.read(reinterpret_cast<char*>(header), sizeof header)) return false;
    // ELF, 64-bit, little-endian, machine 62 (x86-64).
    return std::memcmp(header, "\x7f" "ELF", 4) == 0 && header[4] == 2 && header[5] == 1 && header[18] == 62 &&
           header[19] == 0;
}

// The dynamic loader's cache (ldconfig -p): soname -> file, x86-64 only.
const std::map<std::string, fs::path>& loader_cache() {
    static const std::map<std::string, fs::path> cache = [] {
        std::map<std::string, fs::path> entries;
        std::optional<fs::path> ldconfig = find_program("ldconfig");
        for (const char* path : {"/sbin/ldconfig", "/usr/sbin/ldconfig", "/usr/bin/ldconfig"}) {
            if (!ldconfig && ::access(path, X_OK) == 0) ldconfig = path;
        }
        if (!ldconfig) return entries;
        // "\tlibSDL3.so.0 (libc6,x86-64) => /usr/lib/libSDL3.so.0"
        run({{ldconfig->string(), "-p"}}, [&](std::string_view line) {
            const size_t arrow = line.find(" => ");
            const size_t flags = line.find(" (");
            if (arrow == std::string_view::npos || flags == std::string_view::npos || flags > arrow) return;
            if (line.substr(flags, arrow - flags).find("x86-64") == std::string_view::npos) return;
            std::string_view name = line.substr(0, flags);
            name.remove_prefix(std::min(name.find_first_not_of(" \t"), name.size()));
            entries.emplace(std::string(name), fs::path(line.substr(arrow + 4)));
        });
        return entries;
    }();
    return cache;
}

std::optional<fs::path> in_folder(const fs::path& folder, const std::string& soname) {
    std::error_code ec;
    const fs::path path = folder / soname;
    if (fs::exists(path, ec) && x86_64_elf(path)) return path;
    return std::nullopt;
}

} // namespace

std::vector<std::string> missing_processor_features() {
    // x86-64-v2 and v3 as the x86-64 psABI defines them (/proc/cpuinfo calls
    // LZCNT abm, and lists XSAVE, not OSXSAVE).
    static constexpr std::pair<const char*, const char*> kFeatures[] = {
        {"cx16", "CMPXCHG16B"}, {"lahf_lm", "LAHF"}, {"popcnt", "POPCNT"}, {"ssse3", "SSSE3"},
        {"sse4_1", "SSE4.1"},   {"sse4_2", "SSE4.2"}, {"avx", "AVX"},      {"avx2", "AVX2"},
        {"bmi1", "BMI1"},       {"bmi2", "BMI2"},     {"f16c", "F16C"},    {"fma", "FMA"},
        {"abm", "LZCNT"},       {"movbe", "MOVBE"},   {"xsave", "XSAVE"}};
    std::ifstream in("/proc/cpuinfo");
    std::string flags;
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with("flags")) {
            flags = line.substr(line.find(':') + 1);
            break;
        }
    }
    if (flags.empty()) return {}; // (cannot tell: the game will)
    const std::vector<std::string> present = words(flags);
    std::vector<std::string> missing;
    for (const auto& [flag, name] : kFeatures) {
        if (std::find(present.begin(), present.end(), flag) == present.end()) missing.emplace_back(name);
    }
    return missing;
}

unsigned processors() {
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) return std::max(1, CPU_COUNT(&set));
    return static_cast<unsigned>(std::max(1L, sysconf(_SC_NPROCESSORS_ONLN)));
}

uint64_t memory_available() {
    uint64_t available = UINT64_MAX;
    std::ifstream meminfo("/proc/meminfo");
    for (std::string line; std::getline(meminfo, line);) {
        if (line.starts_with("MemAvailable:")) available = std::strtoull(line.c_str() + 13, nullptr, 10) * 1024;
    }
    // Less in a control group with a memory limit (a container, a sandbox).
    std::ifstream cgroup("/proc/self/cgroup");
    for (std::string line; std::getline(cgroup, line);) {
        if (!line.starts_with("0::")) continue;
        const fs::path folder = fs::path("/sys/fs/cgroup") / fs::path(line.substr(3)).relative_path();
        std::ifstream max(folder / "memory.max"), current(folder / "memory.current");
        uint64_t limit = 0, used = 0;
        if (max >> limit && current >> used && limit > used) available = std::min(available, limit - used);
    }
    return available;
}

uint64_t free_space(fs::path folder) {
    struct statvfs s {};
    if (statvfs(existing(folder).c_str(), &s) != 0) return UINT64_MAX;
    return static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
}

bool same_file_system(const fs::path& a, const fs::path& b) {
    struct stat sa {}, sb {};
    return ::stat(existing(a).c_str(), &sa) == 0 && ::stat(existing(b).c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
}

std::optional<fs::path> find_program(const std::string& name) {
    std::error_code ec;
    const auto runnable = [&](const fs::path& path) {
        return fs::is_regular_file(path, ec) && ::access(path.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string::npos) {
        if (runnable(name)) return fs::absolute(name, ec);
        return std::nullopt;
    }
    const char* path = std::getenv("PATH");
    std::istringstream folders(path ? path : "/usr/local/bin:/usr/bin:/bin");
    for (std::string folder; std::getline(folders, folder, ':');) {
        if (!folder.empty() && folder.front() == '/' && runnable(fs::path(folder) / name)) return fs::path(folder) / name;
    }
    return std::nullopt;
}

std::vector<std::string> compiler_candidates() {
    // Versioned names, newest first ("clang++-18" where clang-18 is
    // installed without its unversioned links).
    const auto versions = [](const std::string& prefix) {
        std::set<int, std::greater<>> found;
        const char* path = std::getenv("PATH");
        std::istringstream folders(path ? path : "/usr/local/bin:/usr/bin:/bin");
        for (std::string folder; std::getline(folders, folder, ':');) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(folder, ec)) {
                const std::string name = entry.path().filename().string();
                if (name.size() > prefix.size() && name.starts_with(prefix) &&
                    name.find_first_not_of("0123456789", prefix.size()) == std::string::npos) {
                    found.insert(std::atoi(name.c_str() + prefix.size()));
                }
            }
        }
        return found;
    };
    std::vector<std::string> names{"clang++"};
    for (const int v : versions("clang++-")) names.push_back("clang++-" + std::to_string(v));
    names.emplace_back("g++");
    for (const int v : versions("g++-")) names.push_back("g++-" + std::to_string(v));
    // (The same compiler under two names is tried once: the builder tells
    // them apart by what each says it is, not by the file the links lead to.)
    std::vector<std::string> candidates;
    for (const std::string& name : names) {
        if (const std::optional<fs::path> program = find_program(name)) candidates.push_back(program->string());
    }
    return candidates;
}

std::optional<Library> find_library(const std::string& soname, const std::vector<fs::path>& folders) {
    if (const auto it = loader_cache().find(soname); it != loader_cache().end() && x86_64_elf(it->second)) {
        return Library{it->second, true};
    }
    for (const fs::path& folder : folders) {
        if (const auto path = in_folder(folder, soname)) return Library{*path, false};
    }
    for (const char* folder : kLibraryFolders) {
        if (const auto path = in_folder(folder, soname)) return Library{*path, false};
    }
    return std::nullopt;
}

std::vector<fs::path> library_folders(const std::vector<std::string>& compiler) {
    std::vector<fs::path> folders;
    const auto add = [&](std::string_view list) {
        while (!list.empty()) {
            const size_t colon = std::min(list.find(':'), list.size());
            fs::path folder = fs::path(list.substr(0, colon)).lexically_normal();
            if (!folder.has_filename() && folder.has_relative_path()) folder = folder.parent_path(); // ("lib/")
            if (folder.is_absolute() && std::find(folders.begin(), folders.end(), folder) == folders.end()) {
                folders.push_back(folder);
            }
            list.remove_prefix(std::min(colon + 1, list.size()));
        }
    };
    if (const char* path = std::getenv("LD_LIBRARY_PATH")) add(path);
    if (const char* path = std::getenv("LIBRARY_PATH")) add(path);
    // "libraries: =/usr/lib/gcc/x86_64-pc-linux-gnu/16.2.1/:/usr/lib/../lib:..."
    Command command{compiler};
    command.argv.emplace_back("-print-search-dirs");
    run(command, [&](std::string_view line) {
        if (line.starts_with("libraries: =")) add(line.substr(12));
    });
    return folders;
}

System parse_system(std::string_view os_release, bool ostree_booted, bool usual_folders) {
    std::string id, like, variant;
    std::istringstream in{std::string(os_release)};
    for (std::string line; std::getline(in, line);) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos || line.starts_with('#')) continue;
        const std::string key = line.substr(0, equals), value = os_release_value(line.substr(equals + 1));
        if (key == "ID") id = value;
        else if (key == "ID_LIKE") like = value;
        else if (key == "VARIANT_ID") variant = value;
    }
    System system;
    system.usual_folders = usual_folders;
    // (What it is decides before what it is like: "cachyos" is like "arch".)
    const std::vector<std::string> ids = words(id + " " + like);
    for (const std::string& name : ids) {
        if (name == "arch") system.family = Family::arch;
        else if (name == "debian" || name == "ubuntu") system.family = Family::debian;
        else if (name == "fedora" || name == "rhel" || name == "centos") system.family = Family::fedora;
        else if (name == "suse" || name.starts_with("opensuse")) system.family = Family::suse;
        if (system.family != Family::other) break;
    }
    // Image-based systems, by the rule the launcher has too: rpm-ostree's
    // and bootc's mark, SteamOS, NixOS and Guix, Fedora's atomic variants,
    // openSUSE's transactional ones and Bazzite.
    constexpr std::string_view kIds[] = {"steamos", "nixos", "guix"};
    constexpr std::string_view kVariants[] = {"silverblue", "kinoite", "sericea", "onyx", "atomic", "iot", "coreos"};
    constexpr std::string_view kNames[] = {"microos", "aeon", "kalpa", "bazzite"};
    const auto one_of = [](const auto& list, std::string_view value) {
        return std::find(std::begin(list), std::end(list), value) != std::end(list);
    };
    const auto named = [&](const std::string& name) {
        return std::any_of(std::begin(kNames), std::end(kNames),
                           [&](std::string_view part) { return name.find(part) != std::string::npos; });
    };
    system.image_based = ostree_booted || one_of(kIds, id) || one_of(kVariants, variant) ||
                         std::any_of(ids.begin(), ids.end(), named);
    return system;
}

const System& this_system() {
    static const System system = [] {
        std::string text;
        for (const char* file : {"/etc/os-release", "/usr/lib/os-release"}) {
            std::ifstream in(file);
            if (!in) continue;
            std::ostringstream content;
            content << in.rdbuf();
            text = content.str();
            break;
        }
        std::error_code ec;
        const bool usual = loader_cache().contains("libc.so.6") ||
                           std::any_of(std::begin(kLibraryFolders), std::end(kLibraryFolders),
                                       [](const char* folder) { return in_folder(folder, "libc.so.6").has_value(); });
        return parse_system(text, fs::exists("/run/ostree-booted", ec), usual);
    }();
    return system;
}

std::string missing_libraries_text(const std::vector<std::string>& sonames, const System& system) {
    std::vector<std::string> names, whats;
    std::vector<const Package*> packages;
    for (const std::string& soname : sonames) {
        const auto package = std::find_if(std::begin(kPackages), std::end(kPackages),
                                          [&](const Package& p) { return soname == p.soname; });
        if (package == std::end(kPackages)) {
            names.push_back(soname);
            whats.push_back(soname);
        } else {
            names.push_back(std::string(package->what) + " (" + soname + ")");
            whats.emplace_back(package->what);
            packages.push_back(package);
        }
    }
    const bool several = sonames.size() > 1;
    std::string text = listed(names);
    if (system.usual_folders) {
        text += several ? " are not installed" : " is not installed";
    } else {
        // (NixOS, Guix: the libraries are wherever their packages put them,
        // and only these name where.)
        text += std::string(several ? " were" : " was") +
                " not found. This system keeps its libraries outside /usr/lib: the build finds them only in folders "
                "that LD_LIBRARY_PATH, LIBRARY_PATH or the compiler name";
    }
    if (packages.empty() && !system.image_based) {
        return text + (several ? ": install the libraries that provide them"
                               : ": install the library that provides it");
    }
    return text + install_text(listed(whats), packages, several, system);
}

std::string missing_compiler_text(const System& system) {
    return std::string("No C++ compiler that can build the game was found. It needs ") + kPackages[0].what +
           install_text("a C++ compiler", {&kPackages[0]}, false, system);
}

} // namespace build
