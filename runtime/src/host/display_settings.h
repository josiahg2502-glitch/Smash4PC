#pragma once

// The player's display settings: $XDG_CONFIG_HOME/ttt2/display.ini, which
// the launcher's Settings write and the game reads when it starts (the
// render size and the frame rate are fixed for a run). Lines are
// `name = value`, `#` starts a comment (at the start of a line or after a
// space):
//
//   fullscreen    = yes | no     start fullscreen (F11 still switches); no
//   resolution    = auto | 1280x720 | 1920x1080 | 2560x1440 | 3840x2160 |
//                   5120x2880    the size the game draws at; auto
//   interpolation = yes | no     120 frames per second on displays of 100 Hz
//                                and more (60 below); yes
//   frame_rate    = auto | 60 | 120 | 180 | 240 | uncapped   with
//                                interpolation, the frames per second (above
//                                the display's refresh it tears); uncapped:
//                                as many as the GPU renders, without vsync;
//                                auto
//
// The environment's TTT2_FULLSCREEN, TTT2_SCALE and TTT2_FPS still win.
// No SDL here: the launcher builds this file too, so that what it writes
// is what the game reads.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cafe::host::display {

// The render resolution: the title's 1280x720 times a scale.
enum class Resolution : uint8_t { kAuto, k720, k1080, k1440, k2160, k2880 };
inline constexpr Resolution kResolutions[] = {Resolution::kAuto,  Resolution::k720,  Resolution::k1080,
                                              Resolution::k1440, Resolution::k2160, Resolution::k2880};

struct Settings {
    bool fullscreen = false;
    Resolution resolution = Resolution::kAuto;
    bool interpolation = true;
    uint32_t frame_rate = 0; // 0: auto, kUncapped: uncapped
    bool operator==(const Settings&) const = default;
};

// frame_rate (and Active::fps) for uncapped.
inline constexpr uint32_t kUncapped = 1000;
// Its value in the file: "auto", "60", ..., "uncapped".
std::string frame_rate_name(uint32_t frame_rate);

// 0 for kAuto, else 1, 1.5, 2, 3 or 4.
float scale_of(Resolution resolution);
// Its value in the file: "auto", "1280x720", ...
std::string_view name_of(Resolution resolution);
// A value of the file (letters in either case), nullopt for none.
std::optional<Resolution> resolution_named(std::string_view name);
// TTT2_SCALE's value: 1, 1.5, 2, 3 or 4 ("2.0" too); nullopt otherwise.
std::optional<Resolution> resolution_of_scale(std::string_view text);

// Automatic: enough to cover a display that many pixels high (2 on 1080p
// and 1440p, 3 on 4K), at most 4.
float auto_scale(uint32_t display_height);

// Interpolation (120 frames per second) needs a display that refreshes at
// least this often, in Hz, exactly (a 99.98 Hz display gets 60); the
// launcher's note goes by the same rule.
inline constexpr float kInterpolationHz = 100.0f;

// The window's size in the desktop's units (SDL sizes windows in them; a
// unit is `density` pixels): the render size when the scale is chosen
// (not 0) and that fits on the desktop with room to spare (64 units
// across, 128 down: panels, the title bar), else the largest multiple of
// 640x360 that fits so (640x360 at least).
std::pair<int, int> window_size(int desktop_width, int desktop_height, float density, float scale);

// A line left out, and why.
struct Warning {
    size_t line; // from 1
    std::string text;
};

struct Parsed {
    Settings settings;
    size_t given = 0; // the settings the text gave a usable value
    std::vector<Warning> warnings;
};

// Unknown names and bad values are left out with a warning (that setting
// keeps its default); of a name given twice the earlier line is left out.
Parsed parse(std::string_view text);

// The file's text for `settings`: every name, each with a comment on its
// values, under a header saying what the file is.
std::string format(const Settings& settings);

// $XDG_CONFIG_HOME/ttt2/display.ini (only an absolute XDG_CONFIG_HOME
// counts, else ~/.config), as keyboard.ini.
std::filesystem::path file_path();
// Another game's file instead (Smash 4 PC: display.ini beside the
// executable), and its name for the file's header.
void set_file(const std::filesystem::path& file, const std::string& game);

struct Loaded {
    Settings settings;
    std::vector<std::string> log; // the warnings, then a summary ("display: ...")
    std::string error;            // why a file that is there cannot be read; empty otherwise
};

// The settings from `file`; all defaults when it is missing or unreadable.
Loaded load(const std::filesystem::path& file);

// What this run uses, the environment applied.
struct Active {
    bool fullscreen = false;
    float scale = 0;  // 0: automatic, auto_scale() of the display
    uint32_t fps = 0; // 0: 120 on displays of 100 Hz and more, else 60
    bool operator==(const Active&) const = default;
};

// `settings` with the environment's values (nullptr or empty: not set):
// TTT2_FULLSCREEN=1 fullscreen (another value windowed), TTT2_SCALE=1, 1.5,
// 2, 3 or 4, TTT2_FPS a frame rate (60, 120, 180, 240). Values that are
// not used get a line in `log`.
Active resolve(const Settings& settings, const char* fullscreen, const char* scale, const char* fps,
               std::vector<std::string>& log);

// The window sets these before the renderer starts (defaults until then).
const Active& active();
void set_active(const Active& active);

} // namespace cafe::host::display
