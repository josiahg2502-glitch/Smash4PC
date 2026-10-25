// The display settings from display.ini (runtime/src/host/display_settings.h):
// what a file says and what is ignored, the text the launcher writes, where
// the file is, the environment's values over the file's, the automatic scale
// and the window's size. Files go to a scratch folder.

#include "host/display_settings.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace display = cafe::host::display;
using display::Resolution;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

bool has_warning(const display::Parsed& parsed, size_t line, const std::string& part) {
    for (const display::Warning& w : parsed.warnings) {
        if (w.line == line && w.text.find(part) != std::string::npos) return true;
    }
    return false;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const std::string& line : lines) all += line + "\n";
    return all;
}

std::string text_of(const display::Settings& s) {
    return std::string(s.fullscreen ? "fullscreen" : "windowed") + ", " + std::string(display::name_of(s.resolution)) +
           (s.interpolation ? ", interpolation" : ", no interpolation");
}

std::string text_of(const display::Active& a) {
    char text[64];
    std::snprintf(text, sizeof(text), "%s, scale %g, %u fps", a.fullscreen ? "fullscreen" : "windowed",
                  static_cast<double>(a.scale), static_cast<unsigned>(a.fps));
    return text;
}

std::string text_of(std::pair<int, int> size) { return std::to_string(size.first) + "x" + std::to_string(size.second); }

void write(const fs::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

void test_resolutions() {
    const struct {
        Resolution resolution;
        const char* name;
        float scale;
    } expected[] = {
        {Resolution::kAuto, "auto", 0.0f},      {Resolution::k720, "1280x720", 1.0f},
        {Resolution::k1080, "1920x1080", 1.5f}, {Resolution::k1440, "2560x1440", 2.0f},
        {Resolution::k2160, "3840x2160", 3.0f}, {Resolution::k2880, "5120x2880", 4.0f},
    };
    check(std::size(display::kResolutions) == std::size(expected), "six resolutions");
    for (size_t i = 0; i < std::size(expected) && i < std::size(display::kResolutions); ++i) {
        const Resolution r = display::kResolutions[i];
        const std::string what = std::string("resolution ") + expected[i].name;
        check(r == expected[i].resolution, what + ": in the launcher's order");
        check(display::name_of(r) == expected[i].name, what + ": name");
        check(display::scale_of(r) == expected[i].scale, what + ": scale");
        check(display::resolution_named(expected[i].name) == r, what + ": named");
    }

    // Letters in either case; nothing else is a resolution.
    check(display::resolution_named("AUTO") == Resolution::kAuto && display::resolution_named("Auto") ==
              Resolution::kAuto && display::resolution_named("1920X1080") == Resolution::k1080,
          "resolution names in either case");
    for (const char* name : {"", "native", "1366x768", "1920 x 1080", "1920*1080", "1080p", "1920x1080p", "1.5",
                             "2", "1920x1080x", "x1920x1080"}) {
        check(!display::resolution_named(name), std::string("no resolution named \"") + name + "\"");
    }

    // TTT2_SCALE's values.
    const struct {
        const char* text;
        Resolution resolution;
    } scales[] = {
        {"1", Resolution::k720},     {"1.5", Resolution::k1080},  {"2", Resolution::k1440},
        {"3", Resolution::k2160},    {"4", Resolution::k2880},    {"2.0", Resolution::k1440},
        {"1.50", Resolution::k1080}, {"4.0", Resolution::k2880},  {"1.0", Resolution::k720},
    };
    for (const auto& s : scales) {
        check(display::resolution_of_scale(s.text) == s.resolution, std::string("scale \"") + s.text + "\"");
    }
    for (const char* text : {"", "0", "0.0", "0.5", "1.25", "2.5", "5", "8", "16", "-1", "-2", "1,5", "two", "2x",
                             "x2", "1.5.0", "nan", "inf", "auto", "1280x720"}) {
        check(!display::resolution_of_scale(text), std::string("no scale \"") + text + "\"");
    }
}

void test_auto_scale() {
    // Enough to cover the display: 1080p and 1440p get 2560x1440, 4K
    // 3840x2160, at most 5120x2880.
    const struct {
        uint32_t height;
        float scale;
    } expected[] = {
        {0, 1},    {480, 1},  {720, 1},  {721, 2},  {768, 2},  {800, 2},  {1080, 2},
        {1200, 2}, {1440, 2}, {1441, 3}, {1600, 3}, {2160, 3}, {2161, 4}, {2880, 4},
        {4320, 4}, {std::numeric_limits<uint32_t>::max(), 4},
    };
    for (const auto& e : expected) {
        const float scale = display::auto_scale(e.height);
        check(scale == e.scale, "auto scale on " + std::to_string(e.height) + " pixels: " + std::to_string(scale));
    }
}

void test_window_size() {
    const struct {
        const char* what;
        int width, height;
        float density, scale;
        std::pair<int, int> size;
    } expected[] = {
        // 1080p: only 1280x720 fits with room to spare.
        {"1080p, automatic", 1920, 1080, 1.0f, 0.0f, {1280, 720}},
        {"1080p, 1280x720", 1920, 1080, 1.0f, 1.0f, {1280, 720}},
        {"1080p, 1920x1080 does not fit", 1920, 1080, 1.0f, 1.5f, {1280, 720}},
        {"1080p, 2560x1440 does not fit", 1920, 1080, 1.0f, 2.0f, {1280, 720}},
        // 1440p at 125 %: the desktop is 2048x1152 units.
        {"1440p at 1.25, automatic", 2048, 1152, 1.25f, 0.0f, {1280, 720}},
        {"1440p at 1.25, 1280x720", 2048, 1152, 1.25f, 1.0f, {1024, 576}},
        {"1440p at 1.25, 1920x1080", 2048, 1152, 1.25f, 1.5f, {1536, 864}},
        {"1440p at 1.25, 2560x1440 does not fit", 2048, 1152, 1.25f, 2.0f, {1280, 720}},
        {"1440p, automatic", 2560, 1440, 1.0f, 0.0f, {1920, 1080}},
        {"1440p, 1920x1080", 2560, 1440, 1.0f, 1.5f, {1920, 1080}},
        {"1440p, 1280x720", 2560, 1440, 1.0f, 1.0f, {1280, 720}},
        {"1440p, 2560x1440 does not fit", 2560, 1440, 1.0f, 2.0f, {1920, 1080}},
        // 4K: today's rule leaves 3200x1800.
        {"4K, automatic", 3840, 2160, 1.0f, 0.0f, {3200, 1800}},
        {"4K, 2560x1440", 3840, 2160, 1.0f, 2.0f, {2560, 1440}},
        {"4K, 3840x2160 does not fit", 3840, 2160, 1.0f, 3.0f, {3200, 1800}},
        {"4K, 5120x2880 does not fit", 3840, 2160, 1.0f, 4.0f, {3200, 1800}},
        {"4K at 2, automatic", 1920, 1080, 2.0f, 0.0f, {1280, 720}},
        {"4K at 2, 1280x720", 1920, 1080, 2.0f, 1.0f, {640, 360}},
        {"4K at 2, 2560x1440", 1920, 1080, 2.0f, 2.0f, {1280, 720}},
        {"4K at 2, 3840x2160 does not fit", 1920, 1080, 2.0f, 3.0f, {1280, 720}},
        // Exactly 64 units across and 128 down to spare, and one less (at 1.25:
        // the multiples of 640x360 are other sizes).
        {"room to spare, exactly", 1600, 992, 1.25f, 1.5f, {1536, 864}},
        {"one unit too narrow", 1599, 992, 1.25f, 1.5f, {1280, 720}},
        {"one unit too low", 1600, 991, 1.25f, 1.5f, {1280, 720}},
        // A small desktop still gets the smallest multiple.
        {"1366x768", 1366, 768, 1.0f, 0.0f, {640, 360}},
        {"no desktop", 0, 0, 1.0f, 0.0f, {640, 360}},
        {"no density", 3840, 2160, 0.0f, 2.0f, {3200, 1800}},
    };
    for (const auto& e : expected) {
        const std::pair<int, int> size = display::window_size(e.width, e.height, e.density, e.scale);
        check(size == e.size, std::string("window on ") + e.what + ": " + text_of(size));
    }
}

void test_parse_good() {
    const display::Parsed parsed = display::parse("# my settings\n"
                                                  "fullscreen = yes\n"
                                                  "\n"
                                                  "resolution=1920x1080   # comment\n"
                                                  "\tinterpolation\t=\tno\t\n");
    check(parsed.warnings.empty(), "good file: no warnings");
    check(parsed.given == 3, "good file: 3 given");
    check(parsed.settings == display::Settings{true, Resolution::k1080, false},
          "good file: " + text_of(parsed.settings));

    // Every value, in either case.
    const struct {
        const char* value;
        bool on;
    } values[] = {
        {"yes", true},  {"true", true},   {"on", true},   {"1", true},   {"YES", true},  {"True", true},
        {"On", true},   {"no", false},    {"false", false}, {"off", false}, {"0", false}, {"NO", false},
        {"FALSE", false}, {"Off", false},
    };
    for (const auto& v : values) {
        const display::Parsed full = display::parse(std::string("fullscreen = ") + v.value + "\n");
        check(full.warnings.empty() && full.given == 1 && full.settings.fullscreen == v.on &&
                  full.settings.resolution == Resolution::kAuto && full.settings.interpolation,
              std::string("fullscreen = ") + v.value);
        const display::Parsed interpolation = display::parse(std::string("interpolation = ") + v.value + "\n");
        check(interpolation.warnings.empty() && interpolation.given == 1 &&
                  interpolation.settings.interpolation == v.on && !interpolation.settings.fullscreen &&
                  interpolation.settings.resolution == Resolution::kAuto,
              std::string("interpolation = ") + v.value);
    }
    for (const Resolution r : display::kResolutions) {
        std::string name(display::name_of(r));
        for (int upper = 0; upper < 2; ++upper) {
            if (upper) {
                for (char& c : name) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
            }
            const display::Parsed one = display::parse("resolution = " + name + "\n");
            check(one.warnings.empty() && one.given == 1 && one.settings.resolution == r &&
                      !one.settings.fullscreen && one.settings.interpolation,
                  "resolution = " + name);
        }
    }

    for (const char* empty : {"", "\n\n", "# nothing\n   \n\t# else\n"}) {
        const display::Parsed nothing = display::parse(empty);
        check(nothing.warnings.empty() && nothing.given == 0 && nothing.settings == display::Settings{},
              "no settings: defaults");
    }
    const display::Parsed windows = display::parse("\xEF\xBB\xBF"
                                                   "fullscreen = yes\r\n"
                                                   "\r\n"
                                                   "resolution = 3840x2160 # 4K\r\n");
    check(windows.warnings.empty() && windows.given == 2 &&
              windows.settings == display::Settings{true, Resolution::k2160, true},
          "byte order mark and CRLF: " + text_of(windows.settings));
    const display::Parsed last = display::parse("interpolation = off");
    check(last.warnings.empty() && last.given == 1 && !last.settings.interpolation, "no newline at the end");
}

void test_comments() {
    const display::Parsed parsed = display::parse("#fullscreen = yes\n"                 // 1
                                                  "   # resolution = 1280x720\n"        // 2
                                                  "fullscreen = yes#\n"                 // 3
                                                  "resolution = 1280x720 #1920x1080\n"  // 4
                                                  "interpolation = # no\n"              // 5
                                                  "interpolation\t# = no\n");           // 6
    check(!parsed.settings.fullscreen, "commented-out line");
    check(has_warning(parsed, 3, "fullscreen: \"yes#\" is not yes or no"), "# inside a word is no comment");
    check(parsed.settings.resolution == Resolution::k720, "comment after a space");
    check(has_warning(parsed, 5, "interpolation: \"\" is not yes or no") && parsed.settings.interpolation,
          "only a comment as value");
    check(has_warning(parsed, 6, "not \"name = value\": \"interpolation\""), "the = inside a comment");
    check(parsed.given == 1 && parsed.warnings.size() == 3, "comments: counts " + std::to_string(parsed.given) +
                                                                " given, " + std::to_string(parsed.warnings.size()) +
                                                                " warnings");
}

void test_bad() {
    const display::Parsed parsed = display::parse("fullscreen yes\n"                // 1: no =
                                                  "= yes\n"                         // 2: no name
                                                  "vsync = yes\n"                   // 3: unknown setting
                                                  "Fullscreen = yes\n"              // 4: names are exact
                                                  "RESOLUTION = 1920x1080\n"        // 5
                                                  "fullscreen = maybe\n"            // 6
                                                  "fullscreen = 2\n"                // 7
                                                  "fullscreen = y\n"                // 8
                                                  "interpolation =\n"               // 9
                                                  "interpolation = yes please\n"    // 10
                                                  "resolution = 1366x768\n"         // 11
                                                  "resolution = 1920 x 1080\n"      // 12
                                                  "resolution = 1080p\n"            // 13
                                                  "resolution = 1.5\n"              // 14: a scale, not a size
                                                  "resolution = 1920x1080 = big\n"  // 15
                                                  "resolution =\n");                // 16
    check(has_warning(parsed, 1, "not \"name = value\": \"fullscreen yes\""), "line without =");
    check(has_warning(parsed, 2, "unknown setting \"\""), "line without a name");
    check(has_warning(parsed, 3, "unknown setting \"vsync\""), "unknown setting");
    check(has_warning(parsed, 4, "unknown setting \"Fullscreen\"") &&
              has_warning(parsed, 5, "unknown setting \"RESOLUTION\""),
          "names are case-sensitive");
    check(has_warning(parsed, 6, "fullscreen: \"maybe\" is not yes or no"), "maybe");
    check(has_warning(parsed, 7, "fullscreen: \"2\" is not yes or no"), "2");
    check(has_warning(parsed, 8, "fullscreen: \"y\" is not yes or no"), "y");
    check(has_warning(parsed, 9, "interpolation: \"\" is not yes or no"), "no value");
    check(has_warning(parsed, 10, "interpolation: \"yes please\" is not yes or no"), "two words");
    for (const auto& [line, value] : std::vector<std::pair<size_t, std::string>>{
             {11, "1366x768"}, {12, "1920 x 1080"}, {13, "1080p"}, {14, "1.5"}, {15, "1920x1080 = big"}, {16, ""}}) {
        check(has_warning(parsed, line, "resolution: \"" + value + "\" is not auto, 1280x720, 1920x1080, 2560x1440, "
                                                                   "3840x2160 or 5120x2880"),
              "resolution \"" + value + "\"");
    }
    check(parsed.warnings.size() == 16, "bad file: one warning per bad line, " +
                                            std::to_string(parsed.warnings.size()));
    check(parsed.settings == display::Settings{} && parsed.given == 0, "bad lines keep the defaults");
}

void test_duplicates() {
    const display::Parsed twice = display::parse("fullscreen = yes\nresolution = auto\nfullscreen = no\n");
    check(!twice.settings.fullscreen, "a setting given twice takes the later value");
    check(twice.given == 2, "a setting given twice counts once");
    check(twice.warnings.size() == 1 && has_warning(twice, 1, "fullscreen: given again on line 3"),
          "a setting given twice is told on the earlier line");

    const display::Parsed thrice = display::parse("resolution = 1280x720\nresolution = 1920x1080\n"
                                                  "resolution = 2560x1440\n");
    check(thrice.settings.resolution == Resolution::k1440 && thrice.given == 1, "three times: the last value");
    check(thrice.warnings.size() == 2 && has_warning(thrice, 1, "resolution: given again on line 2") &&
              has_warning(thrice, 2, "resolution: given again on line 3"),
          "three times: the two earlier lines are told");

    const display::Parsed bad_again = display::parse("fullscreen = yes\nfullscreen = nope\n");
    check(bad_again.settings.fullscreen && bad_again.given == 1, "a bad repeat keeps the good value");
    check(bad_again.warnings.size() == 1 && has_warning(bad_again, 2, "is not yes or no"),
          "a bad repeat: only its own warning");
    const display::Parsed bad_first = display::parse("fullscreen = nope\nfullscreen = yes\n");
    check(bad_first.settings.fullscreen && bad_first.given == 1 && bad_first.warnings.size() == 1 &&
              has_warning(bad_first, 1, "is not yes or no"),
          "a bad line, then a good one");

    const display::Parsed order = display::parse("interpolation = no\nvsync = 1\ninterpolation = yes\n");
    check(order.warnings.size() == 2 && order.warnings[0].line == 1 && order.warnings[1].line == 2,
          "warnings in line order");
}

void test_format() {
    const std::string text = display::format(display::Settings{});
    std::vector<std::string> lines;
    for (size_t start = 0; start < text.size();) {
        const size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end - start));
        start = end == std::string::npos ? text.size() : end + 1;
    }
    check(!text.empty() && text.back() == '\n', "the text ends with a newline");
    check(lines.size() == 6, "a header of two lines and four settings: " + std::to_string(lines.size()) + " lines");
    if (lines.size() == 6) {
        check(lines[0].starts_with("# ") && lines[1].starts_with("# "), "the header is a comment");
        check(text.find("Tekken Tag Tournament 2") != std::string::npos && text.find("ttt2") == std::string::npos,
              "the header names the game as players know it");
        check(lines[2].starts_with("fullscreen = no  # "), "fullscreen line: " + lines[2]);
        check(lines[3].starts_with("resolution = auto  # "), "resolution line: " + lines[3]);
        check(lines[4].starts_with("interpolation = yes  # "), "interpolation line: " + lines[4]);
        check(lines[5].starts_with("frame_rate = auto  # "), "frame rate line: " + lines[5]);
    }

    // Everything the launcher can write is read back as it was meant.
    for (const bool fullscreen : {false, true}) {
        for (const Resolution resolution : display::kResolutions) {
            for (const bool interpolation : {false, true}) {
                for (const uint32_t rate : {0u, 60u, 120u, 180u, 240u}) {
                    const display::Settings settings{fullscreen, resolution, interpolation, rate};
                    const display::Parsed parsed = display::parse(display::format(settings));
                    check(parsed.settings == settings && parsed.given == 4 && parsed.warnings.empty(),
                          "round trip of " + text_of(settings) + ", frame rate " + std::to_string(rate));
                }
            }
        }
    }
}

void test_frame_rate() {
    check(display::parse("frame_rate = 120").settings.frame_rate == 120, "frame_rate 120");
    check(display::parse("frame_rate = AUTO").settings.frame_rate == 0, "frame_rate auto");
    const display::Parsed bad = display::parse("frame_rate = 90");
    check(bad.warnings.size() == 1 && bad.settings.frame_rate == 0, "frame_rate 90 is left out");
    std::vector<std::string> log;
    check(display::resolve(display::Settings{false, Resolution::kAuto, true, 240}, nullptr, nullptr, nullptr, log).fps == 240,
          "the frame rate with interpolation");
    check(display::resolve(display::Settings{false, Resolution::kAuto, false, 240}, nullptr, nullptr, nullptr, log).fps == 60,
          "no interpolation is 60");
}

void test_path(const fs::path& scratch) {
    const char* old_home = std::getenv("HOME");
    const char* old_config = std::getenv("XDG_CONFIG_HOME");
    const std::string home = old_home ? old_home : "", config = old_config ? old_config : "";

    setenv("HOME", (scratch / "home").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (scratch / "config").c_str(), 1);
    check(display::file_path() == scratch / "config/ttt2/display.ini", "XDG_CONFIG_HOME");
    setenv("XDG_CONFIG_HOME", "relative/config", 1);
    check(display::file_path() == scratch / "home/.config/ttt2/display.ini", "a relative XDG_CONFIG_HOME is ignored");
    setenv("XDG_CONFIG_HOME", "", 1);
    check(display::file_path() == scratch / "home/.config/ttt2/display.ini", "an empty XDG_CONFIG_HOME is ignored");
    unsetenv("XDG_CONFIG_HOME");
    check(display::file_path() == scratch / "home/.config/ttt2/display.ini", "no XDG_CONFIG_HOME");
    unsetenv("HOME");
    check(display::file_path() == "/.config/ttt2/display.ini", "no HOME either: " + display::file_path().string());

    if (old_home) setenv("HOME", home.c_str(), 1);
    if (old_config) setenv("XDG_CONFIG_HOME", config.c_str(), 1);
}

void test_load(const fs::path& scratch) {
    const fs::path file = scratch / "display.ini";
    const std::string name = file.string();
    const std::string defaults = "fullscreen no, resolution auto, interpolation yes";
    fs::remove(file);
    const display::Loaded missing = display::load(file);
    check(missing.settings == display::Settings{} && missing.error.empty(), "missing file: defaults, no error");
    check(missing.log == std::vector<std::string>{"display: defaults (no " + name + ")"},
          "missing file: log " + joined(missing.log));
    const fs::path not_folder = scratch / "plain";
    write(not_folder, "");
    const fs::path under = not_folder / "display.ini";
    const display::Loaded under_file = display::load(under);
    check(under_file.settings == display::Settings{} && under_file.error.empty() &&
              under_file.log == std::vector<std::string>{"display: defaults (no " + under.string() + ")"},
          "a file where the folder should be: missing " + joined(under_file.log));

    write(file, "fullscreen = on\nvsync = yes\nresolution = 2560x1440\n");
    const display::Loaded loaded = display::load(file);
    check(loaded.settings == display::Settings{true, Resolution::k1440, true} && loaded.error.empty(),
          "file: " + text_of(loaded.settings));
    const std::vector<std::string> log{
        "display: " + name + ":2: unknown setting \"vsync\", line ignored",
        "display: fullscreen yes, resolution 2560x1440, interpolation yes (from " + name + ")"};
    check(loaded.log == log, "file: log " + joined(loaded.log));

    const display::Settings written{true, Resolution::k1080, false};
    write(file, display::format(written));
    const display::Loaded launcher = display::load(file);
    check(launcher.settings == written && launcher.error.empty() &&
              launcher.log == std::vector<std::string>{"display: fullscreen yes, resolution 1920x1080, "
                                                       "interpolation no (from " + name + ")"},
          "the launcher's file: " + joined(launcher.log));

    const fs::path link = scratch / "link.ini";
    fs::create_symlink(file, link);
    check(display::load(link).settings == written, "a link to the file");

    write(file, "");
    const display::Loaded empty = display::load(file);
    check(empty.settings == display::Settings{} && empty.error.empty() &&
              empty.log == std::vector<std::string>{"display: " + defaults + " (from " + name + ")"},
          "empty file: " + joined(empty.log));

    // Present but never opened: a folder, a pipe (reading one would wait for
    // a writer), a file too large to be settings. The error says why, and the
    // log has the same in one line.
    const auto refused = [](const display::Loaded& l, const std::string& why, const std::string& what) {
        check(l.settings == display::Settings{} && l.error.find(why) != std::string::npos &&
                  l.log == std::vector<std::string>{"display: defaults (" + l.error + ")"},
              what + ": error \"" + l.error + "\", log " + joined(l.log));
    };
    const fs::path folder = scratch / "folder.ini";
    fs::create_directories(folder);
    refused(display::load(folder), folder.string() + " is not a file", "a folder");
    const fs::path pipe = scratch / "pipe.ini";
    fs::remove(pipe);
    if (mkfifo(pipe.c_str(), 0600) == 0) {
        refused(display::load(pipe), pipe.string() + " is not a file", "a pipe");
        fs::remove(pipe);
    } else {
        check(false, std::string("cannot make a pipe: ") + std::strerror(errno));
    }

    // 64 KiB is still read, a byte more is not.
    const std::string first = "fullscreen = yes\n#";
    write(file, first + std::string(64 * 1024 - first.size(), '#'));
    const display::Loaded largest = display::load(file);
    check(largest.settings.fullscreen && largest.error.empty(), "a file of 64 KiB: " + joined(largest.log));
    write(file, first + std::string(64 * 1024 + 1 - first.size(), '#'));
    refused(display::load(file), name + " is larger than 64 KiB", "a file over 64 KiB");

    // Unreadable: the file itself, or the folder it is in. Root reads them
    // anyway.
    const fs::path unreadable = scratch / "unreadable.ini";
    write(unreadable, "fullscreen = yes\n");
    fs::permissions(unreadable, fs::perms::none);
    const fs::path locked = scratch / "locked";
    fs::create_directories(locked);
    write(locked / "display.ini", "fullscreen = yes\n");
    fs::permissions(locked, fs::perms::none);
    if (getuid() != 0) {
        const std::string denied = std::strerror(EACCES);
        refused(display::load(unreadable), "cannot read " + unreadable.string() + ": " + denied, "an unreadable file");
        const fs::path inside = locked / "display.ini";
        refused(display::load(inside), "cannot read " + inside.string() + ": " + denied,
                "a file in an unreadable folder");
    }
    fs::permissions(locked, fs::perms::owner_all);
    fs::permissions(unreadable, fs::perms::owner_read | fs::perms::owner_write);

    // Opened, but the read fails (EIO, as from a failing disk): Linux's
    // /proc/self/mem is a regular file whose first bytes are never mapped.
    if (std::ifstream("/proc/self/mem").is_open()) {
        refused(display::load("/proc/self/mem"), std::string("cannot read /proc/self/mem: ") + std::strerror(EIO),
                "a failed read");
    }
}

void test_resolve() {
    const display::Settings windowed{};
    const display::Settings chosen{true, Resolution::k1080, false};
    std::vector<std::string> log;
    const auto resolved = [&](const display::Settings& s, const char* fullscreen, const char* scale, const char* fps) {
        log.clear();
        return display::resolve(s, fullscreen, scale, fps, log);
    };

    // The file's settings when the environment says nothing.
    display::Active active = resolved(windowed, nullptr, nullptr, nullptr);
    check(active == display::Active{false, 0.0f, 0} && log.empty(), "defaults: " + text_of(active));
    active = resolved(chosen, nullptr, nullptr, nullptr);
    check(active == display::Active{true, 1.5f, 60} && log.empty(), "the file's settings: " + text_of(active));
    for (const Resolution r : display::kResolutions) {
        active = resolved({false, r, true}, nullptr, nullptr, nullptr);
        check(active == display::Active{false, display::scale_of(r), 0},
              "resolution " + std::string(display::name_of(r)) + ": " + text_of(active));
    }
    active = resolved(chosen, "", "", "");
    check(active == display::Active{true, 1.5f, 60} && log.empty(), "empty values are not set: " + text_of(active));

    // TTT2_FULLSCREEN: 1 is fullscreen, anything else windowed.
    active = resolved(windowed, "1", nullptr, nullptr);
    check(active == display::Active{true, 0.0f, 0} &&
              log == std::vector<std::string>{"display: TTT2_FULLSCREEN=1: fullscreen"},
          "TTT2_FULLSCREEN=1: " + text_of(active) + ", " + joined(log));
    active = resolved(chosen, "0", nullptr, nullptr);
    check(active == display::Active{false, 1.5f, 60} &&
              log == std::vector<std::string>{"display: TTT2_FULLSCREEN=0: windowed"},
          "TTT2_FULLSCREEN=0: " + text_of(active) + ", " + joined(log));
    for (const char* other : {"yes", "true", "2", "10", "1 ", " 1"}) {
        active = resolved(chosen, other, nullptr, nullptr);
        check(!active.fullscreen && log.size() == 1 && log[0].ends_with(": windowed"),
              std::string("TTT2_FULLSCREEN=") + other + ": " + text_of(active) + ", " + joined(log));
    }

    // TTT2_SCALE: 1, 1.5, 2, 3 or 4; other values leave the file's.
    active = resolved(windowed, nullptr, "1.5", nullptr);
    check(active == display::Active{false, 1.5f, 0} &&
              log == std::vector<std::string>{"display: TTT2_SCALE=1.5: 1920x1080"},
          "TTT2_SCALE=1.5: " + text_of(active) + ", " + joined(log));
    active = resolved({false, Resolution::k2880, true}, nullptr, "1", nullptr);
    check(active.scale == 1.0f && log == std::vector<std::string>{"display: TTT2_SCALE=1: 1280x720"},
          "TTT2_SCALE=1 over 5120x2880: " + text_of(active));
    active = resolved(windowed, nullptr, "2.0", nullptr);
    check(active.scale == 2.0f, "TTT2_SCALE=2.0: " + text_of(active));
    for (const char* bad : {"0", "5", "2.5", "-1", "abc", "1,5"}) {
        active = resolved(chosen, nullptr, bad, nullptr);
        check(active == display::Active{true, 1.5f, 60} &&
                  log == std::vector<std::string>{std::string("display: TTT2_SCALE=") + bad +
                                                  " is not 1, 1.5, 2, 3 or 4, ignored"},
              std::string("TTT2_SCALE=") + bad + ": " + text_of(active) + ", " + joined(log));
        active = resolved(windowed, nullptr, bad, nullptr);
        check(active.scale == 0.0f, std::string("TTT2_SCALE=") + bad + " stays automatic: " + text_of(active));
    }

    // TTT2_FPS: any rate, over interpolation either way.
    active = resolved(chosen, nullptr, nullptr, "120");
    check(active == display::Active{true, 1.5f, 120} && log == std::vector<std::string>{"display: TTT2_FPS=120"},
          "TTT2_FPS=120 over interpolation = no: " + text_of(active) + ", " + joined(log));
    active = resolved(windowed, nullptr, nullptr, "60");
    check(active == display::Active{false, 0.0f, 60} && log == std::vector<std::string>{"display: TTT2_FPS=60"},
          "TTT2_FPS=60 over interpolation = yes: " + text_of(active));
    for (const uint32_t fps : {90u, 180u, 240u}) {
        active = resolved(windowed, nullptr, nullptr, std::to_string(fps).c_str());
        check(active.fps == fps, "TTT2_FPS=" + std::to_string(fps) + ": " + text_of(active));
    }
    for (const char* bad : {"0", "-60", "abc", "120fps", "1e2", "12.5", "99999999999"}) {
        active = resolved(chosen, nullptr, nullptr, bad);
        check(active.fps == 60 && log == std::vector<std::string>{std::string("display: TTT2_FPS=") + bad +
                                                                  " is not a frame rate, ignored"},
              std::string("TTT2_FPS=") + bad + " over interpolation = no: " + text_of(active) + ", " + joined(log));
        active = resolved(windowed, nullptr, nullptr, bad);
        check(active.fps == 0, std::string("TTT2_FPS=") + bad + " over interpolation = yes: " + text_of(active));
    }

    // All three, in that order in the log, after what it had.
    log = {"display: earlier"};
    active = display::resolve({false, Resolution::k720, false}, "1", "3", "240", log);
    check(active == display::Active{true, 3.0f, 240}, "all three: " + text_of(active));
    check(log == std::vector<std::string>{"display: earlier", "display: TTT2_FULLSCREEN=1: fullscreen",
                                          "display: TTT2_SCALE=3: 3840x2160", "display: TTT2_FPS=240"},
          "all three: log " + joined(log));
    active = resolved({true, Resolution::k1080, true}, "0", "8", "fast");
    check(active == display::Active{false, 1.5f, 0} && log.size() == 3, "all three, two of them bad: " +
                                                                           text_of(active) + ", " + joined(log));
}

void test_active() {
    check(display::active() == display::Active{}, "before the window sets it: defaults");
    display::set_active({true, 1.5f, 120});
    check(display::active() == display::Active{true, 1.5f, 120}, "what the window set");
    display::set_active({});
    check(display::active() == display::Active{}, "set again");
}

} // namespace

int main(int argc, char** argv) {
    // The scratch folder: the build tree's, never the player's config.
    const fs::path scratch = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("display_settings_test.d"));
    // A run stopped inside test_load leaves a folder nobody can enter.
    std::error_code ignored;
    fs::permissions(scratch / "locked", fs::perms::owner_all, ignored);
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    test_active();
    test_resolutions();
    test_auto_scale();
    test_window_size();
    test_parse_good();
    test_comments();
    test_bad();
    test_duplicates();
    test_format();
    test_frame_rate();
    test_path(scratch);
    test_load(scratch);
    test_resolve();

    fs::remove_all(scratch);
    if (g_failures == 0) std::printf("display_settings_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
