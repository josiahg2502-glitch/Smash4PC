#include "title/text.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <string_view>

namespace cafe::title::text {
namespace {

// In the rewrites, these bytes stand for icons.
constexpr char kA = '\x01', kB = '\x02', kX = '\x03', kY = '\x04', kStart = '\x05', kSelect = '\x06', kCross = '\x07';

char32_t icon(char c) {
    switch (c) {
    case kA: return kRightButton; // the Wii U's A is the right face button
    case kB: return kBottomButton;
    case kX: return kTopButton;
    case kY: return kLeftButton;
    case kStart: return kStartButton;
    case kSelect: return kSelectButton;
    default: return kPad;
    }
}

struct Rewrite {
    std::string_view from, to;
};

// The prompts to press Start, whole (the title screen's and the attract
// mode's): their translations put the icon where the phrasing allows.
constexpr Rewrite kPrompts[] = {
    {"PRESS THE + BUTTON", "PRESS \x05 BUTTON"},
    {"APPUYER SUR LE BOUTON +", "APPUYER SUR LE BOUTON \x05"},
    {"PREMI IL PULSANTE +", "PREMI IL PULSANTE \x05"},
    {"PLUS/START-KNOPF DR\xC3\x9C" "CKEN", "\x05-KNOPF DR\xC3\x9C" "CKEN"},
    {"PULSA EL BOT\xC3\x93N +", "PULSA \x05"},
    {"\xD0\x9D\xD0\x90\xD0\x96\xD0\x90\xD0\xA2\xD0\xAC START", "\xD0\x9D\xD0\x90\xD0\x96\xD0\x90\xD0\xA2\xD0\xAC \x05"},
};

// Button names within texts, each language's: the name becomes the icon
// ("press the X Button" -> "press the [X]"), which is always shorter. A
// name starting or ending with a letter only matches as a whole word.
constexpr Rewrite kNames[] = {
    // English
    {"A, B, X, and Y Buttons", "\x01, \x02, \x03, and \x04"},
    {"+ Control Pad", "\x07"},
    {"+Control Pad", "\x07"},
    {"+ Button", "\x05"},
    {"- Button", "\x06"},
    {"A Button", "\x01"},
    {"B Button", "\x02"},
    {"X Button", "\x03"},
    {"Y Button", "\x04"},
    // French
    {"boutons A, B, X et Y", "\x01, \x02, \x03 et \x04"},
    {"manette +", "\x07"},
    {"bouton +", "\x05"},
    {"touche +", "\x05"},
    {"bouton -", "\x06"},
    {"bouton A", "\x01"},
    {"bouton B", "\x02"},
    {"bouton X", "\x03"},
    {"bouton Y", "\x04"},
    // Italian
    {"pulsantiera +", "\x07"},
    {"pulsante +", "\x05"},
    {"pulsante -", "\x06"},
    {"pulsante A", "\x01"},
    {"pulsante B", "\x02"},
    {"pulsante X", "\x03"},
    {"pulsante Y", "\x04"},
    // German
    {"A-, B-, X- oder Y-Knopf", "\x01, \x02, \x03 oder \x04"},
    {"PLUS/START-Knopf", "\x05"},
    {"Minus-Knopf", "\x06"},
    {"A-Knopf", "\x01"},
    {"B-Knopf", "\x02"},
    {"X-Knopf", "\x03"},
    {"Y-Knopf", "\x04"},
    // Spanish
    {"botones A, B, X e Y", "\x01, \x02, \x03 e \x04"},
    {"bot\xC3\xB3n +", "\x05"},
    {"Bot\xC3\xB3n +", "\x05"},
    {"bot\xC3\xB3n -", "\x06"},
    {"Bot\xC3\xB3n -", "\x06"},
    {"bot\xC3\xB3n A", "\x01"},
    {"Bot\xC3\xB3n A", "\x01"},
    {"bot\xC3\xB3n B", "\x02"},
    {"Bot\xC3\xB3n B", "\x02"},
    {"bot\xC3\xB3n X", "\x03"},
    {"Bot\xC3\xB3n X", "\x03"},
    {"bot\xC3\xB3n Y", "\x04"},
    {"Bot\xC3\xB3n Y", "\x04"},
};

// A text's space: its bytes and at least two zero bytes, to four. A key's:
// at least one zero byte.
size_t text_space(size_t bytes) { return (bytes + 2 + 3) & ~size_t{3}; }
size_t key_space(size_t bytes) { return (bytes + 1 + 3) & ~size_t{3}; }

// Letters, digits and anything not ASCII (letters of other alphabets).
bool wordlike(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u);
}

size_t characters(std::string_view utf8) {
    return static_cast<size_t>(std::count_if(utf8.begin(), utf8.end(), [](char c) { return (c & 0xC0) != 0x80; }));
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// `to` with its icon bytes as the icons' UTF-8.
std::string expand(std::string_view to) {
    std::string out;
    for (const char c : to) {
        if (c >= kA && c <= kCross) append_utf8(out, icon(c));
        else out += c;
    }
    return out;
}

// The text with button names as icons; unchanged if it names none.
std::string rewrite(std::string_view text) {
    for (const Rewrite& r : kPrompts) {
        if (text == r.from) return expand(r.to);
    }
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const Rewrite* match = nullptr;
        for (const Rewrite& r : kNames) {
            if (text.compare(i, r.from.size(), r.from) != 0) continue;
            if (wordlike(r.from.front()) && i > 0 && wordlike(text[i - 1])) continue;
            const size_t end = i + r.from.size();
            if (wordlike(r.from.back()) && end < text.size() && wordlike(text[end])) continue;
            match = &r;
            break;
        }
        if (match) {
            out += expand(match->to);
            i += match->from.size();
        } else {
            out += text[i++];
        }
    }
    return out;
}

// The zero-terminated string at `at` (empty if it runs off the end).
std::string_view string_at(const std::vector<uint8_t>& bank, size_t at) {
    if (at >= bank.size()) return {};
    const auto* begin = reinterpret_cast<const char*>(bank.data() + at);
    const void* end = std::memchr(begin, 0, bank.size() - at);
    return end ? std::string_view(begin, static_cast<size_t>(static_cast<const char*>(end) - begin)) : std::string_view{};
}

// A key: letters, '_', digits ("common_4", "Manu_627"), after the previous
// record's padding.
bool key_at(const std::vector<uint8_t>& bank, size_t at, std::string_view& key) {
    if (at == 0 || bank[at - 1] != 0) return false;
    key = string_at(bank, at);
    const size_t underscore = key.find('_');
    if (underscore == 0 || underscore == std::string_view::npos || underscore + 1 >= key.size()) return false;
    const auto letters = key.substr(0, underscore), digits = key.substr(underscore + 1);
    return std::all_of(letters.begin(), letters.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); }) &&
           std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

} // namespace

bool is_bank(const std::vector<uint8_t>& contents) {
    return contents.size() >= 0x30 && std::memcmp(contents.data(), "NTXB", 4) == 0;
}

namespace {

// Calls `visit(header, start, text)` for each text of the bank, in order;
// `visit` returns where to go on looking (past what it wrote) or 0.
template <typename Visit>
void each_text(const std::vector<uint8_t>& bank, Visit&& visit) {
    if (!is_bank(bank)) return;
    for (size_t at = 0x30; at + 8 < bank.size(); at += 4) {
        std::string_view key;
        if (!key_at(bank, at, key)) continue;
        const size_t header = at + key_space(key.size());
        const size_t start = header + 4;
        if (start >= bank.size()) continue;
        const std::string_view text = string_at(bank, start);
        if ((size_t{bank[header + 2]} << 8 | bank[header + 3]) != characters(text)) continue; // not a record
        if (const size_t next = visit(header, start, text)) at = next - 4;
    }
}

} // namespace

int show_button_icons(std::vector<uint8_t>& bank) {
    int changed = 0;
    each_text(bank, [&](size_t header, size_t start, std::string_view text) -> size_t {
        const std::string to = rewrite(text);
        const size_t room = text_space(text.size());
        if (to == text || to.size() + 2 > room || start + room > bank.size()) return 0;
        std::memset(&bank[start], 0, room);
        std::memcpy(&bank[start], to.data(), to.size());
        const size_t count = characters(to);
        bank[header + 2] = static_cast<uint8_t>(count >> 8);
        bank[header + 3] = static_cast<uint8_t>(count);
        ++changed;
        return start + room; // past the text
    });
    return changed;
}

void code_points(const std::vector<uint8_t>& bank, std::set<char32_t>& used) {
    each_text(bank, [&](size_t, size_t, std::string_view text) -> size_t {
        for (size_t i = 0; i < text.size();) {
            const auto b = static_cast<unsigned char>(text[i]);
            const size_t length = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
            char32_t c = length == 1 ? b : b & (0x3F >> (length - 1));
            for (size_t k = 1; k < length && i + k < text.size(); ++k) c = c << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            used.insert(c);
            i += length;
        }
        return 0;
    });
}

} // namespace cafe::title::text
