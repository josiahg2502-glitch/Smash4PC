#include "host/display_settings.h"

#include "host/files.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace cafe::host::display {
namespace {

namespace fs = std::filesystem;

// A hand-edited file is a few hundred bytes; anything much larger is not one.
constexpr uint64_t kMaxFileSize = 64 * 1024;

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// `#` starts a comment at the start of a line or after a space (a `#`
// inside a word is part of it).
std::string_view strip_comment(std::string_view line) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '#' && (i == 0 || space(line[i - 1]))) return line.substr(0, i);
    }
    return line;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return lower(x) == lower(y);
           });
}

std::string quoted(std::string_view s) { return "\"" + std::string(s) + "\""; }

std::optional<bool> yes_no(std::string_view value) {
    for (std::string_view yes : {"yes", "true", "on", "1"}) {
        if (same(value, yes)) return true;
    }
    for (std::string_view no : {"no", "false", "off", "0"}) {
        if (same(value, no)) return false;
    }
    return std::nullopt;
}

struct Named {
    Resolution resolution;
    std::string_view name;
    float scale;
};
constexpr std::array<Named, 6> kNamed{{
    {Resolution::kAuto, "auto", 0.0f},
    {Resolution::k720, "1280x720", 1.0f},
    {Resolution::k1080, "1920x1080", 1.5f},
    {Resolution::k1440, "2560x1440", 2.0f},
    {Resolution::k2160, "3840x2160", 3.0f},
    {Resolution::k2880, "5120x2880", 4.0f},
}};

const Named& named(Resolution resolution) {
    for (const Named& n : kNamed) {
        if (n.resolution == resolution) return n;
    }
    return kNamed[0];
}

std::string summary(const Settings& s) {
    return std::string("fullscreen ") + (s.fullscreen ? "yes" : "no") + ", resolution " +
           std::string(name_of(s.resolution)) + ", interpolation " + (s.interpolation ? "yes" : "no") +
           ", frame rate " + frame_rate_name(s.frame_rate);
}

bool set(const char* value) { return value != nullptr && *value != '\0'; }

fs::path home() {
    const char* h = std::getenv("HOME");
    if (h == nullptr || *h == 0) h = std::getenv("USERPROFILE"); // Windows
    return h && *h ? fs::path(h) : fs::path("/");
}

Active g_active;

} // namespace

float scale_of(Resolution resolution) { return named(resolution).scale; }

std::string_view name_of(Resolution resolution) { return named(resolution).name; }

std::optional<Resolution> resolution_named(std::string_view name) {
    for (const Named& n : kNamed) {
        if (same(name, n.name)) return n.resolution;
    }
    return std::nullopt;
}

std::optional<Resolution> resolution_of_scale(std::string_view text) {
    text = trim(text);
    float value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    for (const Named& n : kNamed) {
        if (n.scale > 0 && n.scale == value) return n.resolution;
    }
    return std::nullopt;
}

float auto_scale(uint32_t display_height) {
    return static_cast<float>(std::clamp<uint64_t>((uint64_t{display_height} + 719) / 720, 1, 4));
}

std::pair<int, int> window_size(int desktop_width, int desktop_height, float density, float scale) {
    const int room_w = desktop_width - 64, room_h = desktop_height - 128;
    if (scale > 0 && density > 0) {
        const int w = static_cast<int>(std::lround(1280 * scale / density));
        const int h = static_cast<int>(std::lround(720 * scale / density));
        if (w <= room_w && h <= room_h) return {w, h};
    }
    const int fit = std::max(1, std::min(room_w / 640, room_h / 360));
    return {640 * fit, 360 * fit};
}

Parsed parse(std::string_view text) {
    enum Name : size_t { kFullscreen, kResolution, kInterpolation, kFrameRate, kNames };
    static constexpr std::array<std::string_view, kNames> kIds{"fullscreen", "resolution", "interpolation",
                                                               "frame_rate"};
    Parsed parsed;
    std::array<size_t, kNames> set_on{}; // the line that gave each setting its value
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3); // a byte order mark from an editor
    size_t number = 0;
    while (!text.empty()) {
        ++number;
        const size_t end = std::min(text.find('\n'), text.size());
        const std::string_view line = trim(strip_comment(text.substr(0, end)));
        text.remove_prefix(std::min(end + 1, text.size()));
        if (line.empty()) continue;

        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            parsed.warnings.push_back({number, "not \"name = value\": " + quoted(line)});
            continue;
        }
        const std::string_view id = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        const auto known = std::find(kIds.begin(), kIds.end(), id);
        if (known == kIds.end()) {
            parsed.warnings.push_back({number, "unknown setting " + quoted(id)});
            continue;
        }
        const size_t which = static_cast<size_t>(known - kIds.begin());
        if (which == kResolution) {
            const std::optional<Resolution> resolution = resolution_named(value);
            if (!resolution) {
                parsed.warnings.push_back({number, "resolution: " + quoted(value) +
                                                       " is not auto, 1280x720, 1920x1080, 2560x1440, "
                                                       "3840x2160 or 5120x2880"});
                continue;
            }
            parsed.settings.resolution = *resolution;
        } else if (which == kFrameRate) {
            uint32_t rate = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), rate);
            const bool numeric = !value.empty() && error == std::errc{} && end == value.data() + value.size();
            if (same(value, "uncapped")) {
                rate = kUncapped;
            } else if (!same(value, "auto") && !(numeric && (rate == 60 || rate == 120 || rate == 180 || rate == 240))) {
                parsed.warnings.push_back(
                    {number, "frame_rate: " + quoted(value) + " is not auto, 60, 120, 180, 240 or uncapped"});
                continue;
            }
            parsed.settings.frame_rate = rate;
        } else {
            const std::optional<bool> on = yes_no(value);
            if (!on) {
                parsed.warnings.push_back({number, std::string(id) + ": " + quoted(value) + " is not yes or no"});
                continue;
            }
            (which == kFullscreen ? parsed.settings.fullscreen : parsed.settings.interpolation) = *on;
        }
        // The earlier line is the one left out.
        if (set_on[which] != 0) {
            parsed.warnings.push_back(
                {set_on[which], std::string(id) + ": given again on line " + std::to_string(number)});
        } else {
            ++parsed.given;
        }
        set_on[which] = number;
    }
    std::stable_sort(parsed.warnings.begin(), parsed.warnings.end(),
                     [](const Warning& a, const Warning& b) { return a.line < b.line; });
    return parsed;
}

namespace {
fs::path g_file;                                   // set_file's, else the XDG path
std::string g_game = "Tekken Tag Tournament 2"; // in the file's header
} // namespace

void set_file(const fs::path& file, const std::string& game) {
    g_file = file;
    g_game = game;
}

std::string format(const Settings& settings) {
    std::string text = "# " + g_game + "'s display settings, written by the launcher's Settings.\n"
                       "# The game reads them when it starts.\n";
    text += std::string("fullscreen = ") + (settings.fullscreen ? "yes" : "no") +
            "  # yes or no (F11 switches in the game)\n";
    text += "resolution = " + std::string(name_of(settings.resolution)) +
            "  # auto, 1280x720, 1920x1080, 2560x1440, 3840x2160 or 5120x2880\n";
    text += std::string("interpolation = ") + (settings.interpolation ? "yes" : "no") +
            "  # yes: 120 frames per second on displays of 100 Hz and more\n";
    text += "frame_rate = " + frame_rate_name(settings.frame_rate) +
            "  # auto, 60, 120, 180, 240 or uncapped (with interpolation; above the display's refresh it tears)\n";
    return text;
}

std::string frame_rate_name(uint32_t frame_rate) {
    if (frame_rate == 0) return "auto";
    if (frame_rate == kUncapped) return "uncapped";
    return std::to_string(frame_rate);
}

fs::path file_path() {
    if (!g_file.empty()) return g_file;
    const char* config = std::getenv("XDG_CONFIG_HOME");
    return (config && *config == '/' ? fs::path(config) : home() / ".config") / "ttt2" / "display.ini";
}

Loaded load(const fs::path& file) {
    Loaded loaded;
    const std::string name = file.string();
    const auto unusable = [&](const std::string& why) {
        loaded.error = why;
        loaded.log.push_back("display: defaults (" + why + ")");
        return loaded;
    };
    host::FileStat st{};
    if (host::file_stat(file, st) != 0) {
        const int error = errno;
        if (error == ENOENT || error == ENOTDIR) {
            loaded.log.push_back("display: defaults (no " + name + ")");
            return loaded;
        }
        return unusable("cannot read " + name + ": " + std::strerror(error));
    }
    // Never open a pipe or a device: that could wait forever.
    if (!S_ISREG(st.st_mode)) return unusable(name + " is not a file");
    if (static_cast<uint64_t>(st.st_size) > kMaxFileSize) return unusable(name + " is larger than 64 KiB");
    std::ifstream in(file, std::ios::binary);
    std::string text;
    try {
        if (in) text.assign(std::istreambuf_iterator<char>(in), {});
    } catch (const std::ios_base::failure& e) {
        // libstdc++'s file buffer throws on a failed read (EIO: a failing
        // disk, a network home gone) whatever the stream's exception mask.
        return unusable("cannot read " + name + ": " + e.code().message());
    }
    if (!in.is_open() || in.bad()) {
        const int error = errno;
        return unusable("cannot read " + name + ": " + std::strerror(error));
    }
    const Parsed parsed = parse(text);
    for (const Warning& warning : parsed.warnings) {
        loaded.log.push_back("display: " + name + ":" + std::to_string(warning.line) + ": " + warning.text +
                             ", line ignored");
    }
    loaded.settings = parsed.settings;
    loaded.log.push_back("display: " + summary(parsed.settings) + " (from " + name + ")");
    return loaded;
}

Active resolve(const Settings& settings, const char* fullscreen, const char* scale, const char* fps,
               std::vector<std::string>& log) {
    Active active{settings.fullscreen, scale_of(settings.resolution), settings.interpolation ? settings.frame_rate : 60u};
    if (set(fullscreen)) {
        active.fullscreen = std::string_view(fullscreen) == "1";
        log.push_back(std::string("display: TTT2_FULLSCREEN=") + fullscreen + ": " +
                      (active.fullscreen ? "fullscreen" : "windowed"));
    }
    if (set(scale)) {
        if (const std::optional<Resolution> resolution = resolution_of_scale(scale)) {
            active.scale = scale_of(*resolution);
            log.push_back(std::string("display: TTT2_SCALE=") + scale + ": " + std::string(name_of(*resolution)));
        } else {
            log.push_back(std::string("display: TTT2_SCALE=") + scale + " is not 1, 1.5, 2, 3 or 4, ignored");
        }
    }
    if (set(fps)) {
        const std::string_view text = fps;
        unsigned value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (same(text, "uncapped")) {
            active.fps = kUncapped;
            log.push_back(std::string("display: TTT2_FPS=") + fps);
        } else if (error == std::errc{} && end == text.data() + text.size() && value > 0) {
            active.fps = value;
            log.push_back(std::string("display: TTT2_FPS=") + fps);
        } else {
            log.push_back(std::string("display: TTT2_FPS=") + fps + " is not a frame rate, ignored");
        }
    }
    return active;
}

const Active& active() { return g_active; }

void set_active(const Active& active) { g_active = active; }

} // namespace cafe::host::display
