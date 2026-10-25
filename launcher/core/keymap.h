#pragma once

// The game's keyboard keys for the Wii U GamePad's buttons, as the launcher's
// Settings change them: the 19 inputs the keyboard drives, their default
// keys (runtime/src/host/window.cpp's), the keys that cannot be given, and
// $XDG_CONFIG_HOME/ttt2/keyboard.ini, which the game reads (and reads again
// when it changes). Keys are positions on the keyboard: USB HID usages (page
// 7), which are SDL's scancodes, found from the window's hardware keycodes
// through Linux's input event codes by the tables here.

#include "paths.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// ---------------------------------------------------------------- keys

struct Key {
    int usage;        // USB HID usage (= SDL scancode)
    int evdev;        // Linux input event code (KEY_*): the XKB keycode less 8
    const char* name; // as on a US keyboard: the file's comment, and the label when the layout has none
    bool typed;       // a character key, which the user's layout names (A, ;, ß ...) rather than `name`
};

// The keys the launcher can take from the keyboard, in USB HID order.
std::span<const Key> keys();
const Key* key_by_usage(int usage);
const Key* key_by_evdev(int evdev);

// Every usage SDL names, by its name (SDL_GetScancodeName: "Return", "Left
// GUI", "Menu"), as the game reads the file: these keys can be given by hand,
// also those the launcher cannot take from the keyboard (they are shown by
// that name).
struct Named {
    int usage;
    const char* name;
};
std::span<const Named> sdl_names();
// A usage by SDL's name of it, in any case (SDL_GetScancodeFromName: the
// first usage of that name); 0 when it names none.
int usage_by_name(std::string_view name);
// SDL's name of a usage, empty when it has none.
std::string_view sdl_name(int usage);
// What a key is called where the layout does not say: the launcher's name,
// else SDL's, else "Key <usage>".
std::string key_name(int usage);

// Usages that are keys: from 4 (1 to 3 are the USB HID error codes) to
// below 512 (SDL_SCANCODE_COUNT), as the game takes them.
inline constexpr int kFirstKey = 4;
inline constexpr int kKeyCount = 512;

// Why a key cannot be given to an input (F11, Esc, the Super keys); null
// when it can.
const char* reserved_key(int usage);
inline constexpr int kEscape = 41; // (cancels choosing a key in the launcher)
// Ctrl, Shift and Alt: pressed with another key they are a shortcut, so
// the launcher takes one alone only once it is let go.
bool modifier_key(int usage);

// ---------------------------------------------------------------- inputs

struct Input {
    const char* id;    // the file's name for it, and its icon's (ttt2-input-<id>)
    const char* group; // "D-pad", "Face buttons", "Shoulders", "Left stick", "System"
    const char* name;  // its row's name: "Up", "Bottom button"
    const char* full;  // its name alone: "D-pad Up", "Bottom button"
    int key;           // its default key
};

inline constexpr int kInputs = 19;
// In the order the launcher shows them, a group after the other.
const std::array<Input, kInputs>& inputs();
int input_index(std::string_view id); // -1 when there is none of that id

// ---------------------------------------------------------------- the keymap

// One key per input; the launcher keeps them all different: a key given to
// one input is taken from the input that had it, which gets the first one's
// old key instead (they swap).
class Keymap {
public:
    Keymap(); // the defaults

    int key(int input) const { return keys_[input]; }
    bool is_default() const;
    // Of the input that has this key, or -1.
    int input_with(int usage) const;

    struct Bound {
        bool ok = false;
        int swapped = -1; // the input that got the old key
        std::string why;  // when refused
    };
    Bound bind(int input, int usage);
    void reset() { *this = Keymap(); }

    // What reading a file found besides its keys.
    struct Notes {
        // Lines left out, as the game leaves them out (an unknown input, a
        // value that is no key, a reserved key): that input keeps its default.
        std::vector<std::string> warnings;
        // Keys the file gives to more than one input ("K: Bottom button and
        // D-pad Up"), which the game presses together. The launcher keeps one
        // key per input: the input of the earlier line keeps it, the other
        // gets its default when that is free (else another free key), so
        // what is shown is not the game's until the keys are saved again.
        std::vector<std::string> shared;
        // Why the file could not be read (the defaults are shown).
        std::string error;
        // Whether the launcher shows the keys the game uses.
        bool as_shown() const { return shared.empty() && error.empty(); }
    };

    // The file's text, and reading it back the way the game reads it (its
    // comments, spaces, numbers and SDL's names).
    std::string text() const;
    static Keymap parse(std::string_view text, Notes* notes = nullptr);
    // A missing file is the defaults (without a note). Only a regular file
    // of 64 KiB at most is read, as by the game (never a pipe or a device,
    // which could block).
    static Keymap load(const fs::path& file, Notes* notes = nullptr);
    // All of it at once (a new file renamed over the old one), so that the
    // game never reads half of it. A link is followed: the file it leads to
    // is replaced, and keeps its mode (a new one gets the umask's).
    bool save(const fs::path& file, std::string* error = nullptr) const;

    bool operator==(const Keymap&) const = default;

private:
    std::array<int, kInputs> keys_;
};

} // namespace launcher
