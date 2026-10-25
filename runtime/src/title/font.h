#pragma once

// The title's fonts: a glyph table ("NFH") and a texture holding the
// glyphs. Button icons are glyphs too, at private-use code points; a code
// names a button by its place on the pad (E024: the right face button, the
// Wii U's A), so icons for another controller go in the same places.
//
// A glyph table: a header (the font's size in pixels at +4, the number of
// glyphs at +8), hash buckets, then from 0x450 a 32-byte record per glyph,
// sorted by code point. A record holds the glyph's box in the texture, its
// placement on the line (in 64ths of a pixel), its line height, its code
// point (big-endian), and bookkeeping left alone here. Some fonts' tables
// are little-endian (the main font's), others big-endian.

#include "title/image.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace cafe::title::font {

struct Glyph {
    uint32_t x = 0, y = 0, width = 0, height = 0; // in the texture
};

// Where a glyph sits on the line, in pixels: its box's left edge from the
// pen, its top above the baseline, and how far the pen moves on.
struct Placement {
    double left = 0, top = 0, advance = 0;
};

// Code point -> glyph.
std::optional<std::map<uint16_t, Glyph>> parse(const std::vector<uint8_t>& nfh);

// Gives the glyph for `code` the box, placement and line height of the
// glyph for `like`. False if the table lacks either.
bool copy_glyph(std::vector<uint8_t>& nfh, uint16_t code, uint16_t like);

// Gives the glyph for `code` another box and placement. False if the table
// lacks it.
bool set_glyph(std::vector<uint8_t>& nfh, uint16_t code, const Glyph& box, const Placement& placement);

struct Font {
    std::map<uint16_t, Glyph> glyphs;
    Rgba texture;
};

// The font's texture with icons that name no console: the face buttons as
// four dots with the pressed button's lit (the font's own Tekken button
// pictures), Start and Select as arrows right and left.
Rgba neutral_icons(const Font& font);

// The font's texture with PlayStation-style icons, drawn here: the face
// buttons' symbols, L1 R1 L2 R2, Start and Select, the pad's four arrows,
// and the Tekken button pictures' dots in the symbols' colours.
Rgba playstation_icons(const Font& font);

} // namespace cafe::title::font
