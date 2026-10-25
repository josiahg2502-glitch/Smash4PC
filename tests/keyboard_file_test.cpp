// keyboard.ini between the launcher, which writes it (launcher/core/keymap.h),
// and the game, which reads it (runtime/src/host/keymap.h): both keep their
// own list of the inputs, their defaults and the keys that cannot be given,
// and these must stay the same. Every file the launcher can write is read
// back by the game's parser as the launcher meant it, and a file written by
// hand (SDL's names, comments, odd spaces, numbers) is read by both alike:
// the launcher shows the game's keys, or says that it cannot.

#include "host/keymap.h"
#include "keymap.h"

#include <SDL3/SDL_keyboard.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

namespace game = cafe::host::keymap;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

// SDL's names, as the window resolves them.
uint16_t sdl_name(const std::string& name) { return static_cast<uint16_t>(SDL_GetScancodeFromName(name.c_str())); }

void test_tables() {
    check(game::kInputCount == static_cast<size_t>(launcher::kInputs), "the same number of inputs");
    for (int i = 0; i < launcher::kInputs && static_cast<size_t>(i) < game::kInputCount; ++i) {
        const launcher::Input& l = launcher::inputs()[i];
        const game::Input& g = game::kInputs[i];
        check(g.id == l.id, "input " + std::to_string(i) + ": the game's " + std::string(g.id) + ", the launcher's " +
                                l.id);
        check(g.default_key == l.key, std::string(l.id) + "'s default: the game's " + std::to_string(g.default_key) +
                                          ", the launcher's " + std::to_string(l.key));
    }
    int reserved = 0;
    for (const launcher::Key& k : launcher::keys()) {
        const bool l = launcher::reserved_key(k.usage) != nullptr;
        check(l == game::reserved(static_cast<uint16_t>(k.usage)),
              std::string(k.name) + " is reserved for one side only");
        reserved += l;
    }
    check(reserved == static_cast<int>(game::kReserved.size()), "the launcher knows every reserved key");
}

void test_files() {
    const game::Parsed defaults = game::parse(launcher::Keymap().text(), sdl_name);
    check(defaults.warnings.empty() && defaults.assigned == game::kInputCount, "the defaults' file: all read");
    check(defaults.keys == game::default_keys(), "the defaults' file: the game's defaults");

    // Every key the launcher can give, to each input (the input that had it
    // gets the old key: two keys change).
    for (int input = 0; input < launcher::kInputs; ++input) {
        for (const launcher::Key& k : launcher::keys()) {
            if (launcher::reserved_key(k.usage)) continue;
            launcher::Keymap keys;
            const std::string what = std::string(launcher::inputs()[input].id) + " = " + k.name;
            if (!keys.bind(input, k.usage).ok) {
                check(false, what + ": not taken by the launcher");
                continue;
            }
            const game::Parsed read = game::parse(keys.text(), sdl_name);
            check(read.warnings.empty(), what + ": the game warns (" +
                                             (read.warnings.empty() ? "" : read.warnings[0].text) + ")");
            check(read.assigned == game::kInputCount, what + ": the game reads " + std::to_string(read.assigned) +
                                                          " keys");
            for (int i = 0; i < launcher::kInputs; ++i) {
                check(read.keys[i] == keys.key(i), what + ": the game reads " + launcher::inputs()[i].id + " as " +
                                                       std::to_string(read.keys[i]) + ", not " +
                                                       std::to_string(keys.key(i)));
            }
        }
    }
}

// A file written by hand names keys as SDL does: the launcher knows all of
// SDL's names, and reads each as the game does.
void test_names() {
    check(launcher::kFirstKey == game::kFirstKey && launcher::kKeyCount == game::kKeyCount, "the same key numbers");
    int named = 0;
    for (int usage = 0; usage < SDL_SCANCODE_COUNT; ++usage) {
        const std::string name = SDL_GetScancodeName(static_cast<SDL_Scancode>(usage));
        if (name.empty()) continue;
        ++named;
        check(launcher::sdl_name(usage) == name, "SDL's name of " + std::to_string(usage) + " (" + name +
                                                     ") is not the launcher's");
        check(launcher::usage_by_name(name) == sdl_name(name),
              "SDL's name " + name + " is another key to the launcher");
    }
    check(named == static_cast<int>(launcher::sdl_names().size()), "the launcher knows only SDL's names");
}

// The keys as the game reads a text, and as the launcher shows it: the same
// when the launcher says so (Notes::as_shown); else the game gives a key to
// several inputs, and the launcher each input one of its own.
void compare(const std::string& text, const std::string& what) {
    const game::Parsed read = game::parse(text, sdl_name);
    launcher::Keymap::Notes notes;
    const launcher::Keymap shown = launcher::Keymap::parse(text, &notes);
    bool same = true, shared = false;
    for (int i = 0; i < launcher::kInputs; ++i) {
        same = same && read.keys[i] == shown.key(i);
        for (int j = 0; j < i; ++j) shared = shared || read.keys[i] == read.keys[j];
    }
    check(same == notes.as_shown(), what + ": the launcher says it shows the game's keys, but it does not");
    check(shared == !notes.shared.empty(), what + ": a key on several inputs, said or not");
    // (Lines left out: as many to both.)
    check(read.warnings.size() == notes.warnings.size(),
          what + ": the game leaves out " + std::to_string(read.warnings.size()) + " lines, the launcher " +
              std::to_string(notes.warnings.size()));
}

void test_by_hand() {
    for (const char* text : {
             "\xEF\xBB\xBF" "dpad_up = 5\n", "dpad_up = 5\f\n", "dpad_up = 5\v\n", "\vdpad_up\f=\v5\n",
             "dpad_up = 5\r# c\n", "dpad_up = 5#c\n", "dpad_up = 1\n", "dpad_up = 2\n", "dpad_up = 3\n",
             "dpad_up = 4\n", "dpad_up = 0004\n", "dpad_up = 511\n", "dpad_up = 512\n", "dpad_up = 99999999999\n",
             "dpad_up = +5\n", "dpad_up = -5\n", "dpad_up = 0x16\n", "home = Menu\n", "home = Keypad ,\n",
             "home = Mute\n", "home = AC Back\n", "home = keypad #\n", "home = #\n", "home = Keypad = (AS400)\n",
             "home = =\n", "home = Return\n", "home = Escape\n", "home = Left GUI\n", "home = F11\n",
             "face_top = 14\n", "face_top = 26\ndpad_up = 26\n", "dpad_up = 26\nface_top = 26\n",
             "dpad_up = Home\n", "face_top = ;\n", "home = h\nl = Home\n",
             "dpad_up = 5\ndpad_up = 6\n", "dpad_up = 5\nface_top = 5\nface_left = 5\n", "nope = 5\n",
             "dpad_up 5\n", "dpad_up =\n", "= 5\n", "\r\n\r\n", "", "#", "dpad_up = K\r\nface_bottom = 26\r\n",
         }) {
        compare(text, std::string("\"") + text + "\"");
    }
    // Every name SDL has, given to an input.
    for (int usage = 0; usage < SDL_SCANCODE_COUNT; ++usage) {
        const std::string name = SDL_GetScancodeName(static_cast<SDL_Scancode>(usage));
        if (!name.empty()) compare("zr = " + name + "\n", "zr = " + name);
    }
    // Files made of lines like those, at random (always the same ones).
    const std::vector<std::string> ids = {"dpad_up", "face_top", "face_bottom", "zr", "home", "plus", "nope", ""};
    const std::vector<std::string> values = {"5", "1", "3", "4", "14", "26", "40", "68", "300", "511", "512",
                                             "K", "return", "Menu", "Keypad ,", "Mute", "Left GUI", "W",
                                             "AC Back", "#", "", "x", "0x16", "+5"};
    const std::vector<std::string> spaces = {"", " ", "\t", "\v", "\f", "\r", "  "};
    const std::vector<std::string> ends = {"", " # c", "#c", "\r", "\v# c"};
    std::mt19937 random(2026);
    const auto pick = [&](const std::vector<std::string>& from) { return from[random() % from.size()]; };
    for (int file = 0; file < 20000; ++file) {
        std::string text = random() % 8 == 0 ? "\xEF\xBB\xBF" : "";
        for (int line = static_cast<int>(random() % 6); line > 0; --line) {
            text += pick(spaces) + pick(ids) + pick(spaces) + (random() % 10 ? "=" : "") + pick(spaces) +
                    pick(values) + pick(spaces) + pick(ends) + "\n";
        }
        compare(text, "file " + std::to_string(file));
        if (g_failures > 20) return;
    }
}

} // namespace

int main() {
    test_tables();
    test_files();
    test_names();
    test_by_hand();
    if (g_failures == 0) std::printf("keyboard_file_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
