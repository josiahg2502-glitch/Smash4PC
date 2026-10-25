#pragma once

// The game's display settings as the launcher's Settings change them:
// $XDG_CONFIG_HOME/ttt2/display.ini (Places::display()), read and written
// by the game's own code for it (runtime/src/host/display_settings.h), so
// that what the launcher writes is what the game reads when it starts.

#include "paths.h"

#include "host/display_settings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace launcher {

namespace display = cafe::host::display;

// The file's text for `settings` (display::format), all of it at once (as
// replace_file writes it).
bool save_display(const fs::path& file, const display::Settings& settings, std::string* error = nullptr);

// As the Settings page names it: "Automatic", "1280 × 720", ...
std::string resolution_label(display::Resolution resolution);
// What Automatic draws at on a display that many pixels high: "2560 × 1440".
std::string automatic_size(uint32_t display_height);

// A monitor of the desktop: its height in pixels and its refresh rate in
// millihertz, each 0 when it is not known.
struct Monitor {
    uint32_t height = 0;
    int refresh = 0;
};

// The game goes by its primary display (Automatic covers that one; it
// shows 120 frames a second when that display or its window's refreshes at
// kInterpolationHz or more, exactly), which GTK cannot name on Wayland. So
// the notes say only what is sure: with one monitor, that one's figures;
// with more, no size, and the 60 fps note only when every monitor is below
// kInterpolationHz.
//
// Under Resolution: "Automatic: enough to cover the display (2560 × 1440 on
// this one).", or "... the main display." when there are more.
std::string automatic_note(const std::vector<Monitor>& monitors);
// Under 120 fps: "This display refreshes at 60 Hz: the game stays at 60 fps
// there." ("These displays refresh at ...: the game stays at 60 fps." with
// more); empty when the game may show 120.
std::string refresh_note(const std::vector<Monitor>& monitors);
// A refresh rate as the note says it, in Hz: "60", or with up to two
// decimals where the whole number would read as kInterpolationHz or more
// although the rate is below it ("99.98").
std::string hertz_text(int millihertz);

// Why display::load could not read `file` (its Loaded::error), said without
// the file's path, which the page names: "Permission denied", "it is not a
// file".
std::string unreadable_reason(const std::string& error, const fs::path& file);

} // namespace launcher
