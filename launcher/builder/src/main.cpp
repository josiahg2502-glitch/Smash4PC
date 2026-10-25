// ttt2-build: builds the game program on the player's PC from their own
// copy of the game, with the kit it is part of (launcher/builder), and
// installs it with that program. The launcher runs it and reads its
// progress from stdout (see usage()).
//
// install and update run these steps in a work folder of their own:
// check      the game's executable is the supported one; the processor, the
//            kit's programs, the compiler, the libraries (the kit links with
//            them into a program that starts), memory and free space will do
// extract    code/Tekken.rpx out of the game
// recompile  cafe-recomp translates it to C++ (about 540 MB)
// compile    the system's compiler compiles that, many files at a time
// link       with the kit's runtime into the game program
// copy       the program installs the game and itself (ttt2 --install), or
//            only itself (--update)
// finish     <install folder>/.ttt2-build says what built it

#include "children.h"
#include "host.h"
#include "wua.h"

#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace build {
namespace {

namespace fs = std::filesystem;
namespace wua = cafe::wua;

constexpr const char* kName = "Tekken Tag Tournament 2";
// The executable the runtime is written for: TEKKEN TAG 2 Wii U EDITION,
// Europe, update 16 (cafe-recomp refuses any other).
constexpr const char* kRpxSha256 = "fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019";
// An install's files (runtime/src/install.h).
constexpr const char* kMarker = ".ttt2-install";
constexpr const char* kStarted = ".ttt2-installing";
constexpr const char* kStamp = ".ttt2-build";
// The generated code's symbols that the kit's runtime uses, defined empty
// (kit_stub.cmake): the check step links it as the game program will be.
constexpr const char* kStub = "lib/generated_stub.o";

// Measured on the v16 code (2026-10-22, Clang 23 and GCC 16): the work folder
// holds 0.54 GB of C++ and 0.1 GB of objects at most, then the 0.13 GB
// program. One compile takes up to 1.08 GB with Clang (most 0.2-0.3 GB) and
// 1.31 GB with GCC (most 0.5 GB).
constexpr uint64_t kWorkBytes = 1'100'000'000;
constexpr uint64_t kProgramBytes = 200'000'000;
constexpr uint64_t kClangCompileBytes = 1'100'000'000;
constexpr uint64_t kGccCompileBytes = 1'350'000'000;

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const std::string& message) { throw Failure(message); }

std::vector<std::string> words(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> out;
    for (std::string word; in >> word;) out.push_back(word);
    return out;
}

std::string gigabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

// ---------------------------------------------------------------- output

// Text for people, on stderr.
void log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void log(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::fputs("ttt2-build: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

// A line for the launcher, on stdout: always one line, out at once.
void say(std::string line) {
    std::replace(line.begin(), line.end(), '\n', ' ');
    line += '\n';
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

using Clock = std::chrono::steady_clock;
std::vector<std::pair<std::string, Clock::time_point>> g_steps; // when each began, for the log

void step(const char* id, const std::string& text) {
    check_cancelled();
    g_steps.emplace_back(id, Clock::now());
    say(std::string("@step ") + id + " " + text);
    log("%s", text.c_str());
}

void all_done() {
    std::string times;
    for (size_t i = 0; i < g_steps.size(); ++i) {
        const Clock::time_point end = i + 1 < g_steps.size() ? g_steps[i + 1].second : Clock::now();
        char text[64];
        std::snprintf(text, sizeof text, "%s%s %.1f s", times.empty() ? "" : ", ", g_steps[i].first.c_str(),
                      std::chrono::duration<double>(end - g_steps[i].second).count());
        times += text;
    }
    log("done: %s", times.c_str());
    say("@done");
}

void progress(uint64_t done, uint64_t total) { say("@progress " + std::to_string(done) + " " + std::to_string(total)); }

// ---------------------------------------------------------------- the kit

// kit.ini: lists of arguments, ${KIT} standing for the kit's folder.
struct Kit {
    fs::path folder;
    std::string version;
    std::string needs; // of the C and C++ libraries, in words ("glibc 2.38 and ...")
    std::vector<std::string> compile, link, libraries, flags;
};

Kit read_kit() {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) fail("cannot find the kit: " + ec.message());
    Kit kit;
    kit.folder = self.parent_path().parent_path();
    std::ifstream ini(kit.folder / "kit.ini"), version(kit.folder / "VERSION");
    if (!ini || !std::getline(version, kit.version)) {
        fail("the launcher is incomplete: " + kit.folder.string() + " has no kit.ini or VERSION");
    }
    std::map<std::string, std::vector<std::string>*> lists = {
        {"compile", &kit.compile}, {"link", &kit.link}, {"libraries", &kit.libraries}, {"flags", &kit.flags}};
    for (std::string line; std::getline(ini, line);) {
        const size_t equals = line.find('=');
        if (line.starts_with('#') || equals == std::string::npos) continue;
        const std::vector<std::string> key = words(line.substr(0, equals));
        if (key.size() == 1 && key[0] == "needs") {
            for (const std::string& word : words(line.substr(equals + 1))) {
                kit.needs += (kit.needs.empty() ? "" : " ") + word;
            }
            continue;
        }
        const auto list = key.size() == 1 ? lists.find(key[0]) : lists.end();
        if (list == lists.end()) continue;
        for (std::string arg : words(line.substr(equals + 1))) {
            for (size_t at; (at = arg.find("${KIT}")) != std::string::npos;) arg.replace(at, 6, kit.folder.string());
            list->second->push_back(arg);
        }
    }
    if (kit.compile.empty() || kit.link.empty()) fail("the launcher is incomplete: its kit.ini says nothing to build with");
    if (!fs::exists(kit.folder / kStub, ec)) {
        fail("the launcher is incomplete: " + kit.folder.string() + " has no " + kStub);
    }
    return kit;
}

// ---------------------------------------------------------------- the game

std::string sha256_hex(const std::vector<uint8_t>& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr);
    std::string hex;
    for (unsigned int i = 0; i < length; ++i) {
        char byte[3];
        std::snprintf(byte, sizeof byte, "%02x", digest[i]);
        hex += byte;
    }
    return hex;
}

// The game as the player has it, read as ttt2 --install reads it: a .wua
// archive (the update's files over the base game's, wua.h) or a folder with
// code/, content/ and meta/ (or an install folder, with them in game/).
class Game {
public:
    explicit Game(const fs::path& path) : path_(path) {
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            folder_ = fs::exists(path / "code/Tekken.rpx", ec) ? path : path / "game";
            if (!fs::exists(folder_ / "code/Tekken.rpx", ec)) fail(path.string() + " holds no " + kName + " (no code/Tekken.rpx)");
            return;
        }
        if (!fs::exists(path, ec)) fail(path.string() + " does not exist");
        archive_.reset(ZArchiveReader::OpenFromFile(path));
        if (!archive_) fail(path.filename().string() + " is not a .wua archive");
        layers_ = wua::game_layers(*archive_);
        if (layers_.empty()) fail(path.filename().string() + " holds no " + kName + " (no code/Tekken.rpx)");
    }

    // code/Tekken.rpx, if it is the executable the port is made for.
    std::vector<uint8_t> rpx() {
        std::vector<uint8_t> data;
        if (archive_) {
            const ZArchiveNodeHandle node = wua::find(*archive_, layers_, "code/Tekken.rpx");
            data.resize(archive_->GetFileSize(node));
            if (archive_->ReadFromFile(node, 0, data.size(), data.data()) != data.size()) {
                fail("cannot read code/Tekken.rpx from " + path_.string() + ": the archive is damaged");
            }
        } else {
            const fs::path file = folder_ / "code/Tekken.rpx";
            std::error_code ec;
            std::ifstream in(file, std::ios::binary);
            data.resize(fs::file_size(file, ec));
            if (ec || !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
                fail("cannot read " + file.string());
            }
        }
        if (sha256_hex(data) != kRpxSha256) {
            fail(path_.filename().string() + " is not the version of the game the port is made for: TEKKEN TAG 2 "
                                             "Wii U EDITION from Europe, with update 16 (others have different code)");
        }
        return data;
    }

    // Each file's path in the title ("content/hdd/x.bin") and size.
    std::vector<std::pair<std::string, uint64_t>> files() {
        std::vector<std::pair<std::string, uint64_t>> out;
        if (archive_) {
            for (const wua::File& f : wua::game_files(*archive_, layers_)) out.emplace_back(f.path, f.size);
            return out;
        }
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(folder_, fs::directory_options::follow_directory_symlink, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 32) fail(folder_.string() + " has folders too deep (a link loop?)");
            if (it->is_regular_file(ec)) out.emplace_back(it->path().lexically_relative(folder_).string(), it->file_size(ec));
        }
        if (ec) fail("cannot list " + folder_.string() + ": " + ec.message());
        return out;
    }

private:
    fs::path path_, folder_;
    std::unique_ptr<ZArchiveReader> archive_;
    std::vector<std::string> layers_;
};

fs::path full_path(const fs::path& path) {
    std::error_code ec;
    fs::path full = fs::absolute(path, ec).lexically_normal();
    if (!full.has_filename()) full = full.parent_path(); // a trailing slash
    return full;
}

// The folder ttt2 --install installs into when given `chosen`
// (runtime/src/install.cpp): a new or empty folder or an install, else a
// folder of the game's own inside it.
fs::path install_target(const fs::path& chosen) {
    std::error_code ec;
    const auto usable = [&](const fs::path& folder) {
        if (!fs::exists(fs::symlink_status(folder, ec))) return true;
        if (!fs::is_directory(folder, ec)) return false;
        return fs::exists(folder / kMarker, ec) || fs::exists(folder / kStarted, ec) || fs::is_empty(folder, ec);
    };
    if (usable(chosen)) return chosen;
    if (!fs::is_directory(chosen, ec)) fail(chosen.string() + " is not a folder");
    if (usable(chosen / kName)) return chosen / kName;
    fail((chosen / kName).string() + " already exists and is not an install of the game: choose another folder");
}

void check_space(const fs::path& folder, uint64_t needed, const std::string& what) {
    const uint64_t available = free_space(folder);
    if (needed > available) {
        fail("not enough free space for " + what + " in " + folder.string() + ": " + gigabytes(needed) + " needed, " +
             gigabytes(available) + " free");
    }
}

// ---------------------------------------------------------------- the work folder

// run-XXXXXX in the work root, locked while the build runs: a build that
// was killed leaves its folder unlocked, and the next one deletes it.
class Work {
public:
    Work(const fs::path& root, bool own_root, bool keep) : root_(root), own_root_(own_root), keep_(keep) {
        std::error_code ec;
        fs::create_directories(root_, ec);
        if (ec) fail("cannot create the work folder " + root_.string() + ": " + ec.message());
        for (const auto& entry : fs::directory_iterator(root_, ec)) {
            if (!entry.path().filename().string().starts_with("run-") || !entry.is_directory(ec) || entry.is_symlink(ec)) {
                continue;
            }
            const int fd = ::open((entry.path() / "lock").c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                log("deleting the work folder of a build that was stopped: %s", entry.path().c_str());
                fs::remove_all(entry.path(), ec);
            }
            ::close(fd);
        }
        std::string folder = (root_ / "run-XXXXXX").string();
        if (::mkdtemp(folder.data()) == nullptr) fail("cannot create a folder in " + root_.string() + ": " + std::strerror(errno));
        folder_ = folder;
        lock_ = ::open((folder_ / "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (lock_ < 0 || ::flock(lock_, LOCK_EX | LOCK_NB) != 0) fail("cannot write in " + folder_.string());
    }

    ~Work() {
        std::error_code ec;
        if (keep_) {
            log("kept the work folder %s", folder_.c_str());
        } else {
            fs::remove_all(folder_, ec);
            if (own_root_) fs::remove(root_, ec); // (when empty)
        }
        ::close(lock_);
    }

    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;

    const fs::path& folder() const { return folder_; }
    bool keep() const { return keep_; }

private:
    fs::path root_, folder_;
    bool own_root_, keep_;
    int lock_ = -1;
};

// ---------------------------------------------------------------- the compiler

struct Compiler {
    std::vector<std::string> command; // the program, and arguments given with it in $CXX
    std::string id;                   // "clang 23.1.1", "gcc 16.2.1"
    bool clang = false;
    // The program the links lead to and what it says it is: the same
    // compiler under another name has the same (ccache's links do not).
    std::string fingerprint;
};

struct Options {
    std::string command;
    std::vector<std::string> positional;
    std::optional<fs::path> work;
    unsigned jobs = 0;
    bool keep = false;
    std::optional<std::string> compiler;
};

// What the check step finds, for the steps after it.
struct Build {
    Kit kit;
    std::unique_ptr<Work> work;
    Compiler compiler;
    std::vector<std::string> libraries; // their files, and run paths to the folders of those not in the loader's cache
    unsigned jobs = 1;
    uint64_t compile_bytes = 0; // one compile's memory, at most
};

// What a compiler must do for the game: guaranteed tail calls (PPC_MUSTTAIL
// in cafe/ppc_ops.h), the kit's flags, and linking C++ (gcc, the C driver,
// compiles C++ but leaves out its libraries) with GCC's C++ library, which
// the kit's runtime was compiled against.
constexpr const char* kProbe = R"probe(#include "cafe/generated.h"
#include "cafe/ppc_ops.h"
#if defined(__clang__)
#if !__has_cpp_attribute(clang::musttail)
#error "this Clang has no [[clang::musttail]]: a newer Clang is needed"
#endif
#elif !defined(__GNUC__) || __GNUC__ < 15
#error "GCC 15 or newer, or Clang, is needed ([[gnu::musttail]])"
#endif
#include <string>
#include <thread>
#if defined(_LIBCPP_VERSION)
#error "it uses LLVM's C++ library (libc++): the game program needs GCC's (libstdc++)"
#endif
PPC_FUNC(probe_next) {}
PPC_FUNC(probe) { PPC_TAIL_CALL(probe_next); }
int main(int argc, char**) {
    std::thread([&] { argc += static_cast<int>(std::to_string(std::lround(argc * 1.5)).size()); }).join();
    return argc;
}
)probe";

// How a command went, from what it printed (stdout and stderr).
struct Ran {
    Ended ended;
    std::string first, error; // its first line, and the first that says "error" or "undefined"
    std::vector<std::string> held; // all of it, when it was not logged as it came

    bool ok() const { return succeeded(ended); }
    // What went wrong: the line that says so, else how it ended.
    std::string reason() const {
        const std::string& text = error.empty() ? first : error;
        return text.empty() ? describe(ended) : text;
    }
    void log() const {
        for (const std::string& line : held) std::fprintf(stderr, "%s\n", line.c_str());
    }
};

// Runs a command, its output into the log as it comes; with `quiet`, held
// for the caller to log if it went wrong.
Ran run_logged(const Command& command, bool quiet = false) {
    Ran ran;
    Command logged = command;
    logged.capture_errors = true;
    ran.ended = run(logged, [&](std::string_view line) {
        if (quiet) ran.held.emplace_back(line);
        else std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
        if (ran.first.empty()) ran.first = line;
        if (ran.error.empty() && (line.find("error") != std::string_view::npos || line.find("undefined") != std::string_view::npos)) {
            ran.error = line;
        }
    });
    return ran;
}

// What the compiler is, from the macros it defines: none when it is a Clang
// or GCC that makes x86-64 programs (and fills in its id), else why not.
std::optional<std::string> identify(Compiler& compiler) {
    std::map<std::string, std::string> macros;
    std::set<std::string> listing;
    Command macros_command{compiler.command};
    macros_command.argv.insert(macros_command.argv.end(), {"-x", "c++", "-E", "-dM", "/dev/null"});
    const Ended listed = run(macros_command, [&](std::string_view line) {
        listing.emplace(line);
        const std::vector<std::string> define = words(std::string(line));
        if (define.size() >= 3 && define[0] == "#define") macros[define[1]] = define[2];
    });
    if (!succeeded(listed)) return "it does not run (" + describe(listed) + ")";
    std::error_code ec;
    compiler.fingerprint = fs::canonical(compiler.command[0], ec).string();
    for (size_t i = 1; i < compiler.command.size(); ++i) compiler.fingerprint += " " + compiler.command[i];
    for (const std::string& line : listing) compiler.fingerprint += "\n" + line;
    if (!macros.contains("__x86_64__")) return "it does not make x86-64 programs";
    compiler.clang = macros.contains("__clang__");
    const auto part = [&](const char* clang, const char* gcc) { return macros[compiler.clang ? clang : gcc]; };
    compiler.id = (compiler.clang ? "clang " : "gcc ") + part("__clang_major__", "__GNUC__") + "." +
                  part("__clang_minor__", "__GNUC_MINOR__") + "." + part("__clang_patchlevel__", "__GNUC_PATCHLEVEL__");
    if (!compiler.clang && !macros.contains("__GNUC__")) return "it is neither Clang nor GCC";
    if (!compiler.clang && std::atoi(macros["__GNUC__"].c_str()) < 15) {
        return "it is GCC " + compiler.id.substr(4) + ": GCC 15 or newer is needed";
    }
    return std::nullopt;
}

// Whether `compiler` can build the game's code: none when it can, else why
// not.
std::optional<std::string> probe(const Compiler& compiler, const Kit& kit, const fs::path& work) {
    {
        std::ofstream source(work / "probe.cpp");
        source << kProbe;
        if (!source) fail("cannot write in " + work.string());
    }
    Command compile{compiler.command};
    compile.argv.insert(compile.argv.end(), kit.compile.begin(), kit.compile.end());
    compile.argv.insert(compile.argv.end(), {(work / "probe.cpp").string(), "-o", (work / "probe").string(), "-pie"});
    compile.argv.insert(compile.argv.end(), kit.flags.begin(), kit.flags.end());
    if (const Ran ran = run_logged(compile); !ran.ok()) {
        const std::string error = ran.reason();
        const bool c_driver = error.find("undefined reference") != std::string::npos ||
                              error.find("undefined symbol") != std::string::npos;
        return "it cannot build the game's code: " + error + (c_driver ? " (a C compiler? Use clang++ or g++)" : "");
    }
    return std::nullopt;
}

// Whether what a link or the dynamic loader said is about the C or C++
// library: a symbol it lacks, or a version of them that it lacks.
bool about_c_library(const std::string& said) {
    const std::string_view text = said;
    std::string_view symbol = text;
    for (const std::string_view mark : {"undefined reference to `", "undefined symbol: "}) {
        if (const size_t at = text.find(mark); at != std::string_view::npos) symbol = text.substr(at + mark.size());
    }
    for (const char* sign : {"`GLIBC", "@GLIBC", "libstdc++", "libc.so", "std::", "__cxa", "__gxx", "__isoc",
                             "operator new", "operator delete", "_ZNSt", "_ZNKSt", "_ZSt"}) {
        if (symbol.find(sign) != std::string_view::npos) return true;
    }
    return false;
}

std::string older_system(const Kit& kit) {
    return "This system's C or C++ library is older than the launcher's" +
           (kit.needs.empty() ? std::string() : ": it needs " + kit.needs) +
           ". Update the system, or use a newer release of it.";
}

// Why `what` (a program of the kit's, or the game program) does not start
// here, from what the dynamic loader said.
std::string not_starting(const std::string& what, const std::string& said, const Kit& kit) {
    // "...: error while loading shared libraries: libSDL3.so.0: cannot open shared object file: ..."
    if (const size_t at = said.find(": cannot open shared object file"); at != std::string::npos) {
        const size_t name = said.rfind(' ', at) + 1;
        return what + " does not start on this system: " + missing_libraries_text({said.substr(name, at - name)});
    }
    // "...: /usr/lib/libstdc++.so.6: version `GLIBCXX_3.4.35' not found (required by ...)"
    if (about_c_library(said)) return what + " does not start on this system (" + said + "). " + older_system(kit);
    return what + " does not start on this system: " + said;
}

// The kit's own programs start here: cafe-recomp says how to use it (with
// exit code 2) when it is given nothing, once the dynamic loader has found
// the libraries it needs at the versions it needs.
void check_tools(const Kit& kit) {
    const Ran ran = run_logged({{(kit.folder / "bin/cafe-recomp").string()}}, true);
    if (WIFEXITED(ran.ended.status) && WEXITSTATUS(ran.ended.status) == 2 && ran.first.starts_with("usage:")) return;
    ran.log();
    fail(not_starting("The launcher's cafe-recomp", ran.reason(), kit));
}

// The kit's libraries, for this compiler's link; fails naming those that
// are missing.
void find_libraries(Build& build) {
    const std::vector<fs::path> folders = library_folders(build.compiler.command);
    build.libraries.clear();
    std::vector<std::string> missing;
    std::set<fs::path> run_paths;
    for (const std::string& soname : build.kit.libraries) {
        const std::optional<Library> library = find_library(soname, folders);
        if (!library) {
            missing.push_back(soname);
            continue;
        }
        build.libraries.push_back(library->file.string());
        // The dynamic loader finds one that is not in its cache only through
        // LD_LIBRARY_PATH, which the game started from the menu has not.
        // (-Xlinker: a comma in -Wl,... would split the folder's name.)
        if (library->cached) continue;
        log("%s is not in the dynamic loader's cache: the game program finds it through a run path (%s)",
            soname.c_str(), library->file.c_str());
        if (const fs::path folder = library->file.parent_path(); run_paths.insert(folder).second) {
            build.libraries.insert(build.libraries.end(), {"-Xlinker", "-rpath", "-Xlinker", folder.string()});
        }
    }
    if (!missing.empty()) fail(missing_libraries_text(missing));
}

// Links the objects into `program` with the kit, its arguments in a
// response file (both compilers' syntax: each quoted, " and \ escaped).
Ran link(const Build& build, const std::vector<fs::path>& objects, const fs::path& program) {
    fs::path response = program;
    response += ".rsp";
    {
        std::ofstream out(response);
        const auto put = [&](const std::string& arg) {
            out << '"';
            for (const char c : arg) {
                if (c == '"' || c == '\\') out << '\\';
                out << c;
            }
            out << "\"\n";
        };
        put("-o");
        put(program.string());
        for (const fs::path& object : objects) put(object.string());
        for (const std::string& arg : build.kit.link) put(arg);
        for (const std::string& library : build.libraries) put(library);
        for (const std::string& arg : build.kit.flags) put(arg);
        if (!out) fail("cannot write " + response.string());
    }
    Command command{build.compiler.command};
    command.argv.push_back("@" + response.string());
    return run_logged(command);
}

// Whether the game program will link and start here with the build's
// compiler and libraries: the kit's stub of the generated code linked as
// the program will be, then started with --help (it only says how to use
// it). None when it does, else why not. The compiler's own probe has
// passed, so what is missing is the system's.
std::optional<std::string> probe_kit(const Build& build) {
    const fs::path program = build.work->folder() / "kit-check";
    if (const Ran ran = link(build, {build.kit.folder / kStub}, program); !ran.ok()) {
        const std::string said = ran.reason();
        return "The game program cannot be linked on this system with " + build.compiler.id + " (" + said + "). " +
               (about_c_library(said) ? older_system(build.kit)
                                      : "A library it uses is older here than the launcher needs: update the system.");
    }
    const Ran ran = run_logged({{program.string(), "--help"}}, true);
    if (ran.ok()) return std::nullopt;
    ran.log();
    return not_starting("The game program", ran.reason(), build.kit);
}

// --compiler, else $CXX, else the first of clang++ and g++ (any version) that
// can build the game and with which the kit links into a program that
// starts; with the libraries for it.
void choose_compiler(Build& build, const std::optional<std::string>& named) {
    std::vector<std::vector<std::string>> candidates;
    const char* cxx = std::getenv("CXX");
    const bool chosen = named || (cxx && !words(cxx).empty());
    if (named) candidates.push_back({*named});
    else if (chosen) candidates.push_back(words(cxx));
    else for (const std::string& program : compiler_candidates()) candidates.push_back({program});
    std::string reasons;
    std::optional<std::string> unlinkable; // why the kit did not link with a compiler that works
    std::set<std::string> tried;
    for (std::vector<std::string>& command : candidates) {
        const std::string name = command[0];
        Compiler compiler;
        compiler.command = command;
        std::optional<std::string> reason = "it is not installed";
        if (const std::optional<fs::path> program = find_program(name)) {
            compiler.command[0] = program->string();
            reason = identify(compiler);
            if (!reason && !tried.insert(compiler.fingerprint).second) {
                log("%s is a compiler tried already", name.c_str());
                continue;
            }
            if (!reason) reason = probe(compiler, build.kit, build.work->folder());
        }
        if (reason) {
            if (chosen) fail("cannot use the compiler " + name + ": " + *reason);
            log("cannot use the compiler %s: %s", name.c_str(), reason->c_str());
            reasons += (reasons.empty() ? " (" : "; ") + name + ": " + *reason;
            continue;
        }
        build.compiler = compiler;
        find_libraries(build);
        const std::optional<std::string> problem = probe_kit(build);
        if (!problem) {
            log("compiler: %s (%s)", compiler.id.c_str(), compiler.command[0].c_str());
            return;
        }
        if (chosen) fail(*problem);
        log("cannot use the compiler %s: the game program does not link or start with it", name.c_str());
        if (!unlinkable) unlinkable = problem;
    }
    if (unlinkable) fail(*unlinkable);
    fail(missing_compiler_text() + (reasons.empty() ? "" : reasons + ")"));
}

// ---------------------------------------------------------------- building

// --work, else $XDG_CACHE_HOME/ttt2/build.
fs::path work_root(const Options& options) {
    if (options.work) return full_path(*options.work);
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    return (cache && *cache == '/' ? fs::path(cache) : fs::path(home ? home : "/") / ".cache") / "ttt2" / "build";
}

// The room the copy step takes in the install folder for the game program.
// On the work folder's file system the program built there is still there
// meanwhile (with --keep, all that was built).
uint64_t program_room(const Options& options, const fs::path& folder) {
    if (!same_file_system(work_root(options), folder)) return kProgramBytes;
    return kProgramBytes + (options.keep ? kWorkBytes : kProgramBytes);
}

Build prepare(const Options& options) {
    if (const std::vector<std::string> missing = missing_processor_features(); !missing.empty()) {
        std::string names;
        for (const std::string& name : missing) names += (names.empty() ? "" : ", ") + name;
        fail("this processor cannot run the game: it lacks " + names +
             " (the game needs x86-64-v3: Intel Haswell, AMD Excavator or newer)");
    }
    Build build;
    build.kit = read_kit();
    log("kit %s in %s", build.kit.version.c_str(), build.kit.folder.c_str());
    check_tools(build.kit);
    build.work = std::make_unique<Work>(work_root(options), !options.work, options.keep);
    check_space(build.work->folder(), kWorkBytes, "building the game program");
    choose_compiler(build, options.compiler);
    build.compile_bytes = build.compiler.clang ? kClangCompileBytes : kGccCompileBytes;
    const uint64_t memory = memory_available();
    build.jobs = options.jobs ? options.jobs
                              : static_cast<unsigned>(std::clamp<uint64_t>(memory / build.compile_bytes, 1, processors()));
    log("%u compiles at a time (%u processors, %s of memory available)", build.jobs, processors(),
        gigabytes(memory).c_str());
    return build;
}

// Compiles every generated file, the largest first (they take longest), as
// many at a time as the build allows; a further one starts only while the
// memory available still holds one more compile as large as the largest so
// far (other programs may need more meanwhile).
std::vector<fs::path> compile(const Build& build, const fs::path& generated, const fs::path& objects) {
    std::vector<std::pair<uint64_t, fs::path>> sources;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(generated, ec)) {
        if (entry.path().extension() == ".cpp") sources.emplace_back(entry.file_size(ec), entry.path());
    }
    if (sources.empty()) fail("cafe-recomp wrote no code into " + generated.string());
    std::sort(sources.begin(), sources.end(), std::greater<>());
    fs::create_directories(objects, ec);
    struct Job {
        size_t index;
        Clock::time_point started;
    };
    std::map<pid_t, Job> running;
    std::vector<fs::path> outputs;
    size_t next = 0;
    uint64_t largest = 0;
    const Clock::time_point started = Clock::now();
    progress(0, sources.size());
    try {
        while (outputs.size() < sources.size()) {
            check_cancelled();
            bool waiting = false;
            while (next < sources.size() && running.size() < build.jobs) {
                if (!running.empty() && memory_available() < std::max(largest, build.compile_bytes)) {
                    waiting = true;
                    break;
                }
                const fs::path& source = sources[next].second;
                Command command{build.compiler.command};
                command.argv.insert(command.argv.end(), build.kit.compile.begin(), build.kit.compile.end());
                command.argv.insert(command.argv.end(),
                                    {"-c", source.string(), "-o", (objects / source.stem()).string() + ".o"});
                running.emplace(start(command), Job{next++, Clock::now()});
            }
            wait_event(waiting ? 500 : -1);
            for (const Ended& ended : reap()) {
                const auto it = running.find(ended.pid);
                if (it == running.end()) continue;
                const Job job = it->second;
                running.erase(it);
                const fs::path& source = sources[job.index].second;
                if (!succeeded(ended)) {
                    const bool killed = WIFSIGNALED(ended.status) && WTERMSIG(ended.status) == SIGKILL;
                    fail("compiling " + source.filename().string() + " failed (" + describe(ended) + ")" +
                         (killed ? ": out of memory? With --jobs fewer compile at a time" : ""));
                }
                largest = std::max(largest, ended.peak_bytes);
                outputs.push_back((objects / source.stem()).string() + ".o");
                log("compiled %s in %.1f s, %.0f MB", source.filename().c_str(),
                    std::chrono::duration<double>(Clock::now() - job.started).count(),
                    static_cast<double>(ended.peak_bytes) / 1e6);
                progress(outputs.size(), sources.size());
            }
        }
    } catch (...) {
        std::vector<pid_t> children;
        for (const auto& [pid, job] : running) children.push_back(pid);
        stop(children);
        throw;
    }
    log("compiled %zu files in %.0f s, %u at a time, at most %.0f MB each", sources.size(),
        std::chrono::duration<double>(Clock::now() - started).count(), build.jobs, static_cast<double>(largest) / 1e6);
    std::sort(outputs.begin(), outputs.end());
    return outputs;
}

// Steps extract to link: the game program, built from `rpx`.
fs::path build_program(const Build& build, const std::vector<uint8_t>& rpx) {
    const fs::path work = build.work->folder();
    step("extract", "Reading the game's code");
    {
        std::ofstream out(work / "Tekken.rpx", std::ios::binary);
        out.write(reinterpret_cast<const char*>(rpx.data()), static_cast<std::streamsize>(rpx.size()));
        if (!out) fail("cannot write " + (work / "Tekken.rpx").string());
    }

    step("recompile", "Translating the game's code to C++");
    const fs::path generated = work / "generated";
    if (const Ran ran = run_logged({{(build.kit.folder / "bin/cafe-recomp").string(), (work / "Tekken.rpx").string(),
                                     generated.string()}});
        !ran.ok()) {
        fail("translating the game's code failed: " + ran.reason());
    }
    // (Each step's leftovers go as soon as they are not needed: the copy
    // step needs their room when the work folder is on the install's file
    // system, see program_room.)
    std::error_code ec;
    if (!build.work->keep()) fs::remove(work / "Tekken.rpx", ec);

    step("compile", "Compiling the game's code");
    const std::vector<fs::path> objects = compile(build, generated, work / "objects");
    if (!build.work->keep()) fs::remove_all(generated, ec);

    step("link", "Linking the game program");
    const fs::path program = work / "ttt2";
    if (const Ran ran = link(build, objects, program); !ran.ok()) {
        fail("linking the game program failed: " + ran.reason());
    }
    if (!build.work->keep()) fs::remove_all(work / "objects", ec);
    return program;
}

// Runs the built program's --install or --update (with its progress lines
// passed on); the install folder it used.
fs::path run_program(const fs::path& program, const std::vector<std::string>& arguments, const std::string& what) {
    Command command{{program.string()}, {"TTT2_INSTALL_PROGRESS=1"}};
    command.argv.insert(command.argv.end(), arguments.begin(), arguments.end());
    std::optional<fs::path> folder;
    std::string error;
    const Ended ended = run(command, [&](std::string_view line) {
        if (line.starts_with("@progress ")) {
            say(std::string(line));
        } else if (line.starts_with("@folder ")) {
            folder = std::string(line.substr(8));
            say(std::string(line));
        } else if (line.starts_with("@error ")) {
            error = line.substr(7);
        } else {
            std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
        }
    });
    if (!succeeded(ended)) fail(error.empty() ? what + " failed (" + describe(ended) + ")" : error);
    if (!folder) fail(what + ": the game program did not say where the game is");
    return *folder;
}

void write_stamp(const fs::path& folder, const Build& build) {
    const fs::path stamp = folder / kStamp;
    fs::path part = stamp;
    part += ".part";
    {
        std::ofstream out(part);
        out << "kit " << build.kit.version << "\ncompiler " << build.compiler.id << '\n';
        if (!out) fail("cannot write " + part.string());
    }
    std::error_code ec;
    fs::rename(part, stamp, ec);
    if (ec) fail("cannot write " + stamp.string() + ": " + ec.message());
}

// ---------------------------------------------------------------- commands

// With GAME, whether it is the game the port is made for; without, whether
// this computer can build the game program (install's check step, without
// the game).
int check(const Options& options) {
    if (options.positional.size() > 1) fail("usage: ttt2-build check [GAME]");
    if (options.positional.empty()) prepare(options);
    else Game(full_path(options.positional[0])).rpx();
    say("@ok");
    return 0;
}

int install(const Options& options) {
    if (options.positional.size() != 2) fail("usage: ttt2-build install GAME FOLDER");
    const fs::path game_path = full_path(options.positional[0]), chosen = full_path(options.positional[1]);
    if (chosen.string().find('\n') != std::string::npos) fail("the install folder's name has a line break in it");

    step("check", "Checking the game and this computer");
    Game game(game_path);
    const std::vector<uint8_t> rpx = game.rpx();
    const fs::path target = install_target(chosen);
    uint64_t needed = program_room(options, target);
    std::error_code ec;
    for (const auto& [path, size] : game.files()) {
        if (!fs::exists(fs::symlink_status(target / "game" / path, ec))) needed += size;
    }
    check_space(target, needed, "the game");
    Build build = prepare(options);

    const fs::path program = build_program(build, rpx);
    step("copy", "Copying the game");
    const fs::path folder = run_program(program, {"--install", game_path.string(), chosen.string()}, "installing the game");
    step("finish", "Finishing");
    write_stamp(folder, build);
    build.work.reset();
    all_done();
    return 0;
}

int update(const Options& options) {
    if (options.positional.size() != 1) fail("usage: ttt2-build update FOLDER");
    const fs::path folder = full_path(options.positional[0]);

    step("check", "Checking the game and this computer");
    std::error_code ec;
    if (!fs::exists(folder / kMarker, ec)) fail(folder.string() + " holds no complete install of " + kName);
    const std::vector<uint8_t> rpx = Game(folder).rpx();
    check_space(folder, program_room(options, folder), "the game program");
    Build build = prepare(options);

    const fs::path program = build_program(build, rpx);
    step("copy", "Copying the game program");
    run_program(program, {"--update", folder.string()}, "updating the game program");
    step("finish", "Finishing");
    write_stamp(folder, build);
    build.work.reset();
    all_done();
    return 0;
}

void usage(FILE* out) {
    std::fprintf(out,
                 "usage: ttt2-build check [GAME] [options]\n"
                 "       ttt2-build install GAME FOLDER [options]\n"
                 "       ttt2-build update FOLDER [options]\n"
                 "Builds Tekken Tag Tournament 2's program from the game (GAME: its .wua, or a\n"
                 "folder with code/, content/ and meta/, or an install folder) with this\n"
                 "computer's C++ compiler, then installs the game into FOLDER, or rebuilds\n"
                 "the program of the install in FOLDER. check checks the game, or without\n"
                 "GAME this computer: its compiler and libraries, and that the program links\n"
                 "with them and starts.\n"
                 "  --work DIR       where to build (default $XDG_CACHE_HOME/ttt2/build)\n"
                 "  --jobs N         compiles at a time (default: as processors and memory allow)\n"
                 "  --keep           keep what was built in the work folder\n"
                 "  --compiler PATH  the C++ compiler (default $CXX, else clang++, else g++ 15+)\n"
                 "Progress for the launcher, one line each on stdout:\n"
                 "  @step <id> <text>   check, extract, recompile, compile, link, copy, finish\n"
                 "  @progress <done> <total>   in the step (compile: files; copy: bytes)\n"
                 "  @folder <path>   the install folder used\n"
                 "  @error <text>    why it failed (exit code 1; 130 when stopped by a signal)\n"
                 "  @done / @ok      install or update done / check passed\n");
}

} // namespace
} // namespace build

int main(int argc, char** argv) {
    using namespace build;
    Options options;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) fail(arg + " needs a value");
                return argv[++i];
            };
            if (arg == "--help" || arg == "-h") {
                usage(stdout);
                return 0;
            }
            if (arg == "--work") options.work = value();
            else if (arg == "--keep") options.keep = true;
            else if (arg == "--compiler") options.compiler = value();
            else if (arg == "--jobs") {
                const std::string jobs = value();
                options.jobs = static_cast<unsigned>(std::atoi(jobs.c_str()));
                if (options.jobs < 1 || jobs.find_first_not_of("0123456789") != std::string::npos) {
                    fail("--jobs needs a number of 1 or more");
                }
            } else if (arg.starts_with("--")) fail("unknown option " + arg);
            else if (options.command.empty()) options.command = arg;
            else options.positional.push_back(arg);
        }
        if (options.command != "check" && options.command != "install" && options.command != "update") {
            usage(stderr);
            say(options.command.empty() ? "@error no command given" : "@error unknown command " + options.command);
            return 2;
        }
        watch_signals();
        if (options.command == "check") return check(options);
        if (options.command == "install") return install(options);
        return update(options);
    } catch (const Cancelled&) {
        log("stopped");
        say("@error stopped");
        return 130;
    } catch (const std::exception& e) {
        log("%s", e.what());
        say(std::string("@error ") + e.what());
        return 1;
    }
}
