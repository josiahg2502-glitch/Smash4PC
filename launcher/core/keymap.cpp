// The game's keyboard keys (see keymap.h).

#include "keymap.h"

#include "files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace launcher {
namespace {

// The keys of a PC keyboard: the USB HID usage tables' keyboard page (the
// usage, which SDL's scancodes are) beside Linux's input event code of the
// same key (linux/input-event-codes.h), which X11 and Wayland give as the
// keycode less 8. Keys that the desktop keeps for itself (volume, media,
// power) are left out, and so are the few that no keyboard sends to Linux
// as these codes or that SDL does not take as the same key everywhere (the
// keypad's comma). keymap_test cross-checks this against both headers.
constexpr Key kKeys[] = {
    {4, 30, "A", true},
    {5, 48, "B", true},
    {6, 46, "C", true},
    {7, 32, "D", true},
    {8, 18, "E", true},
    {9, 33, "F", true},
    {10, 34, "G", true},
    {11, 35, "H", true},
    {12, 23, "I", true},
    {13, 36, "J", true},
    {14, 37, "K", true},
    {15, 38, "L", true},
    {16, 50, "M", true},
    {17, 49, "N", true},
    {18, 24, "O", true},
    {19, 25, "P", true},
    {20, 16, "Q", true},
    {21, 19, "R", true},
    {22, 31, "S", true},
    {23, 20, "T", true},
    {24, 22, "U", true},
    {25, 47, "V", true},
    {26, 17, "W", true},
    {27, 45, "X", true},
    {28, 21, "Y", true},
    {29, 44, "Z", true},
    {30, 2, "1", true},
    {31, 3, "2", true},
    {32, 4, "3", true},
    {33, 5, "4", true},
    {34, 6, "5", true},
    {35, 7, "6", true},
    {36, 8, "7", true},
    {37, 9, "8", true},
    {38, 10, "9", true},
    {39, 11, "0", true},
    {40, 28, "Enter", false},
    {41, 1, "Esc", false},
    {42, 14, "Backspace", false},
    {43, 15, "Tab", false},
    {44, 57, "Space", false},
    {45, 12, "-", true},
    {46, 13, "=", true},
    {47, 26, "[", true},
    {48, 27, "]", true},
    {49, 43, "\\", true},
    {51, 39, ";", true},
    {52, 40, "'", true},
    {53, 41, "`", true},
    {54, 51, ",", true},
    {55, 52, ".", true},
    {56, 53, "/", true},
    {57, 58, "Caps Lock", false},
    {58, 59, "F1", false},
    {59, 60, "F2", false},
    {60, 61, "F3", false},
    {61, 62, "F4", false},
    {62, 63, "F5", false},
    {63, 64, "F6", false},
    {64, 65, "F7", false},
    {65, 66, "F8", false},
    {66, 67, "F9", false},
    {67, 68, "F10", false},
    {68, 87, "F11", false},
    {69, 88, "F12", false},
    {70, 99, "Print Screen", false},
    {71, 70, "Scroll Lock", false},
    {72, 119, "Pause", false},
    {73, 110, "Insert", false},
    {74, 102, "Home", false},
    {75, 104, "Page Up", false},
    {76, 111, "Delete", false},
    {77, 107, "End", false},
    {78, 109, "Page Down", false},
    {79, 106, "Right", false},
    {80, 105, "Left", false},
    {81, 108, "Down", false},
    {82, 103, "Up", false},
    {83, 69, "Num Lock", false},
    {84, 98, "Keypad /", false},
    {85, 55, "Keypad *", false},
    {86, 74, "Keypad -", false},
    {87, 78, "Keypad +", false},
    {88, 96, "Keypad Enter", false},
    {89, 79, "Keypad 1", false},
    {90, 80, "Keypad 2", false},
    {91, 81, "Keypad 3", false},
    {92, 75, "Keypad 4", false},
    {93, 76, "Keypad 5", false},
    {94, 77, "Keypad 6", false},
    {95, 71, "Keypad 7", false},
    {96, 72, "Keypad 8", false},
    {97, 73, "Keypad 9", false},
    {98, 82, "Keypad 0", false},
    {99, 83, "Keypad .", false},
    {100, 86, "Non-US \\", true}, // (the key right of the left Shift on ISO keyboards)
    {101, 127, "Menu", false},
    {103, 117, "Keypad =", false},
    {104, 183, "F13", false},
    {105, 184, "F14", false},
    {106, 185, "F15", false},
    {107, 186, "F16", false},
    {108, 187, "F17", false},
    {109, 188, "F18", false},
    {110, 189, "F19", false},
    {111, 190, "F20", false},
    {112, 191, "F21", false},
    {113, 192, "F22", false},
    {114, 193, "F23", false},
    {115, 194, "F24", false},
    {135, 89, "Ro", true},
    {136, 93, "Katakana/Hiragana", false},
    {137, 124, "Yen", true},
    {138, 92, "Henkan", false},
    {139, 94, "Muhenkan", false},
    {144, 122, "Hangul", false},
    {145, 123, "Hanja", false},
    {224, 29, "Left Ctrl", false},
    {225, 42, "Left Shift", false},
    {226, 56, "Left Alt", false},
    {227, 125, "Left Super", false},
    {228, 97, "Right Ctrl", false},
    {229, 54, "Right Shift", false},
    {230, 100, "Right Alt", false},
    {231, 126, "Right Super", false},
};

// SDL's name of every usage it names (SDL_GetScancodeName, SDL 3), which a
// file written by hand may give; keyboard_file_test checks each against
// SDL. ("#" and "Keypad #" cannot be given by name: "#" after a space
// starts a comment; nor the digits, which are numbers: usages.)
constexpr Named kSdlNames[] = {
    {4, "A"}, {5, "B"}, {6, "C"}, {7, "D"}, {8, "E"}, {9, "F"}, {10, "G"}, {11, "H"}, {12, "I"}, {13, "J"},
    {14, "K"}, {15, "L"}, {16, "M"}, {17, "N"}, {18, "O"}, {19, "P"}, {20, "Q"}, {21, "R"}, {22, "S"}, {23, "T"},
    {24, "U"}, {25, "V"}, {26, "W"}, {27, "X"}, {28, "Y"}, {29, "Z"}, {30, "1"}, {31, "2"}, {32, "3"}, {33, "4"},
    {34, "5"}, {35, "6"}, {36, "7"}, {37, "8"}, {38, "9"}, {39, "0"}, {40, "Return"}, {41, "Escape"},
    {42, "Backspace"}, {43, "Tab"}, {44, "Space"}, {45, "-"}, {46, "="}, {47, "["}, {48, "]"}, {49, "\\"}, {50, "#"},
    {51, ";"}, {52, "'"}, {53, "`"}, {54, ","}, {55, "."}, {56, "/"}, {57, "CapsLock"}, {58, "F1"}, {59, "F2"},
    {60, "F3"}, {61, "F4"}, {62, "F5"}, {63, "F6"}, {64, "F7"}, {65, "F8"}, {66, "F9"}, {67, "F10"}, {68, "F11"},
    {69, "F12"}, {70, "PrintScreen"}, {71, "ScrollLock"}, {72, "Pause"}, {73, "Insert"}, {74, "Home"},
    {75, "PageUp"}, {76, "Delete"}, {77, "End"}, {78, "PageDown"}, {79, "Right"}, {80, "Left"}, {81, "Down"},
    {82, "Up"}, {83, "Numlock"}, {84, "Keypad /"}, {85, "Keypad *"}, {86, "Keypad -"}, {87, "Keypad +"},
    {88, "Keypad Enter"}, {89, "Keypad 1"}, {90, "Keypad 2"}, {91, "Keypad 3"}, {92, "Keypad 4"}, {93, "Keypad 5"},
    {94, "Keypad 6"}, {95, "Keypad 7"}, {96, "Keypad 8"}, {97, "Keypad 9"}, {98, "Keypad 0"}, {99, "Keypad ."},
    {100, "NonUSBackslash"}, {101, "Application"}, {102, "Power"}, {103, "Keypad ="}, {104, "F13"}, {105, "F14"},
    {106, "F15"}, {107, "F16"}, {108, "F17"}, {109, "F18"}, {110, "F19"}, {111, "F20"}, {112, "F21"}, {113, "F22"},
    {114, "F23"}, {115, "F24"}, {116, "Execute"}, {117, "Help"}, {118, "Menu"}, {119, "Select"}, {120, "Stop"},
    {121, "Again"}, {122, "Undo"}, {123, "Cut"}, {124, "Copy"}, {125, "Paste"}, {126, "Find"}, {127, "Mute"},
    {128, "VolumeUp"}, {129, "VolumeDown"}, {133, "Keypad ,"}, {134, "Keypad = (AS400)"}, {135, "International 1"},
    {136, "International 2"}, {137, "International 3"}, {138, "International 4"}, {139, "International 5"},
    {140, "International 6"}, {141, "International 7"}, {142, "International 8"}, {143, "International 9"},
    {144, "Language 1"}, {145, "Language 2"}, {146, "Language 3"}, {147, "Language 4"}, {148, "Language 5"},
    {149, "Language 6"}, {150, "Language 7"}, {151, "Language 8"}, {152, "Language 9"}, {153, "AltErase"},
    {154, "SysReq"}, {155, "Cancel"}, {156, "Clear"}, {157, "Prior"}, {158, "Return"}, {159, "Separator"},
    {160, "Out"}, {161, "Oper"}, {162, "Clear / Again"}, {163, "CrSel"}, {164, "ExSel"}, {176, "Keypad 00"},
    {177, "Keypad 000"}, {178, "ThousandsSeparator"}, {179, "DecimalSeparator"}, {180, "CurrencyUnit"},
    {181, "CurrencySubUnit"}, {182, "Keypad ("}, {183, "Keypad )"}, {184, "Keypad {"}, {185, "Keypad }"},
    {186, "Keypad Tab"}, {187, "Keypad Backspace"}, {188, "Keypad A"}, {189, "Keypad B"}, {190, "Keypad C"},
    {191, "Keypad D"}, {192, "Keypad E"}, {193, "Keypad F"}, {194, "Keypad XOR"}, {195, "Keypad ^"},
    {196, "Keypad %"}, {197, "Keypad <"}, {198, "Keypad >"}, {199, "Keypad &"}, {200, "Keypad &&"},
    {201, "Keypad |"}, {202, "Keypad ||"}, {203, "Keypad :"}, {204, "Keypad #"}, {205, "Keypad Space"},
    {206, "Keypad @"}, {207, "Keypad !"}, {208, "Keypad MemStore"}, {209, "Keypad MemRecall"},
    {210, "Keypad MemClear"}, {211, "Keypad MemAdd"}, {212, "Keypad MemSubtract"}, {213, "Keypad MemMultiply"},
    {214, "Keypad MemDivide"}, {215, "Keypad +/-"}, {216, "Keypad Clear"}, {217, "Keypad ClearEntry"},
    {218, "Keypad Binary"}, {219, "Keypad Octal"}, {220, "Keypad Decimal"}, {221, "Keypad Hexadecimal"},
    {224, "Left Ctrl"}, {225, "Left Shift"}, {226, "Left Alt"}, {227, "Left GUI"}, {228, "Right Ctrl"},
    {229, "Right Shift"}, {230, "Right Alt"}, {231, "Right GUI"}, {257, "ModeSwitch"}, {258, "Sleep"}, {259, "Wake"},
    {260, "ChannelUp"}, {261, "ChannelDown"}, {262, "MediaPlay"}, {263, "MediaPause"}, {264, "MediaRecord"},
    {265, "MediaFastForward"}, {266, "MediaRewind"}, {267, "MediaTrackNext"}, {268, "MediaTrackPrevious"},
    {269, "MediaStop"}, {270, "Eject"}, {271, "MediaPlayPause"}, {272, "MediaSelect"}, {273, "AC New"},
    {274, "AC Open"}, {275, "AC Close"}, {276, "AC Exit"}, {277, "AC Save"}, {278, "AC Print"},
    {279, "AC Properties"}, {280, "AC Search"}, {281, "AC Home"}, {282, "AC Back"}, {283, "AC Forward"},
    {284, "AC Stop"}, {285, "AC Refresh"}, {286, "AC Bookmarks"}, {287, "SoftLeft"}, {288, "SoftRight"},
    {289, "Call"}, {290, "EndCall"},
};

// The GamePad's inputs, with the game's default keys (W, A, S and D for the
// D-pad; I, J, K and L the face buttons' diamond; H and ; for L and R, U and
// O for ZL and ZR; the arrows for the left stick).
constexpr std::array<Input, kInputs> kInputList = {{
    {"dpad_up", "D-pad", "Up", "D-pad Up", 26},
    {"dpad_down", "D-pad", "Down", "D-pad Down", 22},
    {"dpad_left", "D-pad", "Left", "D-pad Left", 4},
    {"dpad_right", "D-pad", "Right", "D-pad Right", 7},
    {"face_top", "Face buttons", "Top button", "Top button", 12},
    {"face_bottom", "Face buttons", "Bottom button", "Bottom button", 14},
    {"face_left", "Face buttons", "Left button", "Left button", 13},
    {"face_right", "Face buttons", "Right button", "Right button", 15},
    {"l", "Shoulders", "L", "L", 11},
    {"r", "Shoulders", "R", "R", 51},
    {"zl", "Shoulders", "ZL", "ZL", 24},
    {"zr", "Shoulders", "ZR", "ZR", 18},
    {"stick_up", "Left stick", "Up", "Left stick Up", 82},
    {"stick_down", "Left stick", "Down", "Left stick Down", 81},
    {"stick_left", "Left stick", "Left", "Left stick Left", 80},
    {"stick_right", "Left stick", "Right", "Left stick Right", 79},
    {"plus", "System", "+ (Start)", "+ (Start)", 40},
    {"minus", "System", "\u2212 (Select)", "\u2212 (Select)", 42},
    {"home", "System", "HOME", "HOME", 74},
}};


// The spaces the game's parser skips (runtime/src/host/keymap.cpp): a file
// written by hand reads the same to both.
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// A line without its comment: '#' at its start or after a space starts one,
// as the game reads it.
std::string_view uncommented(std::string_view line) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '#' && (i == 0 || space(line[i - 1]))) return line.substr(0, i);
    }
    return line;
}

// Lower case, as SDL compares names.
std::string folded(std::string_view s) {
    std::string out;
    for (char c : s) out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return out;
}

// The key a value names, as the game reads it: digits are a number (a
// usage, the 1 key being 30), anything else SDL's name of a key; 0 when it
// names none. A number that is not a key here is still one (the game knows
// it), shown by its number.
int key_of(std::string_view value) {
    if (value.empty()) return 0;
    if (std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        unsigned number = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
        const bool whole = ec == std::errc() && end == value.data() + value.size();
        return whole && number >= kFirstKey && number < kKeyCount ? static_cast<int>(number) : 0;
    }
    return usage_by_name(value);
}

// A file written by hand is a few hundred bytes; the game reads no larger one.
constexpr off_t kMaxFileSize = 64 * 1024;

} // namespace

std::span<const Key> keys() { return kKeys; }

const Key* key_by_usage(int usage) {
    for (const Key& k : kKeys) {
        if (k.usage == usage) return &k;
    }
    return nullptr;
}

const Key* key_by_evdev(int evdev) {
    for (const Key& k : kKeys) {
        if (k.evdev == evdev) return &k;
    }
    return nullptr;
}

std::span<const Named> sdl_names() { return kSdlNames; }

int usage_by_name(std::string_view name) {
    const std::string wanted = folded(name);
    if (wanted.empty()) return 0;
    for (const Named& n : kSdlNames) {
        if (folded(n.name) == wanted) return n.usage;
    }
    return 0;
}

std::string_view sdl_name(int usage) {
    for (const Named& n : kSdlNames) {
        if (n.usage == usage) return n.name;
    }
    return {};
}

std::string key_name(int usage) {
    if (const Key* key = key_by_usage(usage)) return key->name;
    // (SDL's name, unless the launcher calls another key so: SDL's "Menu" is
    // not the key the launcher calls Menu.)
    const std::string_view sdl = sdl_name(usage);
    const bool other = std::any_of(std::begin(kKeys), std::end(kKeys), [&](const Key& k) { return sdl == k.name; });
    return sdl.empty() || other ? "Key " + std::to_string(usage) : std::string(sdl);
}

const char* reserved_key(int usage) {
    switch (usage) {
    case 68: return "F11 toggles full screen";
    case kEscape: return "Esc stops choosing a key";
    case 227:
    case 231: return "Super is the desktop's key";
    default: return nullptr;
    }
}

bool modifier_key(int usage) { return usage >= 224 && usage <= 230 && usage != 227; }

const std::array<Input, kInputs>& inputs() { return kInputList; }

int input_index(std::string_view id) {
    for (int i = 0; i < kInputs; ++i) {
        if (id == kInputList[i].id) return i;
    }
    return -1;
}

Keymap::Keymap() {
    for (int i = 0; i < kInputs; ++i) keys_[i] = kInputList[i].key;
}

bool Keymap::is_default() const { return *this == Keymap(); }

int Keymap::input_with(int usage) const {
    for (int i = 0; i < kInputs; ++i) {
        if (keys_[i] == usage) return i;
    }
    return -1;
}

Keymap::Bound Keymap::bind(int input, int usage) {
    Bound bound;
    if (input < 0 || input >= kInputs) {
        bound.why = "There is no such input";
        return bound;
    }
    if (const char* why = reserved_key(usage)) {
        bound.why = why;
        return bound;
    }
    if (usage <= 0 || !key_by_usage(usage)) {
        bound.why = "This key cannot be used";
        return bound;
    }
    bound.ok = true;
    const int other = input_with(usage);
    if (other == input) return bound;
    if (other >= 0) {
        keys_[other] = keys_[input];
        bound.swapped = other;
    }
    keys_[input] = usage;
    return bound;
}

std::string Keymap::text() const {
    std::string out =
        "# Tekken Tag Tournament 2: the keyboard's keys for the Wii U GamePad's buttons.\n"
        "# Written by Ultimate Launcher (Settings); the game reads it when it starts and\n"
        "# again whenever it changes. A line is \"input = key\": the key's position on the keyboard as a USB HID\n"
        "# usage (an SDL scancode) or an SDL scancode name; the comment names that key on a US keyboard.\n";
    const char* group = nullptr;
    for (int i = 0; i < kInputs; ++i) {
        const Input& in = kInputList[i];
        if (!group || std::strcmp(group, in.group) != 0) {
            group = in.group;
            out += std::string("\n# ") + group + "\n";
        }
        char line[128];
        std::snprintf(line, sizeof line, "%-11s = %-3d  # %s\n", in.id, keys_[i], key_name(keys_[i]).c_str());
        out += line;
    }
    return out;
}

Keymap Keymap::parse(std::string_view text, Notes* notes) {
    Notes unused;
    Notes& out = notes ? *notes : unused;
    const auto warn = [&](const std::string& what) { out.warnings.push_back(what); };
    Keymap map;
    std::array<int, kInputs> given{}; // the line that gives each its key, 0 when none does
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3); // (a byte order mark from an editor)
    int number = 0;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        const std::string_view raw = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        ++number;
        const std::string_view line = trim(uncommented(raw));
        if (line.empty()) continue;
        const std::string where = "line " + std::to_string(number);
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            warn(where + ": not \"input = key\"");
            continue;
        }
        const std::string_view id = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
        const int input = input_index(id);
        if (input < 0) {
            warn(where + ": no input " + std::string(id));
            continue;
        }
        const int key = key_of(value);
        if (key == 0) {
            warn(where + ": " + std::string(value) + " is not a key; " + std::string(id) + " keeps its default");
            continue;
        }
        if (const char* why = reserved_key(key)) {
            warn(where + ": " + why + "; " + std::string(id) + " keeps its default");
            continue;
        }
        if (given[input]) {
            warn("line " + std::to_string(given[input]) + ": " + std::string(id) + " is given again on " + where +
                 ", which counts");
        }
        map.keys_[input] = key;
        given[input] = number;
    }
    // A key on two inputs (by hand: given twice, or given to one input while
    // another keeps it as its default), which the game presses together: the
    // input the file gives it to keeps it (the earlier line, when it gives it
    // to both), and the other one gets what a swap would give it: its own
    // default when that is free, else the winner's default, else the first
    // key free.
    for (int i = 0; i < kInputs; ++i) {
        int winner = -1;
        for (int j = 0; j < kInputs && winner < 0; ++j) {
            if (j != i && map.keys_[j] == map.keys_[i] && given[j] && (!given[i] || given[j] < given[i])) winner = j;
        }
        if (winner < 0) continue;
        out.shared.push_back(key_name(map.keys_[i]) + ": " + kInputList[winner].full + " and " +
                             kInputList[i].full);
        int key = kInputList[i].key;
        if (map.input_with(key) >= 0) key = kInputList[winner].key;
        if (map.input_with(key) >= 0) {
            for (const Key& k : kKeys) {
                if (!reserved_key(k.usage) && map.input_with(k.usage) < 0) {
                    key = k.usage;
                    break;
                }
            }
        }
        map.keys_[i] = key;
    }
    return map;
}

Keymap Keymap::load(const fs::path& file, Notes* notes) {
    Notes unused;
    Notes& out = notes ? *notes : unused;
    // As the game reads it: never a pipe or a device, which could block (or
    // never end), and checked again on what was opened, which the name may no
    // longer be.
    struct stat st{};
    if (::stat(file.c_str(), &st) != 0) {
        if (errno != ENOENT && errno != ENOTDIR) out.error = std::strerror(errno);
        return Keymap();
    }
    const auto unusable = [](const struct stat& s) -> const char* {
        if (!S_ISREG(s.st_mode)) return "it is not a file";
        if (s.st_size > kMaxFileSize) return "it is larger than 64 KiB";
        return nullptr;
    };
    if (const char* why = unusable(st)) {
        out.error = why;
        return Keymap();
    }
    const int fd = ::open(file.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) out.error = std::strerror(errno);
        return Keymap();
    }
    std::string text, why;
    if (::fstat(fd, &st) != 0) why = std::strerror(errno);
    else if (const char* bad = unusable(st)) why = bad;
    char buffer[4096];
    while (why.empty()) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) why = std::strerror(errno);
        if (n <= 0) break;
        text.append(buffer, static_cast<size_t>(n));
        if (text.size() > static_cast<size_t>(kMaxFileSize)) why = "it is larger than 64 KiB";
    }
    ::close(fd);
    if (!why.empty()) {
        out.error = why;
        return Keymap();
    }
    return parse(text, &out);
}

bool Keymap::save(const fs::path& file, std::string* error) const { return replace_file(file, text(), error); }

} // namespace launcher
