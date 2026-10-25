// The title's file formats and the pictures made from them, on synthetic
// data (no game files needed): archive scrambling, packing and rebuilding,
// texture decoding, the font's glyph table and icon sets, the text banks'
// button names, the copyright line without "Wii U Edition", and a still
// without the logo's subtitle getting none.

#include "title/archive.h"
#include "title/draw.h"
#include "title/font.h"
#include "title/image.h"
#include "title/text.h"
#include "title/title_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace cafe::title;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}
uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

// ------------------------------------------------------------------ archives

void test_scramble() {
    // The stream: four words seeded x = x * 0xBDE95 + 0x8EF345 from the hash,
    // XORed big-endian over each 16-byte block, each word then stepping
    // w = w * 1531 + 5011.
    const uint32_t hash = 0x12345678;
    std::vector<uint8_t> data(37, 0);
    archive::scramble(data.data(), data.size(), hash);
    uint32_t state[4], x = hash;
    for (uint32_t& s : state) s = x = x * 0xBDE95u + 0x8EF345u;
    std::vector<uint8_t> expected;
    while (expected.size() < data.size()) {
        for (uint32_t& s : state) {
            put_be32(expected, s);
            s = s * 1531u + 5011u;
        }
    }
    expected.resize(data.size());
    check(data == expected, "scramble: the keystream");
    archive::scramble(data.data(), data.size(), hash);
    check(data == std::vector<uint8_t>(37, 0), "scramble: twice is nothing");
}

struct Archive {
    std::vector<uint8_t> ofs, bin;
};

std::vector<uint8_t> chunk(const char* tag, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> c;
    put_be32(c, static_cast<uint32_t>(8 + payload.size()));
    c.insert(c.end(), tag, tag + 4);
    c.insert(c.end(), payload.begin(), payload.end());
    return c;
}

// An archive of entries as stored, laid out as the title's are.
Archive make_archive(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& entries) {
    std::vector<uint8_t> names, offsets, payload;
    for (const auto& [hash, stored] : entries) {
        put_be32(names, hash);
        put_be32(names, 0);
        put_be32(offsets, static_cast<uint32_t>(payload.size()) | (stored.size() % 16));
        payload.insert(payload.end(), stored.begin(), stored.end());
        payload.resize((payload.size() + 15) & ~size_t{15});
    }
    put_be32(offsets, static_cast<uint32_t>(payload.size()));
    Archive a;
    for (const auto& c : {chunk("ftyp", bytes("nrfo....")), chunk("head", std::vector<uint8_t>(20)), chunk("ofsi", names),
                          chunk("ofsd", offsets)}) {
        a.ofs.insert(a.ofs.end(), c.begin(), c.end());
    }
    a.bin.assign(archive::kPayload - 8, 0);
    put_be32(a.bin, static_cast<uint32_t>(8 + payload.size()));
    a.bin.insert(a.bin.end(), {'i', 'm', 'g', 'e'});
    a.bin.insert(a.bin.end(), payload.begin(), payload.end());
    return a;
}

std::vector<uint8_t> contents_of(const Archive& a, uint32_t hash) {
    const auto index = archive::parse_index(a.ofs);
    const archive::Entry* e = index ? index->find(hash) : nullptr;
    if (!e || archive::kPayload + e->offset + e->size > a.bin.size()) return {};
    std::vector<uint8_t> stored(a.bin.begin() + static_cast<ptrdiff_t>(archive::kPayload + e->offset),
                                a.bin.begin() + static_cast<ptrdiff_t>(archive::kPayload + e->offset + e->size));
    return archive::unpack(stored, hash).value_or(std::vector<uint8_t>{});
}

void test_archive() {
    const uint8_t flags[8] = {0x30, 0x14, 0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> big(5000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7 % 251);
    std::vector<uint8_t> raw = bytes("not compressed, 37 bytes long......."), raw_stored = raw;
    archive::scramble(raw_stored.data(), raw_stored.size(), 0xBBBB);
    const std::vector<uint8_t> small = bytes("sixteen bytes!!!");
    const Archive a = make_archive({{0xAAAA, archive::pack(big, 0xAAAA, flags)},
                                    {0xBBBB, raw_stored},
                                    {0xCCCC, archive::pack(small, 0xCCCC, flags)}});
    check(contents_of(a, 0xAAAA) == big, "archive: a compressed entry");
    check(contents_of(a, 0xBBBB) == raw, "archive: an entry stored as it is");
    check(contents_of(a, 0xCCCC) == small, "archive: the last entry");

    const auto index = archive::parse_index(a.ofs);
    const archive::Entry* e = index->find(0xAAAA);
    std::vector<uint8_t> start(a.bin.begin() + archive::kPayload, a.bin.begin() + archive::kPayload + 64);
    const auto first = archive::peek(start, 0xAAAA, 8);
    check(first == std::vector<uint8_t>(big.begin(), big.begin() + 8), "archive: peeking into a compressed entry");
    uint8_t got[8];
    archive::stored_flags(std::vector<uint8_t>(a.bin.begin() + archive::kPayload, a.bin.begin() + archive::kPayload + 16),
                          0xAAAA, got);
    check(std::memcmp(got, flags, 8) == 0 && e->offset == 0, "archive: an entry's NRCZ flags");

    // The middle entry replaced by a longer one, and one added.
    std::vector<uint8_t> longer(300, 'x'), added = bytes("an entry the archive did not hold");
    const auto rebuilt = archive::rebuild(a.ofs, std::vector<uint8_t>(a.bin.begin(), a.bin.begin() + archive::kPayload),
                                          {{0xBBBB, archive::pack(longer, 0xBBBB, flags)},
                                           {0xDDDD, archive::pack(added, 0xDDDD, flags)}});
    check(rebuilt.has_value(), "rebuild: an archive with a replaced and an added entry");
    if (!rebuilt) return;
    Archive b;
    b.ofs = rebuilt->ofs;
    b.bin.resize(rebuilt->bin_size);
    for (const archive::Piece& p : rebuilt->bin) {
        if (p.data) std::memcpy(&b.bin[p.at], p.data->data(), p.size);
        else std::memcpy(&b.bin[p.at], &a.bin[p.source], p.size);
    }
    check(contents_of(b, 0xAAAA) == big && contents_of(b, 0xBBBB) == longer && contents_of(b, 0xCCCC) == small &&
              contents_of(b, 0xDDDD) == added,
          "rebuild: every entry read back");
    const auto rebuilt_index = archive::parse_index(b.ofs);
    check(rebuilt_index && rebuilt_index->entries.size() == 4 && rebuilt_index->entries.back().hash == 0xDDDD,
          "rebuild: the added entry is the last");
    check(b.ofs.size() == a.ofs.size() + 12 && std::equal(a.ofs.begin(), a.ofs.begin() + 8 + 8 + 8 + 20, b.ofs.begin()),
          "rebuild: the index's other chunks kept");
    check(be32(&b.bin[archive::kPayload - 8]) == b.bin.size() - archive::kPayload + 8, "rebuild: the payload's size");
}

// ------------------------------------------------------------------ textures

void test_bc() {
    // BC1: red and blue endpoints, texel 0 index 0, texel 1 index 1, texel 2
    // index 2 (2/3 red), texel 3 index 3 (1/3 red).
    const uint8_t bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0};
    const Rgba a = decode_bc(1, bc1, 4, 4);
    check(a.at(0, 0)[0] == 255 && a.at(0, 0)[2] == 0, "BC1: the first endpoint");
    check(a.at(1, 0)[0] == 0 && a.at(1, 0)[2] == 255, "BC1: the second endpoint");
    check(a.at(2, 0)[0] == 170 && a.at(3, 0)[0] == 85, "BC1: the colours between");
    // BC3: alpha endpoints 255 and 0, texel 0 index 0, texel 1 index 1.
    uint8_t bc3[16] = {255, 0, 0b00001000, 0, 0, 0, 0, 0};
    std::memcpy(bc3 + 8, bc1, 8);
    const Rgba b = decode_bc(3, bc3, 4, 4);
    check(b.at(0, 0)[3] == 255 && b.at(1, 0)[3] == 0, "BC3: alpha");
}

// ------------------------------------------------------------------ fonts

// A glyph table, little-endian like the main font's or big-endian like
// the plain font's.
std::vector<uint8_t> make_nfh(std::vector<std::pair<uint16_t, font::Glyph>> glyphs, bool big_endian = false) {
    std::sort(glyphs.begin(), glyphs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint8_t> nfh(0x450, 0);
    std::memcpy(nfh.data(), "NFH", 4);
    const auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) nfh[at + i] = static_cast<uint8_t>(v >> (big_endian ? 24 - 8 * i : 8 * i));
    };
    put32(4, 24); // pixels
    put32(8, static_cast<uint32_t>(glyphs.size()));
    const auto put16 = [&](uint8_t* p, uint32_t v) {
        p[big_endian ? 0 : 1] = static_cast<uint8_t>(v >> 8);
        p[big_endian ? 1 : 0] = static_cast<uint8_t>(v);
    };
    for (const auto& [code, g] : glyphs) {
        uint8_t r[32] = {};
        put16(r + 4, g.x);
        put16(r + 6, g.y);
        put16(r + 12, g.width * 64); // the advance
        r[20] = static_cast<uint8_t>(g.width);
        r[21] = static_cast<uint8_t>(g.height);
        r[22] = static_cast<uint8_t>(code >> 8);
        r[23] = static_cast<uint8_t>(code);
        nfh.insert(nfh.end(), r, r + 32);
    }
    return nfh;
}

bool same_box(const Rgba& a, const Rgba& b, const font::Glyph& g) {
    for (uint32_t y = g.y; y < g.y + g.height; ++y) {
        if (std::memcmp(a.at(g.x, y), b.at(g.x, y), g.width * 4) != 0) return false;
    }
    return true;
}

void test_font() {
    const std::vector<std::pair<uint16_t, font::Glyph>> table = {
        {0xE024, {0, 0, 22, 22}},  {0xE01C, {24, 0, 22, 22}}, {0xE036, {48, 0, 22, 22}},
        {0x2282, {0, 24, 36, 17}}, {'L', {40, 24, 10, 14}},   {'1', {52, 24, 10, 14}}};
    const auto glyphs = font::parse(make_nfh(table));
    check(glyphs && glyphs->size() == table.size() && glyphs->at(0x2282).width == 36 && glyphs->at('1').x == 52,
          "font: the glyph table");
    if (!glyphs) return;
    font::Font f{*glyphs, blank(96, 64)};
    for (uint32_t y = 0; y < 22; ++y) { // the lit-dot picture: a white square
        for (uint32_t x = 30; x < 40; ++x) std::memset(f.texture.at(x, y), 255, 4);
    }
    for (uint32_t y = 0; y < 64; ++y) f.texture.at(90, y)[1] = 77; // outside every box
    const Rgba neutral = font::neutral_icons(f);
    const font::Glyph source = glyphs->at(0xE01C);
    check(same_box(neutral, f.texture, source), "neutral icons: the dot pictures stay");
    bool copied = true;
    for (uint32_t y = 0; y < 22; ++y) copied = copied && std::memcmp(neutral.at(0, y), f.texture.at(24, y), 22 * 4) == 0;
    check(copied, "neutral icons: the right button shows the right dot lit");
    check(neutral.at(90, 10)[1] == 77, "neutral icons: the rest of the texture stays");
    const Rgba playstation = font::playstation_icons(f);
    check(!same_box(playstation, f.texture, glyphs->at(0xE024)) && !same_box(playstation, f.texture, glyphs->at(0x2282)),
          "PlayStation icons: drawn in the face button's and L1's boxes");
    check(playstation.at(90, 10)[1] == 77, "PlayStation icons: the rest of the texture stays");
}

void test_glyph_tables() {
    for (const bool big_endian : {false, true}) {
        const std::string kind = big_endian ? " (big-endian)" : " (little-endian)";
        auto nfh = make_nfh({{'A', {0, 0, 20, 24}}, {0xA3, {30, 0, 22, 25}}, {0xE024, {60, 0, 22, 22}}}, big_endian);
        const auto glyphs = font::parse(nfh);
        check(glyphs && glyphs->size() == 3 && glyphs->at(0xA3).x == 30 && glyphs->at(0xA3).height == 25,
              "font: a glyph table" + kind);
        const auto record = [&](uint16_t code) {
            for (size_t at = 0x450; at + 32 <= nfh.size(); at += 32) {
                const uint8_t* r = &nfh[at + 4];
                if ((r[18] << 8 | r[19]) == code) return std::vector<uint8_t>(r, r + 20);
            }
            return std::vector<uint8_t>{};
        };
        check(font::copy_glyph(nfh, 0xA3, 0xE024), "font: copying a glyph" + kind);
        const auto copied = font::parse(nfh);
        const auto a3 = record(0xA3), e024 = record(0xE024);
        check(copied && copied->at(0xA3).x == 60 && copied->at(0xA3).width == 22 &&
                  std::equal(a3.begin(), a3.begin() + 18, e024.begin()) && a3[18] == 0x00 && a3[19] == 0xA3,
              "font: a copied glyph has the other's box and placement, and its code point" + kind);
        check(font::set_glyph(nfh, 'A', {100, 50, 21, 21}, {0, 19, 22}), "font: setting a glyph" + kind);
        const auto set = font::parse(nfh);
        const auto a = record('A');
        const auto at16 = [&](size_t i) { return big_endian ? a[i] << 8 | a[i + 1] : a[i] | a[i + 1] << 8; };
        check(set && set->at('A').x == 100 && set->at('A').y == 50 && set->at('A').width == 21 && at16(6) == 19 * 64 &&
                  at16(8) == 22 * 64,
              "font: a set glyph's box and placement" + kind);
        check(!font::copy_glyph(nfh, 0xB3, 'A') && !font::set_glyph(nfh, 0xB3, {}, {}),
              "font: a glyph the table lacks" + kind);
    }
    auto unsorted = make_nfh({{'A', {0, 0, 1, 1}}, {'B', {0, 0, 1, 1}}});
    std::swap_ranges(unsorted.begin() + 0x450, unsorted.begin() + 0x470, unsorted.begin() + 0x470);
    check(!font::parse(unsorted), "font: a table out of code point order is not read");
}

void test_draw() {
    draw::Canvas c(20, 20);
    c.fill(draw::Shape::circle(20, 20, {10, 10}, 6), draw::Colour::rgb(0xFFFFFF));
    const Rgba r = c.result();
    double area = 0;
    for (uint32_t y = 0; y < 20; ++y) {
        for (uint32_t x = 0; x < 20; ++x) area += r.at(x, y)[3] / 255.0;
    }
    check(std::abs(area - 3.14159 * 36) < 2.0, "draw: a circle's area");
}

// ------------------------------------------------------------------ text banks

struct Record {
    std::string key, text, english;
};

size_t pad(size_t bytes, size_t zeros) { return (bytes + zeros + 3) & ~size_t{3}; }

std::vector<uint8_t> make_bank(const std::vector<Record>& records, bool bad_count = false) {
    std::vector<uint8_t> bank(0x30, 0);
    std::memcpy(bank.data(), "NTXB", 4);
    for (const Record& r : records) {
        std::vector<uint8_t> key(r.key.begin(), r.key.end());
        key.resize(pad(key.size(), 1), 0);
        bank.insert(bank.end(), key.begin(), key.end());
        size_t count = 0;
        for (const char c : r.text) count += (c & 0xC0) != 0x80;
        if (bad_count) ++count;
        bank.insert(bank.end(), {0xAB, 0xCD, static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count)});
        for (const std::string* s : {&r.text, &r.english}) {
            std::vector<uint8_t> t(s->begin(), s->end());
            t.resize(pad(t.size(), 2), 0);
            bank.insert(bank.end(), t.begin(), t.end());
        }
    }
    return bank;
}

// The bank's texts and their lengths, in order; a shortened text keeps its
// space (zero bytes up to the next part).
std::vector<std::pair<std::string, size_t>> texts(const std::vector<uint8_t>& bank) {
    std::vector<std::pair<std::string, size_t>> out;
    size_t at = 0x30;
    const auto skip_zeros = [&] {
        while (at < bank.size() && bank[at] == 0) ++at;
    };
    while (at < bank.size()) {
        const std::string key(reinterpret_cast<const char*>(&bank[at]));
        at += pad(key.size(), 1);
        const size_t count = size_t{bank[at + 2]} << 8 | bank[at + 3];
        at += 4;
        const std::string text(reinterpret_cast<const char*>(&bank[at]));
        at += text.size();
        skip_zeros();
        const std::string english(reinterpret_cast<const char*>(&bank[at]));
        at += english.size();
        skip_zeros();
        out.push_back({text, count});
    }
    return out;
}

void test_text() {
    const std::string start = "\xEF\xBC\xBC", select = "\xC2\xB3", a = "\xC2\xA3", b = "\xC2\xA5", x = "\xC2\xA2",
                      y = "\xC2\xAA", pad_icon = "\xC2\xBB";
    auto bank = make_bank({
        {"common_4", "PRESS THE + BUTTON", "PRESS THE + BUTTON"},
        {"practice_987", "Press the - Button and B Button to set.", "Press the - Button and B Button "},
        {"common_9", "A DATA Button is no button.", "A DATA Button is no button."},
        {"common_10", "Appuyer sur le bouton Annuler.", "Press Cancel."},
        {"Manu_1692", "Attack using the A, B, X, and Y Buttons.", "Attack using the A, B, X, and Y"},
        {"common_479", "Press left on the +Control Pad.", "Press left on the +Control Pad."},
        {"common_12", "Drücke den X-Knopf.", "Press the X Button."},
    });
    const int changed = text::show_button_icons(bank);
    const auto t = texts(bank);
    check(changed == 5, "text: five texts name buttons");
    check(t.size() == 7, "text: the records still parse");
    if (t.size() != 7) return;
    check(t[0].first == "PRESS " + start + " BUTTON" && t[0].second == 14, "text: the prompt to press Start");
    check(t[1].first == "Press the " + select + " and " + b + " to set.", "text: the - and B buttons");
    check(t[2].first == "A DATA Button is no button.", "text: only whole button names");
    check(t[3].first == "Appuyer sur le bouton Annuler.", "text: not a word starting with a button's letter");
    check(t[4].first == "Attack using the " + a + ", " + b + ", " + x + ", and " + y + ".", "text: a list of buttons");
    check(t[5].first == "Press left on the " + pad_icon + ".", "text: the +Control Pad");
    check(t[6].first == "Drücke den " + x + ".", "text: German");
    auto wrong = make_bank({{"common_4", "PRESS THE + BUTTON", "PRESS THE + BUTTON"}}, true);
    check(text::show_button_icons(wrong) == 0, "text: a record whose length does not match is left alone");

    std::set<char32_t> used;
    text::code_points(make_bank({{"common_5", "Drücke \xE2\x94\xBC!", "Press the pad!"}}), used);
    check(used == std::set<char32_t>{U'D', U'r', U'\u00FC', U'c', U'k', U'e', U' ', U'\u253C', U'!'},
          "text: the code points texts use (not the English originals')");
}

// ------------------------------------------------------------------ the copyright line

void test_copyright_line() {
    // 14 words of white letters with a dark edge, two letters each.
    Rgba line = blank(700, 32);
    std::vector<uint32_t> starts;
    uint32_t x = 30;
    for (int word = 0; word < 14; ++word) {
        starts.push_back(x);
        for (int letter = 0; letter < 2; ++letter) {
            for (uint32_t yy = 8; yy < 24; ++yy) {
                for (uint32_t xx = x - 2; xx < x + 10; ++xx) {
                    uint8_t* p = line.at(xx, yy);
                    if (p[3] == 0) p[3] = 200; // the edge
                }
                for (uint32_t xx = x; xx < x + 8; ++xx) std::memset(line.at(xx, yy), 255, 4);
            }
            x += 10;
        }
        x += 12;
    }
    const auto out = title_screen::copyright_line(line);
    check(out && out->width == 700 && out->height == 32, "copyright line: made");
    if (!out) return;
    // Words left: 11 (the three before the last seven gone); centred.
    uint32_t left = 700, right = 0, runs = 0;
    bool inside = false;
    uint32_t gap = 0;
    for (uint32_t c = 0; c < 700; ++c) {
        const bool ink = out->at(c, 16)[0] == 255 && out->at(c, 16)[3] == 255;
        if (out->at(c, 16)[3] != 0) {
            left = std::min(left, c);
            right = c + 1;
        }
        if (ink) {
            if (!inside && gap >= 5) ++runs;
            if (!inside && runs == 0) runs = 1;
            inside = true;
            gap = 0;
        } else {
            inside = false;
            ++gap;
        }
    }
    check(runs == 11, "copyright line: three words fewer (" + std::to_string(runs) + ")");
    check(std::abs(static_cast<int>(left) - static_cast<int>(700 - right)) <= 1, "copyright line: centred");
    check(!title_screen::copyright_line(blank(700, 32)), "copyright line: none for a line without the words");
}

// ------------------------------------------------------------------ the logo

void test_logo() {
    // Only the Wii U still has the strokes the new subtitle is made from.
    check(!title_screen::logo(blank(1280, 720)), "logo: none for a still without the subtitle");
    check(!title_screen::logo(blank(640, 360)), "logo: none for another size");
}

} // namespace

int main() {
    test_scramble();
    test_archive();
    test_bc();
    test_font();
    test_glyph_tables();
    test_draw();
    test_text();
    test_copyright_line();
    test_logo();
    if (g_failures == 0) std::printf("title_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
