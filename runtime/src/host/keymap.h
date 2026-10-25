#pragma once

// The keyboard's keys for the Wii U GamePad: the defaults, and the player's
// own from $XDG_CONFIG_HOME/ttt2/keyboard.ini, which the launcher's Settings
// write. Lines are `id = key`, `#` starts a comment (at the start of a line
// or after a space); a key is an SDL scancode (the USB HID usage of the key
// at that place on the keyboard) or SDL's name for one. No SDL here: the
// window resolves names (SDL_GetScancodeFromName), tests resolve their own.

#include "cafe/input.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cafe::host::keymap {

// One assignable input: GamePad buttons, or a direction of the left stick.
struct Input {
    std::string_view id;
    uint32_t button;
    int8_t stick_x, stick_y; // up positive
    uint16_t default_key;
};

// In the launcher's order. The defaults go by position, as on a US QWERTY
// keyboard: W/S/A/D for the D-pad; I/K/J/L for X/B/Y/A (the GamePad's
// diamond, by place: I top, K bottom, J left, L right); H/; for L/R, U/O
// for ZL/ZR; the arrows the left stick; Enter Plus, Backspace Minus, the
// Home key HOME.
inline constexpr std::array<Input, 19> kInputs{{
    {"dpad_up", os::vpad::kUp, 0, 0, 26},
    {"dpad_down", os::vpad::kDown, 0, 0, 22},
    {"dpad_left", os::vpad::kLeft, 0, 0, 4},
    {"dpad_right", os::vpad::kRight, 0, 0, 7},
    {"face_top", os::vpad::kX, 0, 0, 12},
    {"face_bottom", os::vpad::kB, 0, 0, 14},
    {"face_left", os::vpad::kY, 0, 0, 13},
    {"face_right", os::vpad::kA, 0, 0, 15},
    {"l", os::vpad::kL, 0, 0, 11},
    {"r", os::vpad::kR, 0, 0, 51},
    {"zl", os::vpad::kZL, 0, 0, 24},
    {"zr", os::vpad::kZR, 0, 0, 18},
    {"stick_up", 0, 0, 1, 82},
    {"stick_down", 0, 0, -1, 81},
    {"stick_left", 0, -1, 0, 80},
    {"stick_right", 0, 1, 0, 79},
    {"plus", os::vpad::kPlus, 0, 0, 40},
    {"minus", os::vpad::kMinus, 0, 0, 42},
    {"home", os::vpad::kHome, 0, 0, 74},
}};
constexpr size_t kInputCount = kInputs.size();

// Scancodes are below this (SDL_SCANCODE_COUNT); 1 to 3 are the USB HID
// error codes, not keys.
constexpr uint16_t kKeyCount = 512;
constexpr uint16_t kFirstKey = 4;

// Keys nobody can assign: F11 is the game's fullscreen key, Escape cancels
// picking a key in the launcher, and the GUI keys belong to the desktop.
inline constexpr std::array<uint16_t, 4> kReserved{68, 41, 227, 231};
bool reserved(uint16_t key);

// A key per input, in kInputs' order.
using Keys = std::array<uint16_t, kInputCount>;
Keys default_keys();

// A scancode name's key, 0 when it names none.
using Resolver = std::function<uint16_t(const std::string& name)>;

// A line left out, and why.
struct Warning {
    size_t line; // from 1
    std::string text;
};

struct Parsed {
    Keys keys = default_keys();
    size_t assigned = 0; // inputs the text gave a usable key
    std::vector<Warning> warnings;
};

// Unknown inputs, bad keys and reserved ones are left out with a warning
// (the input keeps its default); of an input given twice the earlier line
// is left out. Two inputs on one key are fine: that key presses both.
Parsed parse(std::string_view text, const Resolver& resolve);

// $XDG_CONFIG_HOME/ttt2/keyboard.ini (only an absolute XDG_CONFIG_HOME
// counts, else ~/.config), as the install's config file.
std::filesystem::path file_path();

struct Loaded {
    Keys keys;
    std::vector<std::string> log; // the warnings, then a summary ("keyboard: ...")
};

// The keys from `file`; all defaults when it is missing or unreadable.
Loaded load(const std::filesystem::path& file, const Resolver& resolve);

// The file, read again whenever it is another one than read last: written
// anew (the launcher's atomic rename: another inode), changed in place
// (size, modification time), created or deleted.
class Watcher {
public:
    Watcher(std::filesystem::path file, Resolver resolve);
    // The first call always reads; later ones only after a change (a stat,
    // cheap enough for the window loop about once a second).
    std::optional<Loaded> reload();

private:
    struct Stamp {
        bool exists = false;
        uint64_t device = 0, inode = 0, size = 0;
        int64_t mtime_ns = 0, ctime_ns = 0;
        bool operator==(const Stamp&) const = default;
    };
    Stamp stamp() const;

    std::filesystem::path file_;
    Resolver resolve_;
    std::optional<Stamp> read_;
};

} // namespace cafe::host::keymap
