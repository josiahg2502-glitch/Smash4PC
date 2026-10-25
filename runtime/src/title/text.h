#pragma once

// The title's text banks ("NTXB" entries of data003: per language, the
// texts of a part of the game). Each text is a record: its key
// ("common_4"), padded to four with at least one zero byte; two bytes; its
// length in characters (big-endian); its UTF-8 text, then the English text
// it was translated from (cut to 32 bytes), each padded to four with at
// least two zero bytes. The bank's index points at the records, so a text
// can change in place within its space.

#include <cstdint>
#include <set>
#include <vector>

namespace cafe::title::text {

// The code points the texts get for the buttons, by the button's place:
// symbols no text of the game uses, which both fonts the texts are drawn
// with have (the main font's own icons, E024 and on, are missing from the
// plain one). Their glyphs become the icons (presentation.cpp).
constexpr char32_t kRightButton = 0x00A3, kBottomButton = 0x00A5, kTopButton = 0x00A2, kLeftButton = 0x00AA; // £ ¥ ¢ ª
constexpr char32_t kStartButton = 0xFF3C, kSelectButton = 0x00B3, kPad = 0x00BB;                             // ＼ ³ »

bool is_bank(const std::vector<uint8_t>& contents);

// Rewrites the Wii U's button names in the bank's texts (the + and -
// buttons, A B X Y, the +Control Pad; in English, French, Italian, German
// and Spanish) to the buttons' icons, which follow the controller. Returns
// how many texts changed.
int show_button_icons(std::vector<uint8_t>& bank);

// Adds the code points of the bank's texts to `used`.
void code_points(const std::vector<uint8_t>& bank, std::set<char32_t>& used);

} // namespace cafe::title::text
