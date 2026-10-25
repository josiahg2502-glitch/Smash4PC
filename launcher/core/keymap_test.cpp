// The game's keyboard keys (keymap.h): the key tables, cross-checked against
// Linux's input-event-codes.h and SDL3's SDL_scancode.h when they are there;
// the inputs and their defaults; binding (swapping, refusing); the file's
// text, reading it back (also as written by hand) and saving it (in a folder
// of its own, made and deleted here: links, modes, pipes).
//
// keymap_test [FOLDER [input-event-codes.h [SDL_scancode.h]]]

#include "keymap.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace {

using namespace launcher;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void write(const fs::path& file, const std::string& text) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << text;
}

int key(const Keymap& map, const char* id) { return map.key(input_index(id)); }

void test_tables() {
    std::set<int> usages, codes;
    int last = 0;
    for (const Key& k : keys()) {
        const std::string what = std::string("keys: ") + k.name;
        check(usages.insert(k.usage).second, what + ": its usage once");
        check(codes.insert(k.evdev).second, what + ": its evdev code once");
        check(k.usage > last, what + ": in usage order");
        last = k.usage;
        check(k.name && *k.name, what + ": a name");
        check(key_by_usage(k.usage) == &k && key_by_evdev(k.evdev) == &k, what + ": found both ways");
        check(!sdl_name(k.usage).empty() && usage_by_name(sdl_name(k.usage)) == k.usage, what + ": SDL names it");
    }
    check(!key_by_usage(0) && !key_by_usage(1000) && !key_by_evdev(0) && !key_by_evdev(-1), "keys: none of nothing");
    check(key_by_evdev(30)->usage == 4 && key_by_evdev(1)->usage == 41 && key_by_evdev(103)->usage == 82 &&
              key_by_evdev(126)->usage == 231 && key_by_evdev(86)->usage == 100,
          "keys: A, Esc, Up, Right Super, the ISO key");

    // By SDL's names (SDL_GetScancodeFromName: in any case), as the game:
    // also keys the launcher cannot take from the keyboard.
    for (const auto& [name, usage] : std::map<std::string, int>{{"Return", 40},
                                                                {"return", 40},
                                                                {"Escape", 41},
                                                                {"left shift", 225},
                                                                {"Left GUI", 227},
                                                                {"Keypad 1", 89},
                                                                {"a", 4},
                                                                {"Backspace", 42},
                                                                {"PageUp", 75},
                                                                {"PrintScreen", 70},
                                                                {"Numlock", 83},
                                                                {"Application", 101},
                                                                {";", 51},
                                                                {"International 1", 135},
                                                                {"Space", 44},
                                                                {"Menu", 118},
                                                                {"keypad ,", 133},
                                                                {"MUTE", 127},
                                                                {"AC Back", 282},
                                                                {"EndCall", 290},
                                                                {"Keypad ||", 202}}) {
        check(usage_by_name(name) == usage, "keys: by the name " + name);
    }
    // (Not by the launcher's own names where SDL's differ: the game would
    // not know them.)
    for (const char* name : {"Enter", "Esc", "Page Up", "LeftShift", "keypad1", "Left Super", "Ro", " Space", ""}) {
        check(usage_by_name(name) == 0, std::string("keys: no key named ") + name);
    }
    check(!usage_by_name("nonsense") && !usage_by_name("Keypad"), "keys: no key of other names");
    check(usage_by_name("Return") == 40 && sdl_name(158) == "Return", "keys: a name twice is the first usage's");
    std::set<int> named;
    for (const Named& n : sdl_names()) {
        check(n.usage >= kFirstKey && n.usage < kKeyCount && named.insert(n.usage).second,
              std::string("keys: SDL names ") + n.name + " once");
    }
    check(named.size() == 246, "keys: SDL's 246 names");
    check(key_name(29) == "Z" && key_name(40) == "Enter" && key_name(282) == "AC Back" && key_name(118) == "Key 118" &&
              key_name(300) == "Key 300",
          "keys: the names shown (SDL's Menu is not the Menu key)");
    for (int usage = 0; usage < 300; ++usage) {
        const bool modifier = usage == 224 || usage == 225 || usage == 226 || usage == 228 || usage == 229 ||
                              usage == 230;
        check(modifier_key(usage) == modifier, "keys: modifier " + std::to_string(usage));
    }

    for (int usage : {68, 41, 227, 231}) {
        check(reserved_key(usage) != nullptr, "keys: reserved " + std::to_string(usage));
    }
    for (const Key& k : keys()) {
        if (k.usage != 68 && k.usage != 41 && k.usage != 227 && k.usage != 231) {
            check(!reserved_key(k.usage), std::string("keys: not reserved ") + k.name);
        }
    }
}

// The table against the headers: the key of each line is the same key in
// both, by their names (KEY_SEMICOLON is SDL_SCANCODE_SEMICOLON), except
// where the two name it differently (listed here).
void test_headers(const fs::path& linux_header, const fs::path& sdl_header) {
    std::error_code ec;
    if (!fs::exists(linux_header, ec) || !fs::exists(sdl_header, ec)) {
        std::printf("keymap_test: %s or %s is missing: the tables are not cross-checked\n", linux_header.c_str(),
                    sdl_header.c_str());
        return;
    }
    // KEY_* codes, also those defined as another KEY_* (KEY_HANGUEL).
    std::map<std::string, std::string> defined;
    const std::regex define(R"(^#define\s+(KEY_\w+)\s+(\w+))");
    std::istringstream linux_text(read(linux_header));
    for (std::string line; std::getline(linux_text, line);) {
        std::smatch m;
        if (std::regex_search(line, m, define)) defined[m[1]] = m[2];
    }
    std::map<int, std::set<std::string>> evdev_names;
    for (const auto& [name, value] : defined) {
        std::string v = value;
        for (int i = 0; i < 4 && defined.count(v); ++i) v = defined[v];
        char* end = nullptr;
        const long code = std::strtol(v.c_str(), &end, 0);
        if (end && *end == '\0') evdev_names[static_cast<int>(code)].insert(name.substr(4));
    }
    std::map<int, std::set<std::string>> sdl_names;
    const std::regex scancode(R"(^\s*SDL_SCANCODE_(\w+)\s*=\s*(\d+))");
    std::istringstream sdl_text(read(sdl_header));
    for (std::string line; std::getline(sdl_text, line);) {
        std::smatch m;
        if (std::regex_search(line, m, scancode)) sdl_names[std::stoi(m[2])].insert(m[1]);
    }
    check(evdev_names.size() > 200 && sdl_names.size() > 200, "headers: read");

    const auto normal = [](std::string name) {
        std::string out;
        for (char c : name) {
            if (c != '_') out += c;
        }
        return out;
    };
    const std::set<std::pair<std::string, std::string>> different = {
        {"ESC", "ESCAPE"},           {"ENTER", "RETURN"},          {"EQUAL", "EQUALS"},
        {"LEFTBRACE", "LEFTBRACKET"}, {"RIGHTBRACE", "RIGHTBRACKET"}, {"DOT", "PERIOD"},
        {"SYSRQ", "PRINTSCREEN"},    {"NUMLOCK", "NUMLOCKCLEAR"},   {"KPASTERISK", "KPMULTIPLY"},
        {"KPSLASH", "KPDIVIDE"},     {"KPDOT", "KPPERIOD"},         {"KPEQUAL", "KPEQUALS"},
        {"102ND", "NONUSBACKSLASH"}, {"COMPOSE", "APPLICATION"},    {"RO", "INTERNATIONAL1"},
        {"KATAKANAHIRAGANA", "INTERNATIONAL2"}, {"YEN", "INTERNATIONAL3"}, {"HENKAN", "INTERNATIONAL4"},
        {"MUHENKAN", "INTERNATIONAL5"}, {"HANGEUL", "LANG1"},       {"HANJA", "LANG2"},
        {"LEFTCTRL", "LCTRL"},       {"LEFTSHIFT", "LSHIFT"},       {"LEFTALT", "LALT"},
        {"LEFTMETA", "LGUI"},        {"RIGHTCTRL", "RCTRL"},        {"RIGHTSHIFT", "RSHIFT"},
        {"RIGHTALT", "RALT"},        {"RIGHTMETA", "RGUI"},
    };
    std::set<std::pair<std::string, std::string>> used;
    for (const Key& k : keys()) {
        const auto e = evdev_names.find(k.evdev);
        const auto s = sdl_names.find(k.usage);
        if (e == evdev_names.end() || s == sdl_names.end()) {
            check(false, std::string("headers: ") + k.name + " is in both");
            continue;
        }
        bool same = false;
        for (const std::string& en : e->second) {
            for (const std::string& sn : s->second) {
                if (normal(en) == normal(sn)) same = true;
                else if (different.count({normal(en), normal(sn)})) {
                    same = true;
                    used.insert({normal(en), normal(sn)});
                }
            }
        }
        check(same, std::string("headers: ") + k.name + " is KEY_" + *e->second.begin() + " and SDL_SCANCODE_" +
                        *s->second.begin());
    }
    check(used.size() == different.size(), "headers: every exception is one");
}

void test_inputs() {
    check(inputs().size() == 19, "inputs: 19");
    const std::map<std::string, int> defaults = {
        {"dpad_up", 26},    {"dpad_down", 22}, {"dpad_left", 4},   {"dpad_right", 7},  {"face_top", 12},
        {"face_bottom", 14}, {"face_left", 13}, {"face_right", 15}, {"l", 11},          {"r", 51},
        {"zl", 24},         {"zr", 18},        {"stick_up", 82},   {"stick_down", 81}, {"stick_left", 80},
        {"stick_right", 79}, {"plus", 40},     {"minus", 42},      {"home", 74}};
    std::set<int> keys_used;
    for (int i = 0; i < kInputs; ++i) {
        const Input& in = inputs()[i];
        const auto d = defaults.find(in.id);
        check(d != defaults.end() && d->second == in.key, std::string("inputs: the default of ") + in.id);
        check(input_index(in.id) == i, std::string("inputs: found ") + in.id);
        check(key_by_usage(in.key) && !reserved_key(in.key), std::string("inputs: usable default of ") + in.id);
        keys_used.insert(in.key);
    }
    check(keys_used.size() == kInputs, "inputs: different defaults");
    check(input_index("nope") == -1 && input_index("") == -1, "inputs: no other ids");
    const char* groups[] = {"D-pad", "Face buttons", "Shoulders", "Left stick", "System"};
    int group = 0;
    for (int i = 0; i < kInputs; ++i) {
        if (inputs()[i].group != std::string(groups[group])) ++group;
        check(group < 5 && inputs()[i].group == std::string(groups[group]), "inputs: in their groups' order");
    }
}

void test_bind() {
    Keymap map;
    check(map.is_default(), "bind: defaults first");
    // Bottom button (K) to 9: nobody has 9.
    Keymap::Bound b = map.bind(input_index("face_bottom"), 38);
    check(b.ok && b.swapped == -1 && key(map, "face_bottom") == 38 && !map.is_default(), "bind: a free key");
    // Top button (I) to 9: Bottom button had it, and gets I.
    b = map.bind(input_index("face_top"), 38);
    check(b.ok && b.swapped == input_index("face_bottom") && key(map, "face_top") == 38 &&
              key(map, "face_bottom") == 12,
          "bind: swaps");
    b = map.bind(input_index("face_top"), 38);
    check(b.ok && b.swapped == -1 && key(map, "face_top") == 38, "bind: its own key again");
    const Keymap before = map;
    for (int usage : {68, 41, 227, 231}) {
        b = map.bind(input_index("l"), usage);
        check(!b.ok && !b.why.empty() && map == before, "bind: refuses " + std::to_string(usage));
    }
    b = map.bind(input_index("l"), 0);
    check(!b.ok && map == before, "bind: refuses no key");
    b = map.bind(input_index("l"), 300);
    check(!b.ok && map == before, "bind: refuses an unknown key");
    b = map.bind(-1, 4);
    check(!b.ok && map == before, "bind: refuses no input");
    // Every key is still one input's.
    std::set<int> used;
    for (int i = 0; i < kInputs; ++i) used.insert(map.key(i));
    check(used.size() == kInputs, "bind: no key twice");
    map.reset();
    check(map.is_default() && map == Keymap(), "bind: reset");
}

void test_file(const fs::path& root) {
    ::umask(022);
    Keymap map;
    map.bind(input_index("face_top"), 38);
    map.bind(input_index("plus"), 44);
    map.bind(input_index("zl"), 225);
    const std::string text = map.text();
    check(text.rfind("# Tekken Tag Tournament 2", 0) == 0, "file: its header");
    check(text.find("ttt2") == std::string::npos, "file: players' names only");
    check(text.find("face_top    = 38   # 9\n") != std::string::npos, "file: a line, with its key's name");
    check(text.find("zl          = 225  # Left Shift\n") != std::string::npos, "file: a modifier");
    check(text.find("\n# Face buttons\n") != std::string::npos, "file: the groups");
    for (const Input& in : inputs()) {
        check(text.find("\n" + std::string(in.id) + " ") != std::string::npos, std::string("file: has ") + in.id);
    }
    Keymap::Notes notes;
    check(Keymap::parse(text, &notes) == map && notes.warnings.empty() && notes.as_shown(), "file: read back");

    notes = {};
    const Keymap hand = Keymap::parse("# by hand\n"
                                      "  dpad_up=Keypad 8   # a comment\n"
                                      "face_bottom = return\n"
                                      "face_top = 0x16\n"
                                      "l = 68\n"
                                      "r = Escape\n"
                                      "sideways = 4\n"
                                      "no equals here\n"
                                      "home = # nothing\n"
                                      "zr = 300\n"
                                      "\r\n"
                                      "minus = Left GUI\n",
                                      &notes);
    check(key(hand, "dpad_up") == 96 && key(hand, "face_bottom") == 40, "file: names");
    check(key(hand, "face_top") == 12 && key(hand, "l") == 11 && key(hand, "r") == 51 && key(hand, "home") == 74 &&
              key(hand, "minus") == 42,
          "file: what is not a usable key keeps the default");
    check(key(hand, "zr") == 300, "file: a number the launcher does not know is kept");
    // (Enter is plus's by default: face_bottom, given it, keeps it, and plus
    // gets K, as if they had swapped.)
    check(key(hand, "face_bottom") == 40 && key(hand, "plus") == 14, "file: a key given and a default");
    std::set<int> used;
    for (int i = 0; i < kInputs; ++i) used.insert(hand.key(i));
    check(used.size() == kInputs, "file: no key twice after reading");
    check(notes.warnings.size() == 7, "file: warnings (" + std::to_string(notes.warnings.size()) + ")");
    // (The game ignores those lines too; but it presses Enter for both.)
    check(notes.shared == std::vector<std::string>{"Enter: Bottom button and + (Start)"} && notes.error.empty(),
          "file: what is not shown as the game has it");

    // A key given twice: the earlier line's input keeps it, and the page
    // says that the game presses both.
    notes = {};
    const Keymap twice = Keymap::parse("face_bottom = 26\ndpad_up = 26\n", &notes);
    check(key(twice, "face_bottom") == 26 && key(twice, "dpad_up") == 14 && notes.warnings.empty() &&
              notes.shared == std::vector<std::string>{"W: Bottom button and D-pad Up"} && !notes.as_shown(),
          "file: given twice");
    const Keymap thrice = Keymap::parse("face_bottom = 26\ndpad_up = 26\nface_left = 14\n");
    check(key(thrice, "dpad_up") == 5,
          "file: given twice, neither default free: the first key free (B: A is D-pad Left's)");
    // Given to one input while another has it by default: the game presses both.
    notes = {};
    const Keymap taken = Keymap::parse("face_top = 14\n", &notes);
    check(key(taken, "face_top") == 14 && key(taken, "face_bottom") == 12 &&
              notes.shared == std::vector<std::string>{"K: Top button and Bottom button"},
          "file: a default taken");

    // Written by hand as the game reads it (runtime/src/host/keymap.cpp).
    struct Hand {
        const char* text;
        int dpad_up; // 26: the default
    };
    for (const Hand& h : std::initializer_list<Hand>{
             {"\xEF\xBB\xBF" "dpad_up = 5", 5}, // (a byte order mark)
             {"dpad_up = 5\f", 5},
             {"dpad_up = 5\v", 5},
             {"\vdpad_up\f=\v5", 5},
             {"dpad_up = 5\r# a comment", 5},
             {"dpad_up = 5\v# a comment", 5},
             {"dpad_up = 5#no comment", 26},
             {"dpad_up = 1", 26}, // (1 to 3 are no keys)
             {"dpad_up = 3", 26},
             {"dpad_up = 4", 4},
             {"dpad_up = 0004", 4},
             {"dpad_up = 511", 511},
             {"dpad_up = 512", 26},
             {"dpad_up = 99999999999999999999", 26},
             {"dpad_up = +5", 26},
             {"dpad_up = Menu", 118},
             {"dpad_up = keypad ,", 133},
             {"dpad_up = AC Back", 282},
             {"dpad_up = Keypad #", 26}, // (" #" starts a comment: "Keypad")
             {"dpad_up = Keypad = (AS400)", 134},
             {"dpad_up = =", 46},
             {"dpad_up = 5\ndpad_up = 6", 6}, // (the later line)
         }) {
        notes = {};
        const Keymap read = Keymap::parse(h.text, &notes);
        check(key(read, "dpad_up") == h.dpad_up, std::string("file: by hand \"") + h.text + "\": " +
                                                     std::to_string(key(read, "dpad_up")));
    }

    const fs::path file = root / "config" / "ttt2" / "keyboard.ini";
    notes = {};
    check(Keymap::load(file, &notes) == Keymap() && notes.as_shown() && notes.warnings.empty(),
          "file: none is the defaults");
    std::string error;
    check(map.save(file, &error), "file: saved (" + error + ")");
    check(read(file) == text, "file: as its text");
    check(Keymap::load(file) == map, "file: loaded");
    struct stat st{};
    check(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644, "file: readable by others");
    const ino_t inode = st.st_ino;
    Keymap other;
    check(other.save(file, &error) && Keymap::load(file) == other, "file: saved over");
    check(::stat(file.c_str(), &st) == 0 && st.st_ino != inode, "file: a new file each time (renamed over)");
    int entries = 0;
    for (const auto& e : fs::directory_iterator(file.parent_path())) {
        (void)e;
        ++entries;
    }
    check(entries == 1, "file: nothing else left beside it");
    write(root / "blocked", "a file, not a folder");
    check(!map.save(root / "blocked" / "keyboard.ini", &error) && !error.empty(), "file: cannot be saved: says why");

    // A link stays one (the setting kept elsewhere is what changes), and the
    // file keeps its mode; a new one gets the umask's.
    const fs::path kept = root / "dotfiles" / "keyboard.ini";
    write(kept, "dpad_up = 5\n");
    ::chmod(kept.c_str(), 0600);
    const fs::path linked = root / "linked" / "ttt2" / "keyboard.ini";
    fs::create_directories(linked.parent_path());
    fs::create_symlink("../../dotfiles/keyboard.ini", linked);
    check(map.save(linked, &error), "file: saved through a link (" + error + ")");
    check(fs::is_symlink(linked) && Keymap::load(kept) == map && Keymap::load(linked) == map,
          "file: the link kept, the file it leads to written");
    check(::stat(kept.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "file: its mode kept");
    int beside = 0;
    for (const auto& e : fs::directory_iterator(kept.parent_path())) {
        (void)e;
        ++beside;
    }
    check(beside == 1, "file: nothing left beside the file a link leads to");
    const fs::path dangling = root / "dangling" / "keyboard.ini";
    fs::create_directories(dangling.parent_path());
    fs::create_symlink("elsewhere.ini", dangling);
    check(map.save(dangling, &error) && fs::is_symlink(dangling) && Keymap::load(dangling.parent_path() /
                                                                                 "elsewhere.ini") == map,
          "file: a link to nothing: the file it names is made");
    ::umask(077);
    const fs::path private_file = root / "private" / "keyboard.ini";
    check(map.save(private_file, &error) && ::stat(private_file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
          "file: a new file with the umask's mode");
    ::umask(022);

    // Reading only a file (as the game): never a pipe, which would block,
    // a folder or a device; nor a large one. Each says why on the page.
    const fs::path pipe = root / "pipe" / "keyboard.ini";
    fs::create_directories(pipe.parent_path());
    check(::mkfifo(pipe.c_str(), 0644) == 0, "file: a pipe made");
    notes = {};
    check(Keymap::load(pipe, &notes) == Keymap() && notes.error == "it is not a file", "file: a pipe is not read");
    notes = {};
    check(Keymap::load(root / "pipe", &notes) == Keymap() && notes.error == "it is not a file",
          "file: a folder is not read");
    notes = {};
    check(Keymap::load("/dev/zero", &notes) == Keymap() && notes.error == "it is not a file",
          "file: a device is not read");
    const fs::path large = root / "large" / "keyboard.ini";
    write(large, "dpad_up = 5\n" + std::string(70000, '#'));
    notes = {};
    check(Keymap::load(large, &notes) == Keymap() && notes.error == "it is larger than 64 KiB",
          "file: a large file is not read");
    if (::geteuid() != 0) {
        const fs::path locked = root / "locked" / "keyboard.ini";
        write(locked, "dpad_up = 5\n");
        ::chmod(locked.c_str(), 0);
        notes = {};
        check(Keymap::load(locked, &notes) == Keymap() && notes.error == std::strerror(EACCES),
              "file: one that cannot be read says why (" + notes.error + ")");
        check(map.save(locked, &error) && ::stat(locked.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 &&
                  Keymap::load(locked) == map,
              "file: saved over one that could not be read: its owner can read it again");
    }

    // The launcher's Places: $XDG_CONFIG_HOME when absolute, else ~/.config.
    const char* saved = std::getenv("XDG_CONFIG_HOME");
    const std::string saved_config = saved ? saved : "";
    ::setenv("XDG_CONFIG_HOME", (root / "x").c_str(), 1);
    check(Places::from_environment().keyboard() == root / "x" / "ttt2" / "keyboard.ini", "file: in XDG_CONFIG_HOME");
    ::setenv("XDG_CONFIG_HOME", "relative", 1);
    check(Places::from_environment().keyboard() == Places::from_environment().home / ".config/ttt2/keyboard.ini",
          "file: not in a relative XDG_CONFIG_HOME");
    if (saved) ::setenv("XDG_CONFIG_HOME", saved_config.c_str(), 1);
    else ::unsetenv("XDG_CONFIG_HOME");
}

} // namespace

int main(int argc, char** argv) {
    const fs::path base = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path()).lexically_normal();
    fs::create_directories(base);
    std::string pattern = (base / "keymap_test.XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
        std::printf("FAIL cannot make a folder in %s\n", base.c_str());
        return 1;
    }
    const fs::path root = pattern;
    test_tables();
    test_headers(argc > 2 ? argv[2] : "/usr/include/linux/input-event-codes.h",
                 argc > 3 ? argv[3] : "/usr/include/SDL3/SDL_scancode.h");
    test_inputs();
    test_bind();
    test_file(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("keymap_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
