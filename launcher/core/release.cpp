// The launcher's own updates (see release.h).

#include "release.h"

#include "json.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>

namespace launcher {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

} // namespace

std::optional<std::vector<unsigned>> version_numbers(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    std::vector<unsigned> numbers;
    for (;;) {
        size_t digits = 0;
        unsigned n = 0;
        while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
            n = n * 10 + static_cast<unsigned>(text[digits] - '0');
            ++digits;
        }
        if (digits == 0 || digits > 9) return std::nullopt;
        numbers.push_back(n);
        text.remove_prefix(digits);
        if (text.empty()) return numbers;
        if (text.front() != '.') return std::nullopt;
        text.remove_prefix(1);
    }
}

std::optional<int> compare_versions(std::string_view a, std::string_view b) {
    const auto x = version_numbers(a), y = version_numbers(b);
    if (!x || !y) return std::nullopt;
    for (size_t i = 0; i < std::max(x->size(), y->size()); ++i) {
        const unsigned p = i < x->size() ? (*x)[i] : 0, q = i < y->size() ? (*y)[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

ReleaseAnswer parse_release(std::string_view text) {
    ReleaseAnswer answer;
    std::string why;
    const auto json = json::parse(text, &why);
    if (!json || !json->is_object()) {
        answer.error = "GitHub's answer is not readable (" + (json ? std::string("not an object") : why) + ")";
        return answer;
    }
    const json::Value& root = *json;
    if (!root["tag_name"].is_string()) {
        // An error: its message says which.
        const std::string message = root["message"].string();
        if (message == "Not Found") answer.none = true;
        else if (has(lower(message), "rate limit")) {
            answer.error = "GitHub answers only so many questions from one network an hour: try again later";
        } else if (!message.empty()) answer.error = "GitHub answered \"" + message + "\"";
        else answer.error = "GitHub's answer names no release";
        return answer;
    }
    Release r;
    r.tag = root["tag_name"].string();
    r.page = root["html_url"].string();
    r.version = r.tag;
    if (!r.version.empty() && (r.version.front() == 'v' || r.version.front() == 'V')) r.version.erase(0, 1);
    if (!version_numbers(r.version)) {
        answer.error = "the newest release, " + r.tag + ", is not named as a version (such as v0.2.0)";
        return answer;
    }
    // Its AppImage for this kind of PC: one named for x86-64, else the only
    // one named for no processor.
    const json::Value* named = nullptr;
    std::vector<const json::Value*> plain;
    for (const json::Value& asset : root["assets"].items()) {
        const std::string name = lower(asset["name"].string());
        if (!name.ends_with(".appimage") || asset["state"].string("uploaded") != "uploaded" ||
            asset["browser_download_url"].string().empty()) {
            continue;
        }
        if (has(name, "x86_64") || has(name, "x86-64") || has(name, "amd64")) {
            if (!named) named = &asset;
        } else if (!has(name, "aarch64") && !has(name, "arm") && !has(name, "i686") && !has(name, "i386")) {
            plain.push_back(&asset);
        }
    }
    const json::Value* chosen = named ? named : plain.size() == 1 ? plain[0] : nullptr;
    if (!chosen) {
        answer.error = "version " + r.version + " has no AppImage to download";
        return answer;
    }
    r.asset = (*chosen)["name"].string();
    r.url = (*chosen)["browser_download_url"].string();
    const double size = (*chosen)["size"].number(-1);
    r.size = size > 0 ? static_cast<uint64_t>(size) : 0;
    const std::string digest = lower((*chosen)["digest"].string());
    if (digest.starts_with("sha256:") && digest.size() == 7 + 64) r.sha256 = digest.substr(7);
    answer.release = std::move(r);
    return answer;
}

std::optional<fs::path> running_appimage(const fs::path& program, const char* appimage, const char* appdir) {
    if (!appimage || !*appimage || !appdir || !*appdir || program.empty()) return std::nullopt;
    const fs::path file = appimage;
    std::error_code ec;
    if (!file.is_absolute() || !fs::is_regular_file(file, ec) || !inside(program, appdir)) return std::nullopt;
    return normal(file);
}

std::string cannot_replace(const fs::path& appimage) {
    const fs::path folder = appimage.parent_path();
    if (::access(folder.c_str(), W_OK | X_OK) != 0) return std::strerror(errno);
    return {};
}

std::optional<Downloader> find_downloader() {
    if (const auto curl = find_in_path("curl")) return Downloader{*curl, true};
    if (const auto wget = find_in_path("wget")) return Downloader{*wget, false};
    return std::nullopt;
}

std::vector<std::string> download_command(const Downloader& d, const std::string& url, bool api, const fs::path& file,
                                          const std::string& user_agent) {
    static const char* const kAccept = "Accept: application/vnd.github+json";
    static const char* const kApiVersion = "X-GitHub-Api-Version: 2022-11-28";
    std::vector<std::string> argv = {d.program.string()};
    if (d.curl) {
        // (Without -f for the API: its error answers say why. A download
        // that stalls for a minute stops.)
        argv.insert(argv.end(), {"-sS", "-L", "--connect-timeout", "20", "-A", user_agent});
        if (api) argv.insert(argv.end(), {"--max-time", "60", "-H", kAccept, "-H", kApiVersion});
        else argv.insert(argv.end(), {"-f", "--speed-limit", "1", "--speed-time", "60", "-o", file.string()});
    } else {
        argv.insert(argv.end(), {"-nv", "--timeout=60", "-U", user_agent});
        if (api) {
            argv.insert(argv.end(), {"--tries=2", "--content-on-error", std::string("--header=") + kAccept,
                                     std::string("--header=") + kApiVersion, "-O", "-"});
        } else {
            argv.insert(argv.end(), {"--tries=3", "-O", file.string()});
        }
    }
    argv.push_back(url);
    return argv;
}

fs::path update_file(const fs::path& appimage) {
    return appimage.parent_path() / ("." + appimage.filename().string() + ".update");
}

bool is_appimage(const fs::path& file) {
    unsigned char head[20] = {};
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(head), sizeof head)) return false;
    return std::memcmp(head, "\177ELF", 4) == 0 && head[8] == 'A' && head[9] == 'I' && head[10] == 2 &&
           head[18] == 0x3e && head[19] == 0;
}

std::string version_in(std::string_view output) {
    while (!output.empty()) {
        const size_t end = std::min(output.find_first_of(" \t\r\n"), output.size());
        const std::string_view word = output.substr(0, end);
        if (!word.empty() && version_numbers(word)) {
            return std::string(word.front() == 'v' || word.front() == 'V' ? word.substr(1) : word);
        }
        output.remove_prefix(std::min(end + 1, output.size()));
    }
    return {};
}

std::string make_executable(const fs::path& update, const fs::path& appimage) {
    struct stat old = {};
    const mode_t mode = ::stat(appimage.c_str(), &old) == 0 ? (old.st_mode & 07777) : 0755;
    if (::chmod(update.c_str(), mode | S_IRUSR | S_IWUSR | S_IXUSR) != 0) return std::strerror(errno);
    return {};
}

std::string replace_appimage(const fs::path& update, const fs::path& appimage) {
    if (::rename(update.c_str(), appimage.c_str()) != 0) return std::strerror(errno);
    return {};
}

bool process_alive(int pid) {
    if (pid <= 0) return false;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(in, stat)) return false;
    // "<pid> (<name>) <state> ...": the name may hold anything, ')' too.
    const size_t close = stat.rfind(')');
    if (close == std::string::npos || close + 2 >= stat.size()) return false;
    const char state = stat[close + 2];
    return state != 'Z' && state != 'X' && state != 'x';
}

} // namespace launcher
