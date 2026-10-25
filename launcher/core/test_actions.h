#pragma once

// TTT2_LAUNCHER_TEST_ACTIONS: what the launcher does by itself for tests,
// through the same code as its buttons (only the file pickers and the
// confirmations are left out), one action after another:
//
//   install:<GAME>:<FOLDER>   update   add:<name>[,<name>...]   add-all-base
//   add-all-owned   remove:<name>   uninstall:keep | uninstall:all   play   quit
//
// separated by ';'. (A path holding ':' or ';' cannot be given; GAME ends at
// the last ':'.) For looking at the window in tests there are also
// expand-dlc (opens the DLC list), expand-log (the last task's log),
// show-uninstall (the uninstall dialog, closed again after its picture),
// wait:<seconds>, focus:<id> (the keyboard focus on a button: "play",
// "add:kazumi", ...) and refresh (as the window's Check Again button; the
// widget focused once it is done is logged). update-launcher is the header
// bar's Update (the AppImage replaced by the newest release's and opened:
// its test actions run again there). For the Settings page:
// settings (opens it), back, bind:<input>:<keycode>[+<keycode>...] (that
// input's key clicked, unless it waits for a key already, then those XKB
// keycodes pressed one after another and let go the other way round,
// through the window's key handler: bind:face_bottom:38, Left Alt alone
// bind:zl:64, Alt+Tab bind:zl:64+23), reset-keys (its Restore Defaults), and
// fullscreen:yes|no, resolution:<auto|1280x720|1920x1080|2560x1440|3840x2160|
// 5120x2880> and interpolation:yes|no (that display setting's switch or list
// set, which saves display.ini, then the file read back).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

struct TestAction {
    std::string name;              // "install", "add", ...
    std::vector<std::string> args; // install: GAME, FOLDER; add: names; remove: the name; uninstall: keep/all;
                                   // wait: the seconds; focus: the widget's id; bind: the input, the keycode;
                                   // fullscreen, interpolation: yes/no; resolution: as display.ini names it
    std::string text;              // as given

    // For the picture's file name: the name and short arguments
    // ("add-kazumi-chloe", "uninstall-all"), never paths.
    std::string label() const;
};

// None, with the reason, when an action is not one of the above.
std::optional<std::vector<TestAction>> parse_test_actions(std::string_view text, std::string* error = nullptr);

} // namespace launcher
