// The keyboard's keys from keyboard.ini (runtime/src/host/keymap.h): what a
// file says and what is ignored, where the file is, and that the game reads
// it again when the launcher writes it anew. Files go to a scratch folder.

#include "host/keymap.h"

#include <SDL3/SDL_keyboard.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace keymap = cafe::host::keymap;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

size_t index_of(std::string_view id) {
    for (size_t i = 0; i < keymap::kInputCount; ++i) {
        if (keymap::kInputs[i].id == id) return i;
    }
    std::printf("FAIL no input %s\n", std::string(id).c_str());
    std::exit(1);
}

// SDL's names, as the window resolves them (no SDL_Init needed for that).
uint16_t sdl_name(const std::string& name) { return static_cast<uint16_t>(SDL_GetScancodeFromName(name.c_str())); }

// Only "A#B" is a key, to see where comments start.
uint16_t test_name(const std::string& name) { return name == "A#B" ? 100 : 0; }

bool has_warning(const keymap::Parsed& parsed, size_t line, const std::string& part) {
    for (const keymap::Warning& w : parsed.warnings) {
        if (w.line == line && w.text.find(part) != std::string::npos) return true;
    }
    return false;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const std::string& line : lines) all += line + "\n";
    return all;
}

void write(const fs::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

// The launcher's way: a whole new file renamed over the old one.
void replace(const fs::path& file, const std::string& text) {
    const fs::path temporary = file.string() + ".new";
    write(temporary, text);
    fs::rename(temporary, file);
}

void test_inputs() {
    // The spec's ids, in the launcher's order, on today's keys.
    const struct {
        const char* id;
        uint32_t button;
        int stick_x, stick_y;
        uint16_t key;
    } expected[] = {
        {"dpad_up", cafe::os::vpad::kUp, 0, 0, 26},       {"dpad_down", cafe::os::vpad::kDown, 0, 0, 22},
        {"dpad_left", cafe::os::vpad::kLeft, 0, 0, 4},    {"dpad_right", cafe::os::vpad::kRight, 0, 0, 7},
        {"face_top", cafe::os::vpad::kX, 0, 0, 12},       {"face_bottom", cafe::os::vpad::kB, 0, 0, 14},
        {"face_left", cafe::os::vpad::kY, 0, 0, 13},      {"face_right", cafe::os::vpad::kA, 0, 0, 15},
        {"l", cafe::os::vpad::kL, 0, 0, 11},              {"r", cafe::os::vpad::kR, 0, 0, 51},
        {"zl", cafe::os::vpad::kZL, 0, 0, 24},            {"zr", cafe::os::vpad::kZR, 0, 0, 18},
        {"stick_up", 0, 0, 1, 82},                        {"stick_down", 0, 0, -1, 81},
        {"stick_left", 0, -1, 0, 80},                     {"stick_right", 0, 1, 0, 79},
        {"plus", cafe::os::vpad::kPlus, 0, 0, 40},        {"minus", cafe::os::vpad::kMinus, 0, 0, 42},
        {"home", cafe::os::vpad::kHome, 0, 0, 74},
    };
    check(std::size(expected) == keymap::kInputCount, "19 inputs");
    const keymap::Keys defaults = keymap::default_keys();
    for (size_t i = 0; i < std::size(expected) && i < keymap::kInputCount; ++i) {
        const keymap::Input& input = keymap::kInputs[i];
        const std::string what = std::string("input ") + expected[i].id;
        check(input.id == expected[i].id, what + " id");
        check(input.button == expected[i].button, what + " button");
        check(input.stick_x == expected[i].stick_x && input.stick_y == expected[i].stick_y, what + " stick");
        check(input.default_key == expected[i].key && defaults[i] == expected[i].key, what + " default key");
        check(!keymap::reserved(defaults[i]), what + " default not reserved");
    }
    // SDL's names for the defaults are the US keys.
    const char* names[] = {"W", "S", "A", "D", "I", "K", "J", "L", "H", ";",
                           "U", "O", "Up", "Down", "Left", "Right", "Return", "Backspace", "Home"};
    for (size_t i = 0; i < std::size(names) && i < keymap::kInputCount; ++i) {
        check(sdl_name(names[i]) == defaults[i], std::string("SDL's name of the default key ") + names[i]);
    }
    check(keymap::reserved(68) && keymap::reserved(41) && keymap::reserved(227) && keymap::reserved(231) &&
              !keymap::reserved(4),
          "reserved keys");
}

void test_good() {
    const keymap::Parsed parsed = keymap::parse("# my keys\n"
                                                "dpad_up = 29\n"
                                                "face_bottom=Return   # by name\n"
                                                "\tl\t=\tleft shift\n"
                                                "home = Keypad 1 # keypad\n"
                                                "zl = 39\n",
                                                sdl_name);
    check(parsed.warnings.empty(), "good file: no warnings");
    check(parsed.assigned == 5, "good file: 5 assigned");
    keymap::Keys expected = keymap::default_keys();
    expected[index_of("dpad_up")] = 29;
    expected[index_of("face_bottom")] = 40;
    expected[index_of("l")] = 225;
    expected[index_of("home")] = 89;
    expected[index_of("zl")] = 39;
    check(parsed.keys == expected, "good file: keys (numbers, names in any case, tabs)");

    // The launcher's file: every input, numbers, comments after them.
    std::string all = "# Tekken Tag Tournament 2: keyboard keys\n";
    for (size_t i = 0; i < keymap::kInputCount; ++i) {
        all += std::string(keymap::kInputs[i].id) + " = " + std::to_string(100 + i) + "  # Key\n";
    }
    const keymap::Parsed full = keymap::parse(all, nullptr);
    check(full.warnings.empty() && full.assigned == keymap::kInputCount, "launcher's file: all 19");
    for (size_t i = 0; i < keymap::kInputCount; ++i) {
        check(full.keys[i] == 100 + i, "launcher's file: key " + std::to_string(i));
    }

    check(keymap::parse("", sdl_name).assigned == 0 && keymap::parse("", sdl_name).keys == keymap::default_keys(),
          "empty text: defaults");
    const keymap::Parsed windows = keymap::parse("\xEF\xBB\xBFr = 5\r\nzr = 6\r\n", sdl_name);
    check(windows.warnings.empty() && windows.keys[index_of("r")] == 5 && windows.keys[index_of("zr")] == 6,
          "byte order mark and CRLF");
    const keymap::Parsed last = keymap::parse("r = 7", sdl_name);
    check(last.assigned == 1 && last.keys[index_of("r")] == 7, "no newline at the end");
}

void test_comments() {
    const keymap::Parsed parsed = keymap::parse("#dpad_up = 5\n"
                                                "   # dpad_down = 5\n"
                                                "l = A#B\n"
                                                "r = 6#7\n"
                                                "zl = 8 #9\n"
                                                "zr = # 10\n",
                                                test_name);
    check(parsed.keys[index_of("dpad_up")] == 26 && parsed.keys[index_of("dpad_down")] == 22, "commented-out lines");
    check(parsed.keys[index_of("l")] == 100, "# inside a word is no comment");
    check(has_warning(parsed, 4, "unknown key \"6#7\""), "6#7 is a name, not 6");
    check(parsed.keys[index_of("zl")] == 8, "comment after a space");
    check(has_warning(parsed, 6, "zr: no key"), "only a comment as value");
    check(parsed.assigned == 2 && parsed.warnings.size() == 2, "comments: counts");
}

void test_bad() {
    const keymap::Parsed parsed = keymap::parse("dpad_up 5\n"      // 1: no =
                                                "= 5\n"            // 2: no id
                                                "jump = 5\n"       // 3: unknown input
                                                "Dpad_Up = 5\n"    // 4: ids are exact
                                                "dpad_up =\n"      // 5: no key
                                                "dpad_up = 1\n"    // 6: a number, not the 1 key
                                                "dpad_up = 0\n"    // 7
                                                "dpad_up = 512\n"  // 8
                                                "dpad_up = 99999999999999999999\n" // 9
                                                "dpad_up = -5\n"   // 10
                                                "dpad_up = Nope\n" // 11
                                                "dpad_up = 0x1d\n" // 12
                                                "dpad_down = 511\n", // 13: the last scancode is fine
                                                sdl_name);
    check(has_warning(parsed, 1, "not \"input = key\""), "line without =");
    check(has_warning(parsed, 2, "unknown input \"\""), "line without id");
    check(has_warning(parsed, 3, "unknown input \"jump\""), "unknown input");
    check(has_warning(parsed, 4, "unknown input \"Dpad_Up\""), "ids are case-sensitive");
    check(has_warning(parsed, 5, "dpad_up: no key"), "no key");
    check(has_warning(parsed, 6, "the 1 key is 30"), "1 is not the 1 key");
    check(has_warning(parsed, 7, "not a key number"), "0");
    check(has_warning(parsed, 8, "not a key number"), "512");
    check(has_warning(parsed, 9, "not a key number"), "huge number");
    check(has_warning(parsed, 10, "unknown key \"-5\""), "negative");
    check(has_warning(parsed, 11, "unknown key \"Nope\""), "unknown name");
    check(has_warning(parsed, 12, "unknown key \"0x1d\""), "hex is no number");
    check(parsed.warnings.size() == 12, "bad file: one warning per bad line");
    keymap::Keys expected = keymap::default_keys();
    expected[index_of("dpad_down")] = 511;
    check(parsed.keys == expected && parsed.assigned == 1, "bad lines keep the defaults");
}

void test_reserved() {
    const keymap::Parsed parsed = keymap::parse("dpad_up = 68\n"
                                                "dpad_down = Escape\n"
                                                "dpad_left = Left GUI\n"
                                                "dpad_right = 231\n"
                                                "plus = F11\n"
                                                "minus = F12\n",
                                                sdl_name);
    for (size_t line = 1; line <= 5; ++line) {
        check(has_warning(parsed, line, "is a reserved key"), "reserved key refused on line " + std::to_string(line));
    }
    keymap::Keys expected = keymap::default_keys();
    expected[index_of("minus")] = 69;
    check(parsed.keys == expected && parsed.assigned == 1, "reserved keys keep the defaults");
}

void test_duplicates() {
    const keymap::Parsed twice = keymap::parse("l = 5\nr = 6\nl = 7\n", sdl_name);
    check(twice.keys[index_of("l")] == 7, "an input given twice takes the later key");
    check(twice.assigned == 2, "an input given twice counts once");
    check(has_warning(twice, 1, "l: given again on line 3"), "an input given twice is told");
    const keymap::Parsed order = keymap::parse("l = 5\nr = Nope\nl = 7\n", sdl_name);
    check(order.warnings.size() == 2 && order.warnings[0].line == 1 && order.warnings[1].line == 2,
          "warnings in line order");
    const keymap::Parsed bad_again = keymap::parse("l = 5\nl = Nope\n", sdl_name);
    check(bad_again.keys[index_of("l")] == 5 && bad_again.assigned == 1, "a bad repeat keeps the good key");

    // One key on two inputs is allowed (it then presses both).
    const keymap::Parsed shared = keymap::parse("face_top = 14\n", sdl_name);
    check(shared.warnings.empty() && shared.keys[index_of("face_top")] == 14 &&
              shared.keys[index_of("face_bottom")] == 14,
          "a key on two inputs");
}

void test_path(const fs::path& scratch) {
    const char* old_home = std::getenv("HOME");
    const std::string home = old_home ? old_home : "";
    setenv("HOME", (scratch / "home").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (scratch / "config").c_str(), 1);
    check(keymap::file_path() == scratch / "config/ttt2/keyboard.ini", "XDG_CONFIG_HOME");
    setenv("XDG_CONFIG_HOME", "relative/config", 1);
    check(keymap::file_path() == scratch / "home/.config/ttt2/keyboard.ini", "a relative XDG_CONFIG_HOME is ignored");
    unsetenv("XDG_CONFIG_HOME");
    check(keymap::file_path() == scratch / "home/.config/ttt2/keyboard.ini", "no XDG_CONFIG_HOME");
    setenv("HOME", home.c_str(), 1);
}

void test_load(const fs::path& scratch) {
    const fs::path file = scratch / "keyboard.ini";
    fs::remove(file);
    keymap::Loaded missing = keymap::load(file, sdl_name);
    check(missing.keys == keymap::default_keys(), "missing file: defaults");
    check(missing.log == std::vector<std::string>{"keyboard: defaults (no " + file.string() + ")"},
          "missing file: log " + joined(missing.log));

    write(file, "l = 5\njump = 4\nr = Return\n");
    const keymap::Loaded loaded = keymap::load(file, sdl_name);
    check(loaded.keys[index_of("l")] == 5 && loaded.keys[index_of("r")] == 40, "file: keys");
    const std::vector<std::string> log{"keyboard: " + file.string() + ":2: unknown input \"jump\", line ignored",
                                       "keyboard: 2 of 19 keys from " + file.string()};
    check(loaded.log == log, "file: log " + joined(loaded.log));

    write(file, "");
    check(keymap::load(file, sdl_name).log == std::vector<std::string>{"keyboard: 0 of 19 keys from " + file.string()},
          "empty file");

    // Never opened: a folder, a pipe (reading one would wait for a writer),
    // a file too large to be a key list.
    const fs::path folder = scratch / "folder.ini";
    fs::create_directories(folder);
    check(joined(keymap::load(folder, sdl_name).log).find("is not a file") != std::string::npos, "a folder");
    const fs::path pipe = scratch / "pipe.ini";
    fs::remove(pipe);
    if (mkfifo(pipe.c_str(), 0600) == 0) {
        const keymap::Loaded fifo = keymap::load(pipe, sdl_name);
        check(fifo.keys == keymap::default_keys() && joined(fifo.log).find("is not a file") != std::string::npos,
              "a pipe");
        fs::remove(pipe);
    }
    const fs::path large = scratch / "large.ini";
    write(large, "l = 5\n" + std::string(70000, '#'));
    const keymap::Loaded big = keymap::load(large, sdl_name);
    check(big.keys == keymap::default_keys() && joined(big.log).find("larger than") != std::string::npos,
          "a large file");
    const fs::path unreadable = scratch / "unreadable.ini";
    write(unreadable, "l = 5\n");
    fs::permissions(unreadable, fs::perms::none);
    if (getuid() != 0) {
        const keymap::Loaded denied = keymap::load(unreadable, sdl_name);
        check(denied.keys == keymap::default_keys() && joined(denied.log).find("cannot read") != std::string::npos,
              "an unreadable file: " + joined(denied.log));
    }
    fs::permissions(unreadable, fs::perms::owner_read | fs::perms::owner_write);

    // Opened, but the read fails (EIO, as from a failing disk): Linux's
    // /proc/self/mem is a regular file whose first bytes are never mapped.
    if (std::ifstream("/proc/self/mem").is_open()) {
        const keymap::Loaded failed = keymap::load("/proc/self/mem", sdl_name);
        const std::string why = std::string("cannot read /proc/self/mem: ") + std::strerror(EIO);
        check(failed.keys == keymap::default_keys() &&
                  failed.log == std::vector<std::string>{"keyboard: defaults (" + why + ")"},
              "a failed read: " + joined(failed.log));
    }
}

void test_watcher(const fs::path& scratch) {
    const fs::path file = scratch / "watched/keyboard.ini";
    fs::remove_all(file.parent_path());
    keymap::Watcher watcher(file, sdl_name);

    // At start-up the file is read whatever it is, here missing.
    std::optional<keymap::Loaded> loaded = watcher.reload();
    check(loaded && loaded->keys == keymap::default_keys(), "start-up without a file");
    check(!watcher.reload(), "still no file: nothing to do");

    fs::create_directories(file.parent_path());
    replace(file, "l = 5\n");
    loaded = watcher.reload();
    check(loaded && loaded->keys[index_of("l")] == 5, "file created");
    check(loaded && loaded->log.back() == "keyboard: 1 of 19 keys from " + file.string(), "reload's log line");
    check(!watcher.reload(), "unchanged: not read again");

    // The launcher's atomic write of a file of the same size, within the
    // same moment: the new inode tells.
    const auto time = fs::last_write_time(file);
    replace(file, "l = 6\n");
    fs::last_write_time(file, time);
    loaded = watcher.reload();
    check(loaded && loaded->keys[index_of("l")] == 6, "renamed over: read again");

    // An editor's write in place: another size, or another time.
    write(file, "l = 70\n");
    loaded = watcher.reload();
    check(loaded && loaded->keys[index_of("l")] == 70, "written in place: read again");
    write(file, "l = 71\n");
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
    loaded = watcher.reload();
    check(loaded && loaded->keys[index_of("l")] == 71, "same size, later time: read again");
    check(!watcher.reload(), "unchanged again");

    fs::remove(file);
    loaded = watcher.reload();
    check(loaded && loaded->keys == keymap::default_keys() &&
              loaded->log == std::vector<std::string>{"keyboard: defaults (no " + file.string() + ")"},
          "deleted: back to the defaults");
    check(!watcher.reload(), "still deleted");
}

} // namespace

int main(int argc, char** argv) {
    // The scratch folder: the build tree's, never the player's config.
    const fs::path scratch = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("keymap_test.d"));
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    test_inputs();
    test_good();
    test_comments();
    test_bad();
    test_reserved();
    test_duplicates();
    test_path(scratch);
    test_load(scratch);
    test_watcher(scratch);

    fs::remove_all(scratch);
    if (g_failures == 0) std::printf("keymap_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
