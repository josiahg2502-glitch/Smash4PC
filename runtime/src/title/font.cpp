#include "title/font.h"

#include "title/draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <tuple>

namespace cafe::title::font {
namespace {

uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }
uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

constexpr size_t kRecordSize = 32;

// A glyph table's records: how many, their byte order. A record's fields,
// from +4: x, y (16 bits each), left, top, advance (signed, 64ths of a
// pixel), 4 zero bytes, line height (as the placement), width, height (8
// bits each), code point.
struct Layout {
    static constexpr size_t kRecords = 0x450, kFields = 4;
    enum : size_t { kX = 0, kY = 2, kLeft = 4, kTop = 6, kAdvance = 8, kWidth = 16, kHeight = 17, kCode = 18 };
    bool big_endian = false;
    uint32_t count = 0;

    uint8_t* record(std::vector<uint8_t>& nfh, uint32_t i) const { return &nfh[kRecords + size_t{i} * kRecordSize + kFields]; }
    const uint8_t* record(const std::vector<uint8_t>& nfh, uint32_t i) const {
        return &nfh[kRecords + size_t{i} * kRecordSize + kFields];
    }
    uint16_t get16(const uint8_t* p) const {
        return static_cast<uint16_t>(big_endian ? p[0] << 8 | p[1] : p[0] | p[1] << 8);
    }
    void put16(uint8_t* p, uint16_t v) const {
        p[big_endian ? 0 : 1] = static_cast<uint8_t>(v >> 8);
        p[big_endian ? 1 : 0] = static_cast<uint8_t>(v);
    }
    static uint16_t code(const uint8_t* fields) { return static_cast<uint16_t>(fields[kCode] << 8 | fields[kCode + 1]); }
};

// The font's size in pixels (well under 2^24) is in the table's byte
// order: a big-endian table's first byte of it is zero.
std::optional<Layout> layout(const std::vector<uint8_t>& nfh) {
    if (nfh.size() < Layout::kRecords || nfh[0] != 'N' || nfh[1] != 'F' || nfh[2] != 'H') return std::nullopt;
    Layout l;
    l.big_endian = nfh[4] == 0;
    l.count = l.big_endian ? be32(&nfh[8]) : le32(&nfh[8]);
    if (Layout::kRecords + size_t{l.count} * kRecordSize > nfh.size()) return std::nullopt;
    return l;
}

// The record of `code`'s glyph, if the table has it.
uint8_t* find(std::vector<uint8_t>& nfh, const Layout& l, uint16_t code) {
    for (uint32_t i = 0; i < l.count; ++i) {
        if (Layout::code(l.record(nfh, i)) == code) return l.record(nfh, i);
    }
    return nullptr;
}

// The face buttons by place, with the Tekken button picture lighting that
// button's dot (1 left punch on the left button, 2 right punch on the top,
// 3 left kick at the bottom, 4 right kick on the right). Each face button
// also has a highlighted icon, four codes on.
enum Place { kRight, kBottom, kTop, kLeft };
struct FaceButton {
    uint16_t code;
    Place place;
    uint16_t dots;
};
constexpr FaceButton kFaceButtons[] = {{0xE024, kRight, 0xE01C}, {0xE025, kBottom, 0xE018}, {0xE026, kTop, 0xE016}, {0xE027, kLeft, 0xE015}};
constexpr uint16_t kHighlighted = 4;

// The Tekken button pictures: every combination of lit dots.
constexpr uint16_t kFirstCombination = 0xE015, kLastCombination = 0xE023;

constexpr uint16_t kStart = 0xE036, kSelect = 0xE037, kStartLit = 0x21D2, kSelectLit = 0x21D4;
struct Shoulder {
    uint16_t code;
    std::string_view name;
    bool lit;
};
constexpr Shoulder kShoulders[] = {{0x2282, "L1", false}, {0x2283, "R1", false}, {0xFF1C, "L1", true}, {0xFF1E, "R1", true},
                                   {0x2286, "L2", false}, {0x2287, "R2", false}, {0x2266, "L2", true}, {0x2267, "R2", true}};
// The pad's cross: nothing lit, left and right, up and down, all.
struct Pad {
    uint16_t code;
    bool horizontal, vertical;
};
constexpr Pad kPads[] = {{0x253C, false, false}, {0x253F, true, false}, {0x2542, false, true}, {0x254B, true, true}};

using draw::Canvas;
using draw::Colour;
using draw::Paint;
using draw::Point;
using draw::Shape;

const Colour kBody = Colour::rgb(0x3A3A3A), kBodyDark = Colour::rgb(0x121212);
const Colour kRim = Colour::rgb(0x9A9A9A), kLitRim = Colour::rgb(0xFF6A58), kGlow = Colour::rgb(0xFF2414, 0.9);
// The symbols' colours, after the PlayStation's own.
Colour symbol_colour(Place place) {
    switch (place) {
    case kRight: return Colour::rgb(0xF2685A);
    case kBottom: return Colour::rgb(0x7FA9EA);
    case kTop: return Colour::rgb(0x45C99B);
    default: return Colour::rgb(0xEC8BD4);
    }
}

Rgba picture(const Font& font, uint16_t code) {
    const Glyph& g = font.glyphs.at(code);
    return crop(font.texture, g.x, g.y, g.width, g.height);
}

bool has(const Font& font, uint16_t code) { return font.glyphs.count(code) != 0; }

// Replaces a glyph's box with `image`, scaled down to fit and centred.
void place(Rgba& texture, const Glyph& box, const Rgba& image) {
    put(texture, blank(box.width, box.height), box.x, box.y);
    if (image.width == 0 || image.height == 0) return;
    const double scale = std::min({1.0, static_cast<double>(box.width) / image.width,
                                   static_cast<double>(box.height) / image.height});
    const uint32_t w = std::max(1u, static_cast<uint32_t>(std::lround(image.width * scale)));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(std::lround(image.height * scale)));
    const Rgba fitted = (w == image.width && h == image.height) ? image : resize(image, w, h);
    put(texture, fitted, box.x + (box.width - w) / 2, box.y + (box.height - h) / 2);
}

// The lit (white) parts red, as the font marks a highlighted button.
Rgba highlighted(Rgba image) {
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        uint8_t* p = &image.pixels[i];
        if (std::min({p[0], p[1], p[2]}) < 150) continue;
        const uint32_t light = std::max({p[0], p[1], p[2]});
        p[0] = static_cast<uint8_t>(light);
        p[1] = p[2] = static_cast<uint8_t>(light * 70 / 255);
    }
    return image;
}

// Text in the font's own letters, side by side.
Rgba text(const Font& font, std::string_view s) {
    std::vector<Rgba> letters;
    uint32_t width = 0, height = 0;
    for (const char c : s) {
        if (!has(font, static_cast<uint16_t>(c))) continue;
        letters.push_back(picture(font, static_cast<uint16_t>(c)));
        width += letters.back().width;
        height = std::max(height, letters.back().height);
    }
    Rgba out = blank(width, height);
    uint32_t x = 0;
    for (const Rgba& letter : letters) {
        put(out, letter, x, height - letter.height);
        x += letter.width;
    }
    return out;
}

// The dark round button or badge, lit with a red glow.
void button(Canvas& c, const Shape& shape, bool lit) {
    if (lit) c.glow(shape, 2.0, kGlow);
    c.fill(shape, Paint(kBody, kBodyDark));
    c.fill(shape.outline(lit ? 1.2 : 0.9), lit ? kLitRim : kRim);
}

Rgba face_button(const Glyph& box, Place place, bool lit) {
    const uint32_t w = box.width, h = box.height;
    Canvas c(w, h);
    const Point centre{w / 2.0, h / 2.0};
    const double radius = std::min(w, h) / 2.0 - (lit ? 1.6 : 0.6);
    button(c, Shape::circle(w, h, centre, radius), lit);
    const double s = radius * 0.43, thick = std::max(1.4, radius * 0.17);
    const Colour colour = symbol_colour(place);
    switch (place) {
    case kRight:
        c.fill(Shape::circle(w, h, centre, s + thick / 2) - Shape::circle(w, h, centre, s - thick / 2), colour);
        break;
    case kBottom:
        c.fill(Shape::line(w, h, {centre.x - s, centre.y - s}, {centre.x + s, centre.y + s}, thick) |
                   Shape::line(w, h, {centre.x + s, centre.y - s}, {centre.x - s, centre.y + s}, thick),
               colour);
        break;
    case kTop: {
        const double t = s * 1.15, lower = centre.y + t * 0.62, upper = centre.y - t * 0.95;
        const Point a{centre.x, upper}, b{centre.x - t, lower}, d{centre.x + t, lower};
        c.fill(Shape::line(w, h, a, b, thick) | Shape::line(w, h, b, d, thick) | Shape::line(w, h, d, a, thick), colour);
        break;
    }
    default: {
        const double t = s * 0.85;
        const Point a{centre.x - t, centre.y - t}, b{centre.x + t, centre.y - t}, d{centre.x + t, centre.y + t},
            e{centre.x - t, centre.y + t};
        c.fill(Shape::line(w, h, a, b, thick) | Shape::line(w, h, b, d, thick) | Shape::line(w, h, d, e, thick) |
                   Shape::line(w, h, e, a, thick),
               colour);
        break;
    }
    }
    return c.result();
}

// Neutral Start and Select: a white disc with a dark arrow, right for
// Start, left for Select (Back).
Rgba start_select_neutral(const Glyph& box, bool start) {
    const uint32_t w = box.width, h = box.height;
    Canvas c(w, h);
    const Point centre{w / 2.0, h / 2.0};
    const double radius = std::min(w, h) / 2.0 - 0.6, s = radius * 0.42, d = start ? 1.0 : -1.0;
    const Shape disc = Shape::circle(w, h, centre, radius);
    c.fill(disc, Paint(Colour::rgb(0xFAFAFA), Colour::rgb(0xD2D2D2)));
    c.fill(disc.outline(1.3), Colour::rgb(0x1A1A1A));
    c.fill(Shape::polygon(w, h, {{centre.x - d * s * 0.8, centre.y - s}, {centre.x + d * s * 1.05, centre.y},
                                 {centre.x - d * s * 0.8, centre.y + s}}),
           Colour::rgb(0x1A1A1A));
    return c.result();
}

// PlayStation Start: an arrow; Select: a bar (the shapes of those buttons).
Rgba start_select(const Glyph& box, bool start, bool lit) {
    const uint32_t w = box.width, h = box.height;
    Canvas c(w, h);
    const Point centre{w / 2.0, h / 2.0};
    const double radius = std::min(w, h) / 2.0 - (lit ? 1.6 : 0.6), s = radius * 0.42;
    button(c, Shape::circle(w, h, centre, radius), lit);
    const Colour white = Colour::rgb(0xF2F2F2);
    if (start) {
        c.fill(Shape::polygon(w, h, {{centre.x - s * 0.8, centre.y - s}, {centre.x + s * 1.05, centre.y}, {centre.x - s * 0.8, centre.y + s}}),
               white);
    } else {
        c.fill(Shape::rounded_rect(w, h, {centre.x - s * 1.1, centre.y - s * 0.4}, {centre.x + s * 1.1, centre.y + s * 0.4}, s * 0.3),
               white);
    }
    return c.result();
}

Rgba shoulder(const Font& font, const Glyph& box, std::string_view name, bool lit) {
    const uint32_t w = box.width, h = box.height;
    Canvas c(w, h);
    const double margin = lit ? 1.8 : 0.7;
    const Shape badge = Shape::rounded_rect(w, h, {margin, margin}, {w - margin, h - margin}, std::min(w, h) * 0.28);
    button(c, badge, lit);
    const Rgba label = text(font, name);
    if (label.width > 0 && label.height > 0) {
        const double fit = std::min((w - 2 * margin - 4.0) / label.width, (h - 2 * margin - 2.5) / label.height);
        const double lw = label.width * fit, lh = label.height * fit;
        c.picture(label, (w - lw) / 2, (h - lh) / 2, lw, lh);
    }
    return c.result();
}

// Four arrow-shaped buttons pointing out from the middle.
Rgba pad(const Glyph& box, bool horizontal, bool vertical) {
    const uint32_t w = box.width, h = box.height;
    Canvas c(w, h);
    const double cx = w / 2.0, cy = h / 2.0, reach = std::min(w, h) / 2.0 - 0.8, half = reach * 0.37;
    struct Arm {
        double dx, dy;
        bool lit;
    };
    const Arm arms[] = {{0, -1, vertical}, {0, 1, vertical}, {-1, 0, horizontal}, {1, 0, horizontal}};
    for (const Arm& arm : arms) {
        // Along the arm (u) and across it (v).
        const auto at = [&](double u, double v) { return Point{cx + arm.dx * u - arm.dy * v, cy + arm.dy * u + arm.dx * v}; };
        const Shape shape = Shape::polygon(w, h, {at(reach, -half), at(reach, half), at(reach * 0.42, half), at(reach * 0.14, 0),
                                                  at(reach * 0.42, -half)});
        if (arm.lit) {
            c.glow(shape, 1.6, kGlow);
            c.fill(shape, Paint(Colour::rgb(0xE8301F), Colour::rgb(0x9E140C)));
            c.fill(shape.outline(0.8), kLitRim);
        } else {
            c.fill(shape, Paint(Colour::rgb(0x505050), Colour::rgb(0x262626)));
            c.fill(shape.outline(0.9), Colour::rgb(0xB8B8B8));
        }
    }
    return c.result();
}

// A Tekken button picture with its lit dots in the symbols' colours: each
// lit texel takes the colour of the dot it belongs to (the nearest lit
// dot of the one-dot pictures).
Rgba coloured_dots(const Font& font, uint16_t code) {
    struct Dot {
        double x = 0, y = 0;
        Colour colour;
    };
    std::vector<Dot> dots;
    for (const FaceButton& b : kFaceButtons) {
        if (!has(font, b.dots)) continue;
        const Rgba one = picture(font, b.dots);
        double sx = 0, sy = 0, n = 0;
        for (uint32_t y = 0; y < one.height; ++y) {
            for (uint32_t x = 0; x < one.width; ++x) {
                const uint8_t* p = one.at(x, y);
                if (std::min({p[0], p[1], p[2]}) > 200 && p[3] > 128) {
                    sx += x;
                    sy += y;
                    ++n;
                }
            }
        }
        if (n > 0) dots.push_back({sx / n, sy / n, symbol_colour(b.place)});
    }
    Rgba image = picture(font, code);
    if (dots.size() != 4) return image;
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            uint8_t* p = image.at(x, y);
            if (std::min({p[0], p[1], p[2]}) < 140) continue;
            const Dot* nearest = &dots[0];
            for (const Dot& d : dots) {
                if (std::hypot(d.x - x, d.y - y) < std::hypot(nearest->x - x, nearest->y - y)) nearest = &d;
            }
            const double light = std::max({p[0], p[1], p[2]}) / 255.0;
            p[0] = static_cast<uint8_t>(std::lround(255 * nearest->colour.r * light));
            p[1] = static_cast<uint8_t>(std::lround(255 * nearest->colour.g * light));
            p[2] = static_cast<uint8_t>(std::lround(255 * nearest->colour.b * light));
        }
    }
    return image;
}

} // namespace

std::optional<std::map<uint16_t, Glyph>> parse(const std::vector<uint8_t>& nfh) {
    const auto l = layout(nfh);
    if (!l) return std::nullopt;
    std::map<uint16_t, Glyph> glyphs;
    for (uint32_t i = 0; i < l->count; ++i) {
        const uint8_t* r = l->record(nfh, i);
        // (Sorted: anything else is not a glyph table this reads right.)
        if (!glyphs.empty() && Layout::code(r) <= glyphs.rbegin()->first) return std::nullopt;
        glyphs[Layout::code(r)] =
            Glyph{l->get16(r + Layout::kX), l->get16(r + Layout::kY), r[Layout::kWidth], r[Layout::kHeight]};
    }
    return glyphs;
}

bool copy_glyph(std::vector<uint8_t>& nfh, uint16_t code, uint16_t like) {
    const auto l = layout(nfh);
    uint8_t* to = l ? find(nfh, *l, code) : nullptr;
    const uint8_t* from = l ? find(nfh, *l, like) : nullptr;
    if (to == nullptr || from == nullptr) return false;
    std::memmove(to, from, Layout::kCode);
    return true;
}

bool set_glyph(std::vector<uint8_t>& nfh, uint16_t code, const Glyph& box, const Placement& placement) {
    const auto l = layout(nfh);
    uint8_t* r = l ? find(nfh, *l, code) : nullptr;
    if (r == nullptr) return false;
    const auto sixty_fourths = [](double pixels) {
        return static_cast<uint16_t>(static_cast<int16_t>(std::lround(pixels * 64)));
    };
    l->put16(r + Layout::kX, static_cast<uint16_t>(box.x));
    l->put16(r + Layout::kY, static_cast<uint16_t>(box.y));
    l->put16(r + Layout::kLeft, sixty_fourths(placement.left));
    l->put16(r + Layout::kTop, sixty_fourths(placement.top));
    l->put16(r + Layout::kAdvance, sixty_fourths(placement.advance));
    r[Layout::kWidth] = static_cast<uint8_t>(box.width);
    r[Layout::kHeight] = static_cast<uint8_t>(box.height);
    return true;
}

Rgba neutral_icons(const Font& font) {
    Rgba texture = font.texture;
    for (const auto& [code, start, lit] : {std::tuple{kStart, true, false}, std::tuple{kSelect, false, false},
                                           std::tuple{kStartLit, true, true}, std::tuple{kSelectLit, false, true}}) {
        if (!has(font, code)) continue;
        const Glyph& box = font.glyphs.at(code);
        const Rgba icon = start_select_neutral(box, start);
        place(texture, box, lit ? highlighted(icon) : icon);
    }
    for (const FaceButton& b : kFaceButtons) {
        if (!has(font, b.dots)) continue;
        const Rgba lit = picture(font, b.dots);
        if (has(font, b.code)) place(texture, font.glyphs.at(b.code), lit);
        const auto code = static_cast<uint16_t>(b.code + kHighlighted);
        if (has(font, code)) place(texture, font.glyphs.at(code), highlighted(lit));
    }
    return texture;
}

Rgba playstation_icons(const Font& font) {
    Rgba texture = font.texture;
    const auto replace = [&](uint16_t code, auto&& make) {
        if (has(font, code)) place(texture, font.glyphs.at(code), make(font.glyphs.at(code)));
    };
    for (const FaceButton& b : kFaceButtons) {
        replace(b.code, [&](const Glyph& box) { return face_button(box, b.place, false); });
        replace(static_cast<uint16_t>(b.code + kHighlighted), [&](const Glyph& box) { return face_button(box, b.place, true); });
    }
    replace(kStart, [](const Glyph& box) { return start_select(box, true, false); });
    replace(kSelect, [](const Glyph& box) { return start_select(box, false, false); });
    replace(kStartLit, [](const Glyph& box) { return start_select(box, true, true); });
    replace(kSelectLit, [](const Glyph& box) { return start_select(box, false, true); });
    for (const Shoulder& s : kShoulders) {
        replace(s.code, [&](const Glyph& box) { return shoulder(font, box, s.name, s.lit); });
    }
    for (const Pad& p : kPads) {
        replace(p.code, [&](const Glyph& box) { return pad(box, p.horizontal, p.vertical); });
    }
    for (uint16_t code = kFirstCombination; code <= kLastCombination; ++code) {
        replace(code, [&](const Glyph&) { return coloured_dots(font, code); });
    }
    return texture;
}

} // namespace cafe::title::font
