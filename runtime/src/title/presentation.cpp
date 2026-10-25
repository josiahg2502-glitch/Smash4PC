#include "title/presentation.h"

#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "cafe/vfs.h"
#include "gpu/replacements.h"
#include "host/window.h"
#include "title/archive.h"
#include "title/font.h"
#include "title/image.h"
#include "title/text.h"
#include "title/title_screen.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>

namespace cafe::title {

void system_info_created(uint32_t info);

namespace {

constexpr const char* kFontArchive = "content/hdd/data003";
constexpr const char* kTitleArchive = "content/hdd/data007";
constexpr const char* kPatchArchive = "content/hdd/data100"; // the update's: the game reads its entries first
constexpr uint32_t kFontGlyphs = 0x3A27BC43;  // the main font's glyph table
constexpr uint32_t kFontTexture = 0x4FC9A19E; // and its texture

// ------------------------------------------------------------------ the game's files

// An entry of the title's archives (as the game has it: before any
// set_archive_entries), and its NRCZ header flags in `flags`.
std::optional<std::vector<uint8_t>> game_entry(const char* archive, uint32_t hash, uint8_t (*flags)[8] = nullptr) {
    const auto ofs = vfs::read_whole(std::string("/vol/") + archive + ".ofs");
    const auto index = ofs ? archive::parse_index(*ofs) : std::nullopt;
    const archive::Entry* entry = index ? index->find(hash) : nullptr;
    if (entry == nullptr) return std::nullopt;
    int32_t handle = 0;
    if (vfs::open_file(std::string("/vol/") + archive + ".bin", "r", handle) != vfs::kOk) return std::nullopt;
    std::vector<uint8_t> stored(entry->size);
    uint64_t transferred = 0;
    vfs::read_file(handle, stored.data(), stored.size(), archive::kPayload + entry->offset, transferred);
    vfs::close_file(handle);
    if (transferred != stored.size()) return std::nullopt;
    if (flags) archive::stored_flags(stored, hash, *flags);
    return archive::unpack(std::move(stored), hash);
}

std::array<uint8_t, 16> md5_of(const uint8_t* data, size_t size) {
    std::array<uint8_t, 16> md5{};
    unsigned int length = 0;
    EVP_Digest(data, size, md5.data(), &length, EVP_md5(), nullptr);
    return md5;
}

void replace_texture(const ntp3::Texture& original, std::function<const std::vector<uint8_t>*()> contents) {
    gpu::TextureReplacement r;
    r.width = original.width;
    r.height = original.height;
    r.format = original.gx2_format();
    r.md5 = md5_of(original.data.data(), original.level0_size());
    r.contents = std::move(contents);
    gpu::add_texture_replacement(std::move(r));
}

// ------------------------------------------------------------------ the controller

std::atomic<host::Controller> g_controller{host::Controller::kOther};

// The object the title keeps its system settings in; its word at +8 is 1
// when menus confirm with the right face button (the Wii U's A).
std::atomic<uint32_t> g_system_info{0};

void apply_confirm_button() {
    const uint32_t info = g_system_info;
    if (info == 0) return;
    const uint32_t big_endian = __builtin_bswap32(g_controller == host::Controller::kNintendo ? 1u : 0u);
    std::memcpy(guest_pointer(info + 8), &big_endian, 4);
}

// ------------------------------------------------------------------ text

struct Bank {
    const char* archive;
    uint32_t hash;
    std::vector<uint8_t> contents;
    uint8_t flags[8];
};

// The archive's text banks.
std::vector<Bank> read_banks(const char* archive_name) {
    std::vector<Bank> banks;
    const auto ofs = vfs::read_whole(std::string("/vol/") + archive_name + ".ofs");
    const auto index = ofs ? archive::parse_index(*ofs) : std::nullopt;
    int32_t handle = 0;
    if (!index || vfs::open_file(std::string("/vol/") + archive_name + ".bin", "r", handle) != vfs::kOk) return banks;
    std::vector<uint32_t> hashes;
    for (const archive::Entry& e : index->entries) {
        // (A compressed entry's code tables come before its first bytes.)
        std::vector<uint8_t> start(std::min<uint64_t>(e.size, 1024));
        uint64_t transferred = 0;
        vfs::read_file(handle, start.data(), start.size(), archive::kPayload + e.offset, transferred);
        if (transferred == start.size() && text::is_bank(archive::peek(std::move(start), e.hash, 0x30))) {
            hashes.push_back(e.hash);
        }
    }
    vfs::close_file(handle);
    for (const uint32_t hash : hashes) {
        Bank bank{archive_name, hash, {}, {}};
        if (auto contents = game_entry(archive_name, hash, &bank.flags)) {
            bank.contents = std::move(*contents);
            banks.push_back(std::move(bank));
        }
    }
    return banks;
}

// ------------------------------------------------------------------ icons

// The texts' button code points (text.h) and the main font's icon for each.
struct Carrier {
    char32_t code;
    uint16_t icon;
};
constexpr Carrier kCarriers[] = {
    {text::kRightButton, 0xE024}, {text::kBottomButton, 0xE025}, {text::kTopButton, 0xE026}, {text::kLeftButton, 0xE027},
    {text::kStartButton, 0xE036}, {text::kSelectButton, 0xE037}, {text::kPad, 0x253C},
};
constexpr size_t kCarried = std::size(kCarriers);

// The other fonts texts are drawn with: the carriers' glyphs show the main
// font's icons, scaled to `icon` texels square and placed like the font's
// own symbols, drawn over the boxes of glyphs no text uses (as large as
// the icon): those listed (in kCarriers' order), or else ideographs and
// Hangul syllables (from U+3400).
struct TextFont {
    uint32_t glyphs, texture;
    uint32_t icon;
    font::Placement placement;
    std::array<uint16_t, kCarried> boxes;
};
const TextFont kTextFonts[] = {
    // The plain font (descriptions): £ ¥ Þ Œ ＼ Ź Ý.
    {0x7C74E49A, 0x099AF947, 21, {0, 19, 22}, {0x00A3, 0x00A5, 0x00DE, 0x0152, 0xFF3C, 0x0179, 0x00DD}},
    // The Korean and Japanese gothic fonts (help panels; they have Latin
    // letters too).
    {0xB1277EE3, 0xC4C9633E, 21, {0, 17, 22}, {}},
    {0x46B70814, 0x335915C9, 21, {0, 17, 22}, {}},
};

// A font's texture with icons, RGBA, for each kind of controller.
struct Icons {
    std::vector<uint8_t> neutral, playstation;
    const std::vector<uint8_t>* now() const {
        return g_controller == host::Controller::kPlayStation ? &playstation : &neutral;
    }
};
std::deque<Icons> g_icons; // (deque: replacements keep pointers to them)

// The boxes the font's carriers' icons go over.
std::optional<std::array<font::Glyph, kCarried>> icon_boxes(const TextFont& f,
                                                            const std::map<uint16_t, font::Glyph>& glyphs,
                                                            const std::set<char32_t>& used) {
    std::array<font::Glyph, kCarried> boxes;
    const auto fits = [&](uint16_t code) {
        const auto g = glyphs.find(code);
        return g != glyphs.end() && !used.count(code) && g->second.width >= f.icon && g->second.height >= f.icon;
    };
    size_t n = 0;
    if (f.boxes[0] != 0) {
        for (; n < kCarried && fits(f.boxes[n]); ++n) boxes[n] = glyphs.at(f.boxes[n]);
    } else {
        for (auto g = glyphs.lower_bound(0x3400); g != glyphs.end() && n < kCarried; ++g) {
            if (fits(g->first)) boxes[n++] = g->second;
        }
    }
    if (n < kCarried) return std::nullopt;
    return boxes;
}

void add_replacement(const ntp3::Texture& original, Icons icons) {
    g_icons.push_back(std::move(icons));
    const Icons* mine = &g_icons.back();
    replace_texture(original, [mine] { return mine->now(); });
}

// The fonts' icons follow the controller, and the texts' carriers' glyphs
// become icons in every font texts are drawn with. Returns the glyph
// tables changed, stored; none if a font is not as expected (then the
// texts must keep their words).
std::map<uint32_t, std::vector<uint8_t>> prepare_icons(const std::set<char32_t>& used) {
    std::map<uint32_t, std::vector<uint8_t>> changed;
    uint8_t flags[8];
    auto table = game_entry(kFontArchive, kFontGlyphs, &flags);
    const auto file = game_entry(kFontArchive, kFontTexture);
    auto glyphs = table ? font::parse(*table) : std::nullopt;
    const auto textures = file ? ntp3::parse(*file) : std::nullopt;
    auto texture = textures && !textures->empty() ? ntp3::decode(textures->front()) : std::nullopt;
    if (!glyphs || !texture) {
        std::fprintf(stderr, "ttt2: warning: the game's font is not where expected; its button icons stay\n");
        return changed;
    }
    const font::Font main{std::move(*glyphs), std::move(*texture)};
    const Rgba neutral = font::neutral_icons(main), playstation = font::playstation_icons(main);
    add_replacement(textures->front(), {neutral.pixels, playstation.pixels});

    const auto fail = [&](const char* what) {
        std::fprintf(stderr, "ttt2: warning: %s is not as expected; texts keep the Wii U's button names\n", what);
        changed.clear();
        return changed;
    };
    for (const Carrier& c : kCarriers) {
        if (!font::copy_glyph(*table, static_cast<uint16_t>(c.code), c.icon)) return fail("the main font");
    }
    changed[kFontGlyphs] = archive::pack(*table, kFontGlyphs, flags);
    for (const TextFont& f : kTextFonts) {
        auto other = game_entry(kFontArchive, f.glyphs, &flags);
        const auto other_file = game_entry(kFontArchive, f.texture);
        const auto other_glyphs = other ? font::parse(*other) : std::nullopt;
        const auto other_textures = other_file ? ntp3::parse(*other_file) : std::nullopt;
        const auto other_texture =
            other_textures && !other_textures->empty() ? ntp3::decode(other_textures->front()) : std::nullopt;
        const auto boxes = other_glyphs && other_texture ? icon_boxes(f, *other_glyphs, used) : std::nullopt;
        if (!boxes) return fail("a font");
        Rgba with[2] = {*other_texture, *other_texture};
        for (size_t i = 0; i < kCarried; ++i) {
            const font::Glyph& box = (*boxes)[i];
            const font::Glyph& icon = main.glyphs.at(kCarriers[i].icon);
            const font::Glyph drawn{box.x, box.y, f.icon, f.icon};
            if (!font::set_glyph(*other, static_cast<uint16_t>(kCarriers[i].code), drawn, f.placement)) return fail("a font");
            for (int k = 0; k < 2; ++k) {
                const Rgba& source = k == 0 ? neutral : playstation;
                put(with[k], blank(box.width, box.height), box.x, box.y);
                put(with[k], resize(crop(source, icon.x, icon.y, icon.width, icon.height), f.icon, f.icon), box.x, box.y);
            }
        }
        add_replacement(other_textures->front(), {std::move(with[0].pixels), std::move(with[1].pixels)});
        changed[f.glyphs] = archive::pack(*other, f.glyphs, flags);
    }
    return changed;
}

// ------------------------------------------------------------------ the title screen

std::vector<std::vector<uint8_t>> g_lines; // copyright lines without "Wii U Edition"

void prepare_copyright(const std::vector<ntp3::Texture>& set) {
    g_lines.reserve(set.size());
    for (const ntp3::Texture& t : set) {
        if (t.height != 32) continue;
        const auto line = ntp3::decode(t);
        const auto fixed = line ? title_screen::copyright_line(*line) : std::nullopt;
        if (!fixed) continue;
        g_lines.push_back(fixed->pixels);
        const std::vector<uint8_t>* contents = &g_lines.back();
        replace_texture(t, [contents] { return contents; });
    }
}

// The logo is a video: the title screen's ends on the still of the Wii U
// logo (the first texture of the set), after fading in from white and
// light sweeping over it. Each frame of it becomes the logo subtitled
// "ULTIMATE EDITION" (title_screen::logo) with the same fade and light.
// The frame is fitted as a * Wii U still + b (a by least squares over the
// logo, b from the black around it); what the fit leaves (the light, and
// coding noise) goes onto a * new still + b, the noise dropped and the
// rest blurred where the stills differ, so that it does not draw the old
// subtitle. Frames that do not fit (other videos) stay as they are.
// Video frames are BT.601 limited-range planes: luma 1280x720, then the
// blue and red chroma planes, half size, 640 texels on a 768 pitch.
// Chroma moves by a * (new still - Wii U still).
struct Logo {
    static constexpr uint32_t kWidth = 1280, kHeight = 720;
    static constexpr uint32_t kBlueOffset = kWidth * kHeight, kRedOffset = kBlueOffset + 768 * (kHeight / 2);
    static constexpr float kNoise = 10.0f; // coding noise, in luma steps
    static constexpr int kBlur = 5;        // radius of the two box blurs
    std::vector<float> from, to;           // luma of the stills (Wii U, new)
    std::vector<uint8_t> differ;           // the stills differ within three texels
    std::vector<int16_t> blue, red;        // chroma: new still - Wii U still
    std::vector<uint32_t> samples, background; // texels to fit with; those black in the Wii U still
    double mean = 0, variance = 0;         // of the Wii U still's luma at the samples
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the logo's part of the frame
    std::vector<float> rest, blurred, scratch;
    std::array<std::pair<uint32_t, float>, 4> recent{}; // luma plane address, a
    size_t next = 0;
};
Logo g_logo;

void ycbcr(const uint8_t* rgb, double& y, double& cb, double& cr) {
    y = 16 + 0.257 * rgb[0] + 0.504 * rgb[1] + 0.098 * rgb[2];
    cb = 128 - 0.148 * rgb[0] - 0.291 * rgb[1] + 0.439 * rgb[2];
    cr = 128 + 0.439 * rgb[0] - 0.368 * rgb[1] - 0.071 * rgb[2];
}

// A box blur of a width x height image, in place.
void box_blur(std::vector<float>& image, uint32_t width, uint32_t height, int radius, std::vector<float>& line) {
    const auto pass = [&](uint32_t count, uint32_t length, size_t step, size_t stride) {
        line.resize(length);
        for (uint32_t c = 0; c < count; ++c) {
            float* v = image.data() + c * stride;
            for (uint32_t i = 0; i < length; ++i) line[i] = v[i * step];
            const auto at = [&](int64_t i) { return line[static_cast<size_t>(std::clamp<int64_t>(i, 0, length - 1))]; };
            double sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += at(k);
            for (uint32_t i = 0; i < length; ++i) {
                v[i * step] = static_cast<float>(sum / (2 * radius + 1));
                sum += at(int64_t{i} + radius + 1) - at(int64_t{i} - radius);
            }
        }
    };
    pass(height, width, 1, width); // rows
    pass(width, height, width, 1); // columns
}

void filter_logo(uint32_t address, uint32_t width, uint32_t height, uint8_t* texels) {
    Logo& l = g_logo;
    if (width == Logo::kWidth && height == Logo::kHeight) {
        double sum = 0, cross = 0, squares = 0;
        for (const uint32_t i : l.samples) {
            const double v = texels[i];
            sum += v;
            cross += v * l.from[i];
            squares += v * v;
        }
        const double n = static_cast<double>(l.samples.size());
        const double mean = sum / n, variance = squares / n - mean * mean;
        const double covariance = cross / n - mean * l.mean;
        const double a = covariance / l.variance;
        const double fit = variance > 1 ? covariance * covariance / (l.variance * variance) : 0;
        const bool logo = a > 0.05 && fit > 0.5;
        l.recent[l.next++ % l.recent.size()] = {address, logo ? static_cast<float>(std::min(a, 1.0)) : 0.0f};
        if (!logo) return;
        l.scratch.clear();
        for (const uint32_t i : l.background) l.scratch.push_back(static_cast<float>(texels[i] - a * l.from[i]));
        const auto middle = l.scratch.begin() + static_cast<ptrdiff_t>(l.scratch.size() / 2);
        std::nth_element(l.scratch.begin(), middle, l.scratch.end());
        const float b = *middle;
        const uint32_t w = l.x1 - l.x0, h = l.y1 - l.y0;
        l.rest.resize(size_t{w} * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = size_t{l.y0 + y} * width + l.x0 + x;
                const float r = texels[i] - static_cast<float>(a * l.from[i]) - b;
                l.rest[size_t{y} * w + x] = r > Logo::kNoise ? r - Logo::kNoise : r < -Logo::kNoise ? r + Logo::kNoise : 0.0f;
            }
        }
        l.blurred = l.rest;
        box_blur(l.blurred, w, h, Logo::kBlur, l.scratch);
        box_blur(l.blurred, w, h, Logo::kBlur, l.scratch);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = size_t{l.y0 + y} * width + l.x0 + x, j = size_t{y} * w + x;
                const float v = static_cast<float>(a * l.to[i]) + b + (l.differ[i] ? l.blurred[j] : l.rest[j]);
                texels[i] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
            }
        }
        return;
    }
    if (width != Logo::kWidth / 2 || height != Logo::kHeight / 2) return;
    for (const auto& [luma, weight] : l.recent) {
        if (weight == 0 || luma == 0) continue;
        const std::vector<int16_t>* delta = address == luma + Logo::kBlueOffset ? &l.blue
                                            : address == luma + Logo::kRedOffset ? &l.red
                                                                                 : nullptr;
        if (delta == nullptr) continue;
        for (uint32_t y = l.y0 / 2; y < l.y1 / 2; ++y) {
            for (uint32_t x = l.x0 / 2; x < l.x1 / 2; ++x) {
                const size_t i = size_t{y} * width + x;
                texels[i] = static_cast<uint8_t>(std::clamp(std::lround(texels[i] + weight * (*delta)[i]), 0L, 255L));
            }
        }
        return;
    }
}

void prepare_logo(const ntp3::Texture& still) {
    const auto from = ntp3::decode(still);
    const auto to = from ? title_screen::logo(*from) : std::nullopt;
    if (!to) return;
    constexpr uint32_t W = Logo::kWidth, H = Logo::kHeight;
    Logo& l = g_logo;
    l.from.resize(size_t{W} * H);
    l.to.resize(size_t{W} * H);
    std::vector<uint32_t> differs(size_t{W + 1} * (H + 1), 0); // prefix sums of |to - from| > 12
    l.x0 = W;
    l.y0 = H;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            double fy, ty, ignored1, ignored2;
            ycbcr(from->at(x, y), fy, ignored1, ignored2);
            ycbcr(to->at(x, y), ty, ignored1, ignored2);
            const size_t i = size_t{y} * W + x;
            l.from[i] = static_cast<float>(fy);
            l.to[i] = static_cast<float>(ty);
            const bool differ = std::abs(ty - fy) > 12;
            differs[size_t{y + 1} * (W + 1) + x + 1] = differs[size_t{y} * (W + 1) + x + 1] +
                                                       differs[size_t{y + 1} * (W + 1) + x] -
                                                       differs[size_t{y} * (W + 1) + x] + (differ ? 1 : 0);
            if (fy > 20 || ty > 20) {
                l.x0 = std::min(l.x0, x);
                l.y0 = std::min(l.y0, y);
                l.x1 = std::max(l.x1, x + 1);
                l.y1 = std::max(l.y1, y + 1);
            }
        }
    }
    if (l.x1 <= l.x0) return;
    // Room for the blur, on even texels for the half-size chroma.
    constexpr uint32_t kMargin = 4 * Logo::kBlur;
    l.x0 = (l.x0 > kMargin ? l.x0 - kMargin : 0) & ~1u;
    l.y0 = (l.y0 > kMargin ? l.y0 - kMargin : 0) & ~1u;
    l.x1 = std::min(W, (l.x1 + kMargin + 1) & ~1u);
    l.y1 = std::min(H, (l.y1 + kMargin + 1) & ~1u);
    l.differ.assign(size_t{W} * H, 0);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t ax = x >= 3 ? x - 3 : 0, ay = y >= 3 ? y - 3 : 0;
            const uint32_t bx = std::min(W, x + 4), by = std::min(H, y + 4);
            const uint32_t count = differs[size_t{by} * (W + 1) + bx] - differs[size_t{ay} * (W + 1) + bx] -
                                   differs[size_t{by} * (W + 1) + ax] + differs[size_t{ay} * (W + 1) + ax];
            l.differ[size_t{y} * W + x] = count > 0;
        }
    }
    l.blue.assign(size_t{W / 2} * (H / 2), 0);
    l.red = l.blue;
    for (uint32_t y = 0; y < H / 2; ++y) {
        for (uint32_t x = 0; x < W / 2; ++x) {
            uint8_t average[2][3];
            for (int s = 0; s < 2; ++s) {
                for (int c = 0; c < 3; ++c) {
                    uint32_t sum = 0;
                    for (uint32_t k = 0; k < 4; ++k) sum += (s ? *to : *from).at(2 * x + k % 2, 2 * y + k / 2)[c];
                    average[s][c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
            double ignored, fcb, fcr, tcb, tcr;
            ycbcr(average[0], ignored, fcb, fcr);
            ycbcr(average[1], ignored, tcb, tcr);
            l.blue[size_t{y} * (W / 2) + x] = static_cast<int16_t>(std::lround(tcb - fcb));
            l.red[size_t{y} * (W / 2) + x] = static_cast<int16_t>(std::lround(tcr - fcr));
        }
    }
    // Fit on every fourth texel both ways.
    double sum = 0, squares = 0;
    for (uint32_t y = l.y0; y < l.y1; y += 4) {
        for (uint32_t x = l.x0; x < l.x1; x += 4) {
            const uint32_t i = y * W + x;
            l.samples.push_back(i);
            if (l.from[i] < 24) l.background.push_back(i);
            sum += l.from[i];
            squares += double{l.from[i]} * l.from[i];
        }
    }
    const double n = static_cast<double>(l.samples.size());
    l.mean = sum / n;
    l.variance = squares / n - l.mean * l.mean;
    if (l.variance <= 0 || l.background.empty()) return;
    gpu::set_plane_filter(filter_logo);
}

void prepare_title_screen() {
    const auto file = game_entry(kTitleArchive, title_screen::kTextures);
    const auto set = file ? ntp3::parse(*file) : std::nullopt;
    if (!set || set->empty()) {
        std::fprintf(stderr, "ttt2: warning: the title screen's pictures are not where expected; its logo stays\n");
        return;
    }
    prepare_copyright(*set);
    prepare_logo(set->front());
}

} // namespace

void system_info_created(uint32_t info) {
    g_system_info = info;
    apply_confirm_button();
}

void prepare_presentation() {
    const auto begin = std::chrono::steady_clock::now();
    // Texts name buttons by code points only the changed fonts show as icons.
    std::vector<Bank> banks = read_banks(kFontArchive), patch = read_banks(kPatchArchive);
    banks.insert(banks.end(), std::make_move_iterator(patch.begin()), std::make_move_iterator(patch.end()));
    std::set<char32_t> used;
    for (const Bank& b : banks) text::code_points(b.contents, used);
    std::map<std::string, std::map<uint32_t, std::vector<uint8_t>>> changed;
    changed[kFontArchive] = prepare_icons(used);
    if (!changed[kFontArchive].empty()) {
        size_t rewritten = 0;
        for (Bank& b : banks) {
            if (text::show_button_icons(b.contents) == 0) continue;
            changed[b.archive][b.hash] = archive::pack(b.contents, b.hash, b.flags);
            ++rewritten;
        }
        for (const auto& [archive_name, entries] : changed) {
            if (!vfs::set_archive_entries(archive_name, entries)) {
                std::fprintf(stderr, "ttt2: warning: cannot change %s; texts may name the Wii U's buttons\n",
                             archive_name.c_str());
            }
        }
        if (std::getenv("TTT2_TRACE_PRESENTATION")) {
            std::fprintf(stderr, "ttt2: %zu text banks, %zu with button names changed\n", banks.size(), rewritten);
        }
    }
    prepare_title_screen();
    if (std::getenv("TTT2_TRACE_PRESENTATION")) {
        std::fprintf(stderr, "ttt2: presentation prepared in %.0f ms\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
    }
    host::on_controller_change([](host::Controller kind) {
        g_controller = kind;
        apply_confirm_button();
        gpu::texture_replacements_changed();
    });
}

} // namespace cafe::title

// The title builds its system settings object here: the PlayStation 3's
// confirm-button setting is fixed to the right button on the Wii U.
PPC_FUNC(sub_0864107C_orig);
PPC_FUNC(sub_0864107C) {
    const uint32_t out = ctx.r[3];
    sub_0864107C_orig(ctx, base);
    cafe::title::system_info_created(PPC_LOAD_U32(out));
}
