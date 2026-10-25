// The checks of the player's PC (see system.h).

#include "system.h"

#include "paths.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

extern char** environ;

namespace launcher {
namespace {

std::string unquote(std::string_view value) {
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

int major_version(std::string_view version) {
    int major = 0;
    for (char c : version) {
        if (c < '0' || c > '9') break;
        major = major * 10 + (c - '0');
    }
    return major;
}

std::string version_of(const fs::path& compiler, const char* option) {
    std::string out = run_output({compiler.string(), option}).value_or("");
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// The versions N of the programs "<prefix>N" in $PATH ("clang++-18" where
// clang-18 is installed without its unversioned links), newest first.
std::set<int, std::greater<>> versions(const std::string& prefix) {
    std::set<int, std::greater<>> found;
    const char* path = std::getenv("PATH");
    std::istringstream folders(path && *path ? path : "/usr/local/bin:/usr/bin:/bin");
    for (std::string folder; std::getline(folders, folder, ':');) {
        std::error_code ec;
        if (folder.empty()) continue;
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.size() > prefix.size() && name.starts_with(prefix) &&
                name.find_first_not_of("0123456789", prefix.size()) == std::string::npos) {
                found.insert(std::atoi(name.c_str() + prefix.size()));
            }
        }
    }
    return found;
}

// The nearest of the folder and its parents that exists.
fs::path existing(fs::path folder) {
    std::error_code ec;
    folder = normal(folder);
    while (folder.has_relative_path() && !fs::exists(folder, ec)) folder = folder.parent_path();
    return folder;
}

enum class Family { kUnknown, kArch, kDebian, kFedora, kSuse };

Family family(const Distro& distro) {
    for (const std::string& id : distro.like) {
        if (id == "arch") return Family::kArch;
        if (id == "debian" || id == "ubuntu") return Family::kDebian;
        if (id == "fedora" || id == "rhel" || id == "centos") return Family::kFedora;
        if (id == "suse" || id == "opensuse" || id.starts_with("opensuse-")) return Family::kSuse;
    }
    return Family::kUnknown;
}

} // namespace

std::vector<std::string> missing_cpu_features(std::string_view cpuinfo) {
    // x86-64-v2 and v3 (v1 is any x86-64), as the kernel names them, and as
    // people know them.
    static const std::pair<const char*, const char*> kFeatures[] = {
        {"cx16", "CMPXCHG16B"}, {"lahf_lm", "LAHF"}, {"popcnt", "POPCNT"}, {"pni", "SSE3"},
        {"ssse3", "SSSE3"},     {"sse4_1", "SSE4.1"}, {"sse4_2", "SSE4.2"}, {"avx", "AVX"},
        {"avx2", "AVX2"},       {"bmi1", "BMI1"},    {"bmi2", "BMI2"},     {"f16c", "F16C"},
        {"fma", "FMA"},         {"abm", "LZCNT"},    {"movbe", "MOVBE"},   {"xsave", "XSAVE"},
    };
    std::set<std::string> flags;
    std::istringstream in{std::string(cpuinfo)};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.starts_with("flags")) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::istringstream words(line.substr(colon + 1));
        for (std::string w; words >> w;) flags.insert(w);
        break; // (every processor lists the same)
    }
    std::vector<std::string> missing;
    for (const auto& [flag, name] : kFeatures) {
        if (!flags.contains(flag)) missing.push_back(name);
    }
    return missing;
}

std::vector<std::string> missing_cpu_features() {
    std::ifstream in("/proc/cpuinfo");
    std::stringstream text;
    text << in.rdbuf();
    return missing_cpu_features(text.str());
}

std::optional<Compiler> find_compiler(std::string* why) {
    if (const char* cxx = std::getenv("CXX"); cxx && *cxx) {
        // (It may hold a launcher first: "ccache clang++".)
        std::istringstream words(cxx);
        std::string first;
        words >> first;
        if (const auto path = find_in_path(first)) return Compiler{*path, cxx};
        if (why) *why = "$CXX is " + std::string(cxx) + ", which is not installed";
        return std::nullopt;
    }
    // In the builder's order (launcher/builder/src/host.cpp): Clang, then
    // GCC, each by its plain name and then by its versioned ones.
    std::vector<std::string> names{"clang++"};
    for (const int v : versions("clang++-")) names.push_back("clang++-" + std::to_string(v));
    names.emplace_back("g++");
    for (const int v : versions("g++-")) names.push_back("g++-" + std::to_string(v));
    std::string too_old;
    for (const std::string& name : names) {
        const auto program = find_in_path(name);
        if (!program) continue;
        const bool clang = name.starts_with("clang");
        const int needed = clang ? 13 : 15;
        const std::string version = version_of(*program, clang ? "-dumpversion" : "-dumpfullversion");
        if (major_version(version) >= needed) return Compiler{*program, name + " " + version};
        if (too_old.empty()) {
            too_old = name + " " + version + " is too old (" + std::to_string(needed) + " or newer is needed)";
        }
    }
    if (why) *why = too_old.empty() ? "no C++ compiler is installed" : too_old;
    return std::nullopt;
}

std::optional<uint64_t> free_space(const fs::path& folder) {
    struct statvfs s {};
    if (statvfs(existing(folder).c_str(), &s) != 0) return std::nullopt;
    return static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
}

uint64_t file_system(const fs::path& folder) {
    struct stat s {};
    return stat(existing(folder).c_str(), &s) == 0 ? static_cast<uint64_t>(s.st_dev) : 0;
}

uint64_t size_of(const fs::path& folder) {
    uint64_t bytes = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec) && !it->is_symlink(ec)) bytes += it->file_size(ec);
    }
    return bytes;
}

Distro parse_os_release(std::string_view text) {
    Distro d;
    std::istringstream in{std::string(text)};
    std::string line, like;
    while (std::getline(in, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos || line.starts_with('#')) continue;
        const std::string key = line.substr(0, equals), value = unquote(line.substr(equals + 1));
        if (key == "ID") d.id = value;
        else if (key == "ID_LIKE") like = value;
        else if (key == "NAME") d.name = value;
        else if (key == "VARIANT_ID") d.variant = value;
    }
    if (!d.id.empty()) d.like.push_back(d.id);
    std::istringstream words(like);
    for (std::string w; words >> w;) d.like.push_back(w);
    return d;
}

Distro this_distro() {
    Distro d;
    for (const char* file : {"/etc/os-release", "/usr/lib/os-release"}) {
        std::ifstream in(file);
        if (!in) continue;
        std::stringstream text;
        text << in.rdbuf();
        d = parse_os_release(text.str());
        break;
    }
    std::error_code ec;
    d.ostree = fs::exists("/run/ostree-booted", ec);
    return d;
}

bool image_based(const Distro& d) {
    if (d.ostree || d.id == "steamos" || d.id == "nixos" || d.id == "guix") return true;
    for (const char* variant : {"silverblue", "kinoite", "sericea", "onyx", "atomic", "iot", "coreos"}) {
        if (d.variant == variant) return true;
    }
    // (like holds the id and each word of ID_LIKE.)
    for (const std::string& id : d.like) {
        for (const char* name : {"microos", "aeon", "kalpa", "bazzite"}) {
            if (id.find(name) != std::string::npos) return true;
        }
    }
    return false;
}

std::string image_based_hint(const std::string& what) {
    // (Word for word the builder's: the player may see both.)
    return "This system's own files cannot be changed with a package manager: " + what +
           " must come from a toolbox or distrobox container, or from the system's own way of adding software.";
}

std::string install_command(const Distro& distro, const std::vector<Package>& packages) {
    // The package of each piece, per family (in the order of Family).
    static const std::map<Package, std::array<const char*, 4>> kNames = {
        // (Debian's clang leaves the C++ library's headers to build-essential,
        // as the builder says too.)
        {Package::kCompiler, {"clang", "clang build-essential", "clang gcc-c++", "clang gcc-c++"}},
        {Package::kPython, {"python", "python3", "python3", "python3"}},
        {Package::kNumpy, {"python-numpy", "python3-numpy", "python3-numpy", "python3-numpy"}},
        {Package::kPillow, {"python-pillow", "python3-pil", "python3-pillow", "python3-Pillow"}},
        {Package::kGamescope, {"gamescope", "gamescope", "gamescope", "gamescope"}},
        {Package::kPactl, {"libpulse", "pulseaudio-utils", "pulseaudio-utils", "pulseaudio-utils"}},
        {Package::kMingw, {"mingw-w64-gcc", "gcc-mingw-w64-x86-64", "mingw64-gcc", "mingw64-cross-gcc"}},
        {Package::kCurl, {"curl", "curl", "curl", "curl"}},
    };
    static const char* const kCommands[] = {"sudo pacman -S --needed", "sudo apt install", "sudo dnf install",
                                            "sudo zypper install"};
    const Family f = family(distro);
    if (f == Family::kUnknown || packages.empty() || image_based(distro)) return {};
    const size_t k = static_cast<size_t>(f) - 1;
    std::string command = kCommands[k];
    std::set<std::string> named;
    for (Package p : packages) {
        if (named.insert(kNames.at(p)[k]).second) command += std::string(" ") + kNames.at(p)[k];
    }
    return command;
}

std::optional<std::string> run_output(const std::vector<std::string>& argv) {
    if (argv.empty()) return std::nullopt;
    int pipe_fds[2];
    if (::pipe2(pipe_fds, O_CLOEXEC) != 0) return std::nullopt;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    std::string out;
    if (spawned == 0) {
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::read(pipe_fds[0], buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            out.append(buffer, static_cast<size_t>(n));
        }
    }
    ::close(pipe_fds[0]);
    if (spawned != 0) return std::nullopt;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
    return out;
}

} // namespace launcher
