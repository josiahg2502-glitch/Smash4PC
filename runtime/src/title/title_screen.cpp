#include "title/title_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace cafe::title::title_screen {
namespace {

// The logo still: "TAG TOURNAMENT 2" ends at row 406 and the subtitle's
// brush lettering starts at row 412 (one stroke of its "N" curls up
// against the banner from row 392); the kanji go on behind both.
constexpr uint32_t kStill = 1280, kStillHeight = 720;
constexpr uint32_t kCurlTop = 392, kSubtitleTop = 412, kSubtitleBelow = 418;
constexpr uint32_t kFadeTop = 406, kFadeBottom = 418;

struct Mask {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bits;
    Mask(uint32_t w, uint32_t h) : width(w), height(h), bits(size_t{w} * h, 0) {}
    uint8_t& operator()(uint32_t x, uint32_t y) { return bits[size_t{y} * width + x]; }
    uint8_t operator()(uint32_t x, uint32_t y) const { return bits[size_t{y} * width + x]; }
};

// Square neighbourhoods, a row pass then a column pass; outside counts as
// clear.
Mask spread(const Mask& m, int radius, bool erode) {
    Mask rows(m.width, m.height), out(m.width, m.height);
    const auto at = [](const Mask& s, int64_t x, int64_t y) {
        return x >= 0 && y >= 0 && x < s.width && y < s.height && s(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    };
    for (int pass = 0; pass < 2; ++pass) {
        const Mask& in = pass == 0 ? m : rows;
        Mask& to = pass == 0 ? rows : out;
        for (uint32_t y = 0; y < m.height; ++y) {
            for (uint32_t x = 0; x < m.width; ++x) {
                bool all = true, any = false;
                for (int k = -radius; k <= radius; ++k) {
                    const bool v = pass == 0 ? at(in, int64_t{x} + k, y) : at(in, x, int64_t{y} + k);
                    all = all && v;
                    any = any || v;
                }
                to(x, y) = erode ? all : any;
            }
        }
    }
    return out;
}

// Dilation by the four neighbours, `times` over.
Mask grow(Mask m, int times) {
    for (int t = 0; t < times; ++t) {
        Mask next = m;
        for (uint32_t y = 0; y < m.height; ++y) {
            for (uint32_t x = 0; x < m.width; ++x) {
                if (m(x, y)) continue;
                next(x, y) = (x > 0 && m(x - 1, y)) || (x + 1 < m.width && m(x + 1, y)) || (y > 0 && m(x, y - 1)) ||
                             (y + 1 < m.height && m(x, y + 1));
            }
        }
        m = std::move(next);
    }
    return m;
}

// The eight-connected parts of `m`, as pixel indices.
std::vector<std::vector<size_t>> parts(const Mask& m) {
    std::vector<std::vector<size_t>> out;
    Mask seen(m.width, m.height);
    std::vector<std::pair<uint32_t, uint32_t>> stack;
    for (uint32_t y = 0; y < m.height; ++y) {
        for (uint32_t x = 0; x < m.width; ++x) {
            if (!m(x, y) || seen(x, y)) continue;
            std::vector<size_t>& part = out.emplace_back();
            stack.push_back({x, y});
            seen(x, y) = 1;
            while (!stack.empty()) {
                const auto [px, py] = stack.back();
                stack.pop_back();
                part.push_back(size_t{py} * m.width + px);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int64_t nx = int64_t{px} + dx, ny = int64_t{py} + dy;
                        if (nx < 0 || ny < 0 || nx >= m.width || ny >= m.height) continue;
                        const auto ux = static_cast<uint32_t>(nx), uy = static_cast<uint32_t>(ny);
                        if (m(ux, uy) && !seen(ux, uy)) {
                            seen(ux, uy) = 1;
                            stack.push_back({ux, uy});
                        }
                    }
                }
            }
        }
    }
    return out;
}

// The parts of `m` that reach below `row`.
Mask reaching_below(const Mask& m, uint32_t row) {
    Mask out(m.width, m.height);
    for (const std::vector<size_t>& part : parts(m)) {
        if (std::none_of(part.begin(), part.end(), [&](size_t i) { return i / m.width > row; })) continue;
        for (const size_t i : part) out.bits[i] = 1;
    }
    return out;
}

// Clear pixels that the edge cannot reach through clear ones
// (four-connected) become set.
void fill_holes(Mask& m) {
    Mask outside(m.width, m.height);
    std::vector<std::pair<uint32_t, uint32_t>> stack;
    const auto visit = [&](uint32_t x, uint32_t y) {
        if (!m(x, y) && !outside(x, y)) {
            outside(x, y) = 1;
            stack.push_back({x, y});
        }
    };
    for (uint32_t x = 0; x < m.width; ++x) {
        visit(x, 0);
        visit(x, m.height - 1);
    }
    for (uint32_t y = 0; y < m.height; ++y) {
        visit(0, y);
        visit(m.width - 1, y);
    }
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        if (x > 0) visit(x - 1, y);
        if (x + 1 < m.width) visit(x + 1, y);
        if (y > 0) visit(x, y - 1);
        if (y + 1 < m.height) visit(x, y + 1);
    }
    for (size_t i = 0; i < m.bits.size(); ++i) m.bits[i] = !outside.bits[i];
}

// Offsets within a disc of radius `r`, nearest first.
std::vector<std::pair<int, int>> disc(int r) {
    std::vector<std::pair<int, int>> out;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy <= r * r + r) out.push_back({dx, dy});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
    });
    return out;
}

// Dilation (or erosion) by a disc; outside counts as clear.
Mask spread_disc(const Mask& m, int radius, bool erode) {
    const auto offsets = disc(radius);
    Mask out(m.width, m.height);
    for (uint32_t y = 0; y < m.height; ++y) {
        for (uint32_t x = 0; x < m.width; ++x) {
            bool all = true, any = false;
            for (const auto& [dx, dy] : offsets) {
                const int64_t nx = int64_t{x} + dx, ny = int64_t{y} + dy;
                const bool v = nx >= 0 && ny >= 0 && nx < m.width && ny < m.height &&
                               m(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny));
                all = all && v;
                any = any || v;
                if (erode ? !all : any) break;
            }
            out(x, y) = erode ? all : any;
        }
    }
    return out;
}

// Each pixel's distance to the nearest set one, up to `limit` (beyond it,
// `limit` + 1).
std::vector<float> distance(const Mask& m, int limit) {
    const auto offsets = disc(limit);
    std::vector<float> out(m.bits.size(), static_cast<float>(limit + 1));
    for (uint32_t y = 0; y < m.height; ++y) {
        for (uint32_t x = 0; x < m.width; ++x) {
            for (const auto& [dx, dy] : offsets) {
                const int64_t nx = int64_t{x} + dx, ny = int64_t{y} + dy;
                if (nx >= 0 && ny >= 0 && nx < m.width && ny < m.height &&
                    m(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny))) {
                    out[size_t{y} * m.width + x] = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                    break;
                }
            }
        }
    }
    return out;
}

// Smooth noise in [-1, 1]: values on a grid of `cell` texels, interpolated.
float noise(uint32_t x, uint32_t y, uint32_t cell, uint32_t seed) {
    const auto at = [seed](uint32_t gx, uint32_t gy) {
        uint32_t h = gx * 0x9E3779B1u ^ (gy + 0x7F4A7C15u) * 0x85EBCA77u ^ seed * 0xC2B2AE3Du;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        h *= 0x297A2D39u;
        h ^= h >> 15;
        return static_cast<float>(h >> 8) / 8388608.0f - 1.0f;
    };
    const uint32_t gx = x / cell, gy = y / cell;
    const float fx = static_cast<float>(x % cell) / cell, fy = static_cast<float>(y % cell) / cell;
    const float top = at(gx, gy) * (1 - fx) + at(gx + 1, gy) * fx;
    const float bottom = at(gx, gy + 1) * (1 - fx) + at(gx + 1, gy + 1) * fx;
    return top * (1 - fy) + bottom * fy;
}

// ------------------------------------------------------------------ the subtitle

// The subtitle is brush lettering: black strokes ("ink") in a rough white
// edge that runs round each word as one shape (counters and gaps between
// touching letters white too), over a brushed underline. "WiiU" is large;
// EDITION's letters are 62 rows high, their ink from row 438 to 500.
constexpr int kCapTop = 438, kBaseline = 500, kCap = kBaseline - kCapTop;

// Ink, 0 (none) to 1 (black), of a width x height rectangle whose top row
// is `top` in the still.
struct Ink {
    uint32_t width = 0, height = 0;
    int top = 0;
    std::vector<float> v;
    Ink() = default;
    Ink(uint32_t w, uint32_t h, int t) : width(w), height(h), top(t), v(size_t{w} * h, 0.0f) {}
    float& operator()(uint32_t x, uint32_t y) { return v[size_t{y} * width + x]; }
    float operator()(uint32_t x, uint32_t y) const { return v[size_t{y} * width + x]; }
};

// Cut down to where it is mostly black; empty if nowhere.
Ink trim(const Ink& p) {
    uint32_t x0 = p.width, x1 = 0, y0 = p.height, y1 = 0;
    for (uint32_t y = 0; y < p.height; ++y) {
        for (uint32_t x = 0; x < p.width; ++x) {
            if (p(x, y) <= 0.5f) continue;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x + 1);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y + 1);
        }
    }
    if (x1 <= x0) return {};
    Ink out(x1 - x0, y1 - y0, p.top + static_cast<int>(y0));
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) out(x, y) = p(x0 + x, y0 + y);
    }
    return out;
}

struct Rect {
    uint32_t x0, x1, y0, y1;
};

// The still's ink in `r`, trimmed (or not).
Ink cut(const Ink& still, Rect r, bool trimmed = true) {
    Ink out(r.x1 - r.x0, r.y1 - r.y0, static_cast<int>(r.y0));
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) out(x, y) = still(r.x0 + x, r.y0 + y);
    }
    return trimmed ? trim(out) : out;
}

// Upside down (`rows`) or mirrored.
Ink flip(const Ink& p, bool rows) {
    Ink out(p.width, p.height, p.top);
    for (uint32_t y = 0; y < p.height; ++y) {
        for (uint32_t x = 0; x < p.width; ++x) {
            out(x, y) = rows ? p(x, p.height - 1 - y) : p(p.width - 1 - x, y);
        }
    }
    return out;
}

// Resized (bilinear, corners on corners) with its top at `top`; strokes
// then widen by `bold` columns each side, keeping their weight where a
// letter narrows.
Ink scaled(const Ink& p, uint32_t width, uint32_t height, int top, int bold) {
    Ink out(width, height, top);
    const auto source = [](uint32_t i, uint32_t from, uint32_t to) {
        return to > 1 ? static_cast<double>(i) * (from - 1) / (to - 1) : 0.0;
    };
    for (uint32_t y = 0; y < height; ++y) {
        const double sy = source(y, p.height, height);
        const auto y0 = static_cast<uint32_t>(sy);
        const uint32_t y1 = std::min(y0 + 1, p.height - 1);
        const double fy = sy - y0;
        for (uint32_t x = 0; x < width; ++x) {
            const double sx = source(x, p.width, width);
            const auto x0 = static_cast<uint32_t>(sx);
            const uint32_t x1 = std::min(x0 + 1, p.width - 1);
            const double fx = sx - x0;
            out(x, y) = static_cast<float>((p(x0, y0) * (1 - fx) + p(x1, y0) * fx) * (1 - fy) +
                                           (p(x0, y1) * (1 - fx) + p(x1, y1) * fx) * fy);
        }
    }
    if (bold == 0) return out;
    Ink wide = out;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            for (int k = -bold; k <= bold; ++k) {
                const int64_t sx = int64_t{x} + k;
                if (sx >= 0 && sx < width) wide(x, y) = std::max(wide(x, y), out(static_cast<uint32_t>(sx), y));
            }
        }
    }
    return wide;
}

// `p` over `into`, its left column at column `x` of `into` and its top at
// row `top` of the still.
void draw(Ink& into, const Ink& p, int x, int top) {
    for (uint32_t y = 0; y < p.height; ++y) {
        const int64_t ty = int64_t{top} - into.top + y;
        if (ty < 0 || ty >= into.height) continue;
        for (uint32_t c = 0; c < p.width; ++c) {
            const int64_t tx = int64_t{x} + c;
            if (tx < 0 || tx >= into.width) continue;
            float& v = into(static_cast<uint32_t>(tx), static_cast<uint32_t>(ty));
            v = std::max(v, p(c, y));
        }
    }
}

struct Placed {
    const Ink& ink;
    int x, top;
};

// Strokes put together into one letter.
Ink join(std::initializer_list<Placed> placed) {
    int top = INT32_MAX, bottom = INT32_MIN, right = 0;
    for (const Placed& p : placed) {
        top = std::min(top, p.top);
        bottom = std::max(bottom, p.top + static_cast<int>(p.ink.height));
        right = std::max(right, p.x + static_cast<int>(p.ink.width));
    }
    Ink out(static_cast<uint32_t>(right), static_cast<uint32_t>(bottom - top), top);
    for (const Placed& p : placed) draw(out, p.ink, p.x, p.top);
    return trim(out);
}

// Strokes cut off at the left or right column would end squarely there;
// each ends in a half ellipse instead, up to six columns deep.
void round_end(Ink& p, bool right) {
    constexpr double kDepth = 6;
    const uint32_t c = right ? p.width - 1 : 0;
    for (uint32_t y = 0; y < p.height;) {
        if (p(c, y) <= 0.5f) {
            ++y;
            continue;
        }
        uint32_t end = y;
        while (end < p.height && p(c, end) > 0.5f) ++end;
        const double half = (end - y) / 2.0, middle = (y + end - 1) / 2.0, depth = std::min(kDepth, half);
        for (uint32_t row = y; row < end; ++row) {
            const double d = (row - middle) / std::max(half, 0.5);
            const auto n = static_cast<uint32_t>(std::lround(depth * (1 - std::sqrt(std::max(0.0, 1 - d * d)))));
            for (uint32_t k = 0; k < std::min(n, p.width); ++k) p(right ? p.width - 1 - k : k, row) = 0;
        }
        y = end;
    }
}

// The letters of "ULTIMATE" at EDITION's size, from the still's ink: T, I
// and E are EDITION's; U is WiiU's, smaller; L is an I upside down with
// E's bottom arm; M is the W upside down; A is the W's right arm with its
// mirror image and E's middle arm across them. None if a stroke is missing.
std::vector<Ink> ultimate(const Ink& still) {
    constexpr Rect kU{505, 606, 418, 504}, kW{323, 452, 410, 504}, kWArm{404, 448, 418, 502};
    constexpr Rect kE{638, 684, 405, 503}, kI{733, 745, 405, 503}, kTI{745, 797, 405, 503}, kT{745, 787, 405, 503};
    constexpr Rect kEBottom{657, 689, 482, 502}, kEMiddle{662, 684, 458, 478};
    constexpr double kWiiU = static_cast<double>(kCap) / (502 - 418); // WiiU's letters to EDITION's size

    const Ink u = cut(still, kU), w = cut(still, kW), arm = cut(still, kWArm), e = cut(still, kE), i = cut(still, kI);
    const Ink bottom = cut(still, kEBottom, false), bar = cut(still, kEMiddle);
    Ink ti = cut(still, kTI), t = cut(still, kT);
    for (const Ink* p : std::initializer_list<const Ink*>{&u, &w, &arm, &e, &i, &ti, &t, &bar}) {
        if (p->width == 0) return {};
    }
    std::vector<Ink> letters;
    letters.push_back(scaled(u, 56, kCap, kCapTop, 1));
    Ink l = join({{flip(i, true), 0, i.top}, {bottom, 6, bottom.top}});
    round_end(l, true);
    letters.push_back(trim(l));
    round_end(ti, false);
    letters.push_back(trim(ti));
    letters.push_back(scaled(flip(w, true), 72, kCap, kCapTop, 2));
    const Ink leg = scaled(arm, static_cast<uint32_t>(std::lround(arm.width * kWiiU)),
                           static_cast<uint32_t>(std::lround(arm.height * kWiiU)), 0, 0);
    const int legs = kBaseline - static_cast<int>(leg.height), across = static_cast<int>(leg.width);
    letters.push_back(join({{leg, 0, legs}, {flip(leg, false), across - 12, legs}, {bar, across / 2 - 6, kCapTop + 30}}));
    round_end(t, false);
    round_end(t, true);
    letters.push_back(trim(t));
    Ink end = e;
    round_end(end, true);
    letters.push_back(trim(end));
    for (const Ink& letter : letters) {
        if (letter.width == 0) return {};
    }
    return letters;
}

} // namespace

std::optional<Rgba> logo(const Rgba& wiiu) {
    if (wiiu.width != kStill || wiiu.height != kStillHeight) return std::nullopt;
    Mask bright(kStill, kStillHeight);
    for (uint32_t y = 0; y < kStillHeight; ++y) {
        for (uint32_t x = 0; x < kStill; ++x) {
            const uint8_t* p = wiiu.at(x, y);
            bright(x, y) = std::max({p[0], p[1], p[2]}) > 150;
        }
    }
    // Letters are thick, the subtitle's curling stroke thin: thin bright
    // parts below the banner's top, and all bright ones below the
    // subtitle's top, connected to what goes on below it, are the subtitle.
    const Mask thick = spread(spread(bright, 3, true), 3, false);
    const Mask thick_near = grow(thick, 2);
    Mask candidates(kStill, kStillHeight);
    for (uint32_t y = kCurlTop + 1; y < kStillHeight; ++y) {
        for (uint32_t x = 0; x < kStill; ++x) {
            candidates(x, y) = bright(x, y) && (!thick_near(x, y) || y > kSubtitleTop);
        }
    }
    const Mask bright_subtitle = reaching_below(candidates, kSubtitleBelow);
    const Mask subtitle = grow(bright_subtitle, 4);
    // The old subtitle goes, and rows below the banner fade to black (the
    // kanji tucked behind it).
    Rgba out = wiiu;
    for (uint32_t y = 0; y < kStillHeight; ++y) {
        const double keep = std::clamp((static_cast<double>(kFadeBottom) - y) / (kFadeBottom - kFadeTop), 0.0, 1.0);
        for (uint32_t x = 0; x < kStill; ++x) {
            uint8_t* p = out.at(x, y);
            const bool gone = y > kCurlTop && subtitle(x, y) && !thick(x, y);
            for (int k = 0; k < 3; ++k) p[k] = gone ? 0 : static_cast<uint8_t>(p[k] * keep + 0.5);
        }
    }

    // The old subtitle's parts: its white (bright, near grey), the shape
    // the white goes round, and in that the ink (dark) and windows onto
    // the logo behind (the rest, where wide enough to have an inside).
    std::vector<float> luma(size_t{kStill} * kStillHeight);
    Mask white(kStill, kStillHeight), dark(kStill, kStillHeight);
    for (uint32_t y = 0; y < kStillHeight; ++y) {
        for (uint32_t x = 0; x < kStill; ++x) {
            const uint8_t* p = wiiu.at(x, y);
            const int high = std::max({p[0], p[1], p[2]}), low = std::min({p[0], p[1], p[2]});
            white(x, y) = bright_subtitle(x, y) && low > 185 && high - low < 40;
            dark(x, y) = high < 70;
            luma[size_t{y} * kStill + x] = static_cast<float>(p[0] + p[1] + p[2]) / 3;
        }
    }
    Mask shape = spread(spread(white, 2, false), 2, true);
    fill_holes(shape);
    Mask between(kStill, kStillHeight), window(kStill, kStillHeight);
    for (size_t i = 0; i < shape.bits.size(); ++i) between.bits[i] = shape.bits[i] && !white.bits[i] && !dark.bits[i];
    for (const std::vector<size_t>& part : parts(between)) {
        const bool inside = std::any_of(part.begin(), part.end(), [&](size_t i) {
            const uint32_t x = i % kStill, y = static_cast<uint32_t>(i / kStill);
            if (x == 0 || y == 0 || x + 1 >= kStill || y + 1 >= kStillHeight) return false;
            for (uint32_t ny = y - 1; ny <= y + 1; ++ny) {
                for (uint32_t nx = x - 1; nx <= x + 1; ++nx) {
                    if (!between(nx, ny)) return false;
                }
            }
            return true;
        });
        if (!inside) continue;
        for (const size_t i : part) window.bits[i] = 1;
    }
    Ink ink(kStill, kStillHeight, 0);
    for (size_t i = 0; i < ink.v.size(); ++i) {
        if (shape.bits[i] && !window.bits[i]) ink.v[i] = std::clamp((0.85f - luma[i] / 255) / 0.7f, 0.0f, 1.0f);
    }
    const std::vector<Ink> letters = ultimate(ink);
    if (letters.empty()) return std::nullopt;

    // The old subtitle as coverage and grey: inside the shape as it is
    // (windows black: the logo behind them has faded), and the white's
    // soft outer edge. Stray specks of the logo's highlights touch it, so
    // what is kept is the shape's large solid parts and what lies close to
    // them.
    std::vector<float> cover(ink.v.size(), 0.0f), grey(ink.v.size(), 0.0f);
    const Mask edge = grow(shape, 1);
    for (size_t i = 0; i < ink.v.size(); ++i) {
        if (shape.bits[i]) {
            cover[i] = 1;
            grey[i] = window.bits[i] ? 0 : luma[i];
        } else if (edge.bits[i]) {
            cover[i] = luma[i] / 255;
            grey[i] = 255;
        }
    }
    Mask large(kStill, kStillHeight);
    for (const std::vector<size_t>& part : parts(spread(spread(shape, 2, true), 2, false))) {
        if (part.size() <= 500) continue;
        for (const size_t i : part) large.bits[i] = 1;
    }
    Mask kept = grow(large, 2);
    for (size_t i = 0; i < kept.bits.size(); ++i) kept.bits[i] = kept.bits[i] && shape.bits[i];
    kept = grow(kept, 1);

    // "ULTIMATE", its letters touching as EDITION's do (M and A just
    // apart), then a word space and EDITION, the line centred where the
    // old one was: EDITION moves right.
    constexpr int kOldLeft = 327, kEditionLeft = 638, kEditionRight = 945, kSpace = 36, kTouch = -3;
    const auto after = [&](size_t k) { return k + 1 == letters.size() ? 0 : k == 3 ? 0 : kTouch; };
    int word = 0;
    for (size_t k = 0; k < letters.size(); ++k) word += static_cast<int>(letters[k].width) + after(k);
    const int line = word + kSpace + kEditionRight - kEditionLeft;
    const int left = static_cast<int>(std::lround((kOldLeft + kEditionRight - line) / 2.0));
    const int moved = left + word + kSpace - kEditionLeft;
    const auto over = [&](int x, uint32_t y, float c, float v) {
        if (x < 0 || x >= static_cast<int>(kStill)) return;
        uint8_t* p = out.at(static_cast<uint32_t>(x), y);
        for (int k = 0; k < 3; ++k) p[k] = static_cast<uint8_t>(std::clamp(p[k] * (1 - c) + v * c + 0.5f, 0.0f, 255.0f));
    };

    // The underline (below the letters' white), longer by what the line
    // grew: its left part moves with ULTIMATE's start, the rest with
    // EDITION, and the stretch before `kSplit` fills the gap twice.
    constexpr int kUnderline = 504, kUnderlineLeft = 250, kUnderlineRight = 1000, kSplit = 620;
    const int start_moved = left - kOldLeft;
    for (int x = kUnderlineLeft + start_moved; x < kUnderlineRight + moved; ++x) {
        const int from = x - (x < kSplit + start_moved ? start_moved : moved);
        if (from < kUnderlineLeft || from >= kUnderlineRight) continue;
        for (uint32_t y = kUnderline; y < kStillHeight; ++y) {
            const size_t i = size_t{y} * kStill + static_cast<uint32_t>(from);
            if (cover[i] > 0) over(x, y, cover[i], grey[i]);
        }
    }
    // EDITION as it was, with its N's stroke curling up beside the banner.
    constexpr int kEditionFrom = 625;
    for (uint32_t y = 0; y < kUnderline; ++y) {
        for (int x = kEditionFrom; x < kUnderlineRight; ++x) {
            const size_t i = size_t{y} * kStill + static_cast<uint32_t>(x);
            if (cover[i] > 0 && kept.bits[i]) over(x + moved, y, cover[i], grey[i]);
        }
    }
    // ULTIMATE's ink with white round it: the ink closed over small gaps
    // and its holes filled, then widened by four texels, give or take a
    // rough, brushy two.
    constexpr int kBoxLeft = 200, kBoxTop = 380;
    constexpr uint32_t kBoxWidth = 880, kBoxHeight = 160;
    Ink drawn(kBoxWidth, kBoxHeight, kBoxTop);
    for (size_t k = 0, x = static_cast<size_t>(left - kBoxLeft); k < letters.size(); ++k) {
        draw(drawn, letters[k], static_cast<int>(x), letters[k].top);
        x += letters[k].width + after(k);
    }
    Mask inked(kBoxWidth, kBoxHeight);
    for (size_t i = 0; i < drawn.v.size(); ++i) inked.bits[i] = drawn.v[i] > 0.5f;
    Mask closed = spread_disc(spread_disc(inked, 4, false), 4, true);
    for (size_t i = 0; i < closed.bits.size(); ++i) closed.bits[i] = closed.bits[i] || inked.bits[i];
    fill_holes(closed);
    const std::vector<float> away = distance(closed, 8);
    for (uint32_t y = 0; y < kBoxHeight; ++y) {
        for (uint32_t x = 0; x < kBoxWidth; ++x) {
            const size_t i = size_t{y} * kBoxWidth + x;
            const float reach = 4 + 1.5f * noise(x, y, 8, 1) + 0.8f * noise(x, y, 3, 2);
            const float white_here = std::clamp(reach - away[i] + 0.5f, 0.0f, 1.0f), black = drawn.v[i];
            const float c = std::max(white_here, black);
            if (c > 0) over(kBoxLeft + static_cast<int>(x), kBoxTop + y, c, 255 * std::min(white_here, 1 - black) / c);
        }
    }
    return out;
}

std::optional<Rgba> copyright_line(const Rgba& line) {
    // Words: runs of columns with letters (bright texels) apart by five
    // empty ones or more.
    std::vector<std::pair<uint32_t, uint32_t>> words;
    uint32_t x = 0;
    const auto ink = [&](uint32_t column) {
        for (uint32_t y = 0; y < line.height; ++y) {
            const uint8_t* p = line.at(column, y);
            if (std::min({p[0], p[1], p[2]}) > 150 && p[3] > 128) return true;
        }
        return false;
    };
    while (x < line.width) {
        if (!ink(x)) {
            ++x;
            continue;
        }
        const uint32_t start = x;
        uint32_t end = x, gap = 0;
        while (x < line.width && gap < 5) {
            if (ink(x)) {
                gap = 0;
                end = x + 1;
            } else {
                ++gap;
            }
            ++x;
        }
        words.push_back({start, end});
    }
    // "... 2 Wii U EDITION & (C) 2012 NAMCO BANDAI Games Inc.": the seven
    // words from "&" on end the line.
    if (words.size() < 10) return std::nullopt;
    constexpr uint32_t kOutline = 3; // letters' dark edge, before their first bright column
    const uint32_t cut_from = words[words.size() - 10].first - kOutline;
    const uint32_t cut_to = words[words.size() - 7].first - kOutline;
    Rgba joined = blank(line.width - (cut_to - cut_from), line.height);
    put(joined, crop(line, 0, 0, cut_from, line.height), 0, 0);
    put(joined, crop(line, cut_to, 0, line.width - cut_to, line.height), cut_from, 0);
    // Centred as before.
    uint32_t left = joined.width, right = 0;
    for (uint32_t y = 0; y < joined.height; ++y) {
        for (uint32_t c = 0; c < joined.width; ++c) {
            if (joined.at(c, y)[3] != 0) {
                left = std::min(left, c);
                right = std::max(right, c + 1);
            }
        }
    }
    if (right <= left) return std::nullopt;
    Rgba out = blank(line.width, line.height);
    put(out, crop(joined, left, 0, right - left, joined.height), (line.width - (right - left)) / 2, 0);
    return out;
}

} // namespace cafe::title::title_screen
