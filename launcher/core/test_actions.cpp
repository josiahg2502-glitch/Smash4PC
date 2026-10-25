// The launcher's test actions (see test_actions.h).

#include "test_actions.h"

#include "display.h"
#include "keymap.h"

#include <algorithm>
#include <cstdlib>

namespace launcher {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::vector<std::string> split(std::string_view s, char by) {
    std::vector<std::string> out;
    for (;;) {
        const size_t at = s.find(by);
        if (const std::string_view part = trim(s.substr(0, at)); !part.empty()) out.emplace_back(part);
        if (at == std::string_view::npos) return out;
        s.remove_prefix(at + 1);
    }
}

} // namespace

std::string TestAction::label() const {
    std::string out = name;
    if (name != "install" && name != "wait") {
        for (const std::string& a : args) {
            out += '-';
            for (char c : a) out += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ? c : '_';
        }
    }
    return out;
}

std::optional<std::vector<TestAction>> parse_test_actions(std::string_view text, std::string* error) {
    std::vector<TestAction> actions;
    for (const std::string& item : split(text, ';')) {
        TestAction a;
        a.text = item;
        const size_t colon = item.find(':');
        a.name = item.substr(0, colon);
        const std::string rest = colon == std::string::npos ? "" : item.substr(colon + 1);
        bool ok = true;
        if (a.name == "install") {
            const size_t last = rest.rfind(':');
            ok = last != std::string::npos && last > 0 && last + 1 < rest.size();
            if (ok) a.args = {rest.substr(0, last), rest.substr(last + 1)};
        } else if (a.name == "add") {
            a.args = split(rest, ',');
            ok = !a.args.empty();
        } else if (a.name == "remove") {
            a.args = split(rest, ',');
            ok = !a.args.empty();
        } else if (a.name == "uninstall") {
            a.args = {rest};
            ok = rest == "keep" || rest == "all";
        } else if (a.name == "focus") {
            a.args = {rest};
            ok = !rest.empty();
        } else if (a.name == "bind") {
            // bind:<input>:<XKB keycode>[+<XKB keycode>...]
            const size_t at = rest.find(':');
            const std::string keycodes = at == std::string::npos ? "" : rest.substr(at + 1);
            a.args = {rest.substr(0, at), keycodes};
            ok = input_index(a.args[0]) >= 0 && !keycodes.empty();
            for (size_t from = 0; ok && from <= keycodes.size();) {
                const size_t plus = std::min(keycodes.find('+', from), keycodes.size());
                const std::string code = keycodes.substr(from, plus - from);
                char* end = nullptr;
                const long number = std::strtol(code.c_str(), &end, 10);
                ok = !code.empty() && code[0] != '-' && code[0] != '+' && end && *end == '\0' && number > 0 &&
                     number < 1024;
                from = plus + 1;
            }
        } else if (a.name == "fullscreen" || a.name == "interpolation") {
            a.args = {rest};
            ok = rest == "yes" || rest == "no";
        } else if (a.name == "resolution") {
            // (As the file names it, whatever the case given.)
            const auto resolution = display::resolution_named(rest);
            ok = resolution.has_value();
            if (ok) a.args = {std::string(display::name_of(*resolution))};
        } else if (a.name == "wait") {
            char* end = nullptr;
            const double seconds = std::strtod(rest.c_str(), &end);
            a.args = {rest};
            ok = !rest.empty() && end && *end == '\0' && seconds >= 0;
        } else {
            ok = colon == std::string::npos &&
                 (a.name == "update" || a.name == "add-all-base" || a.name == "add-all-owned" || a.name == "play" ||
                  a.name == "quit" || a.name == "expand-dlc" || a.name == "expand-log" || a.name == "show-uninstall" ||
                  a.name == "refresh" || a.name == "settings" || a.name == "back" || a.name == "reset-keys" ||
                  a.name == "update-launcher");
        }
        if (!ok) {
            if (error) *error = "not a test action: " + item;
            return std::nullopt;
        }
        actions.push_back(std::move(a));
    }
    return actions;
}

} // namespace launcher
