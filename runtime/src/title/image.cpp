#include "title/image.h"

#include <algorithm>
#include <cstring>

namespace cafe::title {
namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

void rgb565(uint16_t c, uint8_t* out) {
    const uint32_t r = c >> 11, g = (c >> 5) & 0x3F, b = c & 0x1F;
    out[0] = static_cast<uint8_t>(r << 3 | r >> 2);
    out[1] = static_cast<uint8_t>(g << 2 | g >> 4);
    out[2] = static_cast<uint8_t>(b << 3 | b >> 2);
    out[3] = 255;
}

// The colour half of a block: two RGB565 endpoints and 2-bit indices.
// Only BC1 has the three-colour mode with transparent black.
void colour_block(const uint8_t* block, bool bc1, uint8_t (&texels)[16][4]) {
    const uint16_t c0 = le16(block), c1 = le16(block + 2);
    uint8_t palette[4][4];
    rgb565(c0, palette[0]);
    rgb565(c1, palette[1]);
    if (!bc1 || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            palette[2][k] = static_cast<uint8_t>((2 * palette[0][k] + palette[1][k] + 1) / 3);
            palette[3][k] = static_cast<uint8_t>((palette[0][k] + 2 * palette[1][k] + 1) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) palette[2][k] = static_cast<uint8_t>((palette[0][k] + palette[1][k]) / 2);
        palette[2][3] = 255;
        std::memset(palette[3], 0, 4);
    }
    const uint32_t indices = le32(block + 4);
    for (int i = 0; i < 16; ++i) std::memcpy(texels[i], palette[(indices >> (2 * i)) & 3], 4);
}

// BC3's alpha half: two endpoints and 3-bit indices.
void alpha_block(const uint8_t* block, uint8_t (&texels)[16][4]) {
    const uint32_t a0 = block[0], a1 = block[1];
    uint8_t palette[8] = {static_cast<uint8_t>(a0), static_cast<uint8_t>(a1)};
    if (a0 > a1) {
        for (uint32_t i = 1; i < 7; ++i) palette[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1 + 3) / 7);
    } else {
        for (uint32_t i = 1; i < 5; ++i) palette[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1 + 2) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t{block[2 + i]} << (8 * i);
    for (int i = 0; i < 16; ++i) texels[i][3] = palette[(bits >> (3 * i)) & 7];
}

} // namespace

Rgba blank(uint32_t width, uint32_t height) {
    Rgba image;
    image.width = width;
    image.height = height;
    image.pixels.assign(size_t{width} * height * 4, 0);
    return image;
}

Rgba crop(const Rgba& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    Rgba out = blank(width, height);
    for (uint32_t row = 0; row < height; ++row) {
        if (y + row >= image.height) break;
        const uint32_t n = std::min(width, image.width > x ? image.width - x : 0);
        std::memcpy(out.at(0, row), image.at(std::min(x, image.width), y + row), size_t{n} * 4);
    }
    return out;
}

Rgba resize(const Rgba& image, uint32_t width, uint32_t height) {
    Rgba out = blank(width, height);
    if (image.width == 0 || image.height == 0) return out;
    const double sx = static_cast<double>(image.width) / width, sy = static_cast<double>(image.height) / height;
    for (uint32_t y = 0; y < height; ++y) {
        const double y0 = y * sy, y1 = (y + 1) * sy;
        for (uint32_t x = 0; x < width; ++x) {
            const double x0 = x * sx, x1 = (x + 1) * sx;
            // Premultiplied, so transparent texels do not bleed their colour.
            double sum[4] = {}, total = 0;
            for (uint32_t iy = static_cast<uint32_t>(y0); iy < image.height && iy < y1; ++iy) {
                const double wy = std::min<double>(iy + 1, y1) - std::max<double>(iy, y0);
                for (uint32_t ix = static_cast<uint32_t>(x0); ix < image.width && ix < x1; ++ix) {
                    const double w = wy * (std::min<double>(ix + 1, x1) - std::max<double>(ix, x0));
                    const uint8_t* p = image.at(ix, iy);
                    const double a = p[3] / 255.0;
                    for (int k = 0; k < 3; ++k) sum[k] += w * p[k] * a;
                    sum[3] += w * p[3];
                    total += w;
                }
            }
            uint8_t* o = out.at(x, y);
            if (total <= 0 || sum[3] <= 0) continue;
            const double alpha = sum[3] / total;
            for (int k = 0; k < 3; ++k) o[k] = static_cast<uint8_t>(std::clamp(sum[k] / total / (alpha / 255.0) + 0.5, 0.0, 255.0));
            o[3] = static_cast<uint8_t>(std::clamp(alpha + 0.5, 0.0, 255.0));
        }
    }
    return out;
}

void put(Rgba& into, const Rgba& image, uint32_t x, uint32_t y) {
    for (uint32_t row = 0; row < image.height && y + row < into.height; ++row) {
        if (x >= into.width) break;
        const uint32_t n = std::min(image.width, into.width - x);
        std::memcpy(into.at(x, y + row), image.at(0, row), size_t{n} * 4);
    }
}

Rgba decode_bc(int bc, const uint8_t* data, uint32_t width, uint32_t height) {
    Rgba out = blank(width, height);
    const size_t block_size = bc == 1 ? 8 : 16;
    const uint32_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const uint8_t* block = data + (size_t{by} * blocks_x + bx) * block_size;
            uint8_t texels[16][4];
            colour_block(block + (bc == 1 ? 0 : 8), bc == 1, texels);
            if (bc == 2) {
                for (int i = 0; i < 16; ++i) {
                    const uint32_t a = (block[i / 2] >> (4 * (i % 2))) & 0xF;
                    texels[i][3] = static_cast<uint8_t>(a * 17);
                }
            } else if (bc == 3) {
                alpha_block(block, texels);
            }
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < width && y < height) std::memcpy(out.at(x, y), texels[i], 4);
            }
        }
    }
    return out;
}

namespace ntp3 {

size_t Texture::level0_size() const {
    const size_t blocks = size_t{(width + 3) / 4} * ((height + 3) / 4);
    switch (format) {
    case kBc1: return blocks * 8;
    case kBc2: case kBc3: return blocks * 16;
    case kArgb8: return size_t{width} * height * 4;
    default: return 0;
    }
}

uint32_t Texture::gx2_format() const {
    switch (format) {
    case kBc1: return 0x31;
    case kBc2: return 0x32;
    case kBc3: return 0x33;
    case kArgb8: return 0x1A;
    default: return 0;
    }
}

// "NTP3", version, texture count; then per texture its header (sizes,
// level count, format, dimensions) and data.
std::optional<std::vector<Texture>> parse(const std::vector<uint8_t>& file) {
    if (file.size() < 16 || std::memcmp(file.data(), "NTP3", 4) != 0) return std::nullopt;
    const uint32_t count = be16(&file[6]);
    std::vector<Texture> textures;
    size_t pos = 16;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 0x18 > file.size()) return std::nullopt;
        const uint32_t total = be32(&file[pos]), data_size = be32(&file[pos + 8]), header_size = be16(&file[pos + 12]);
        if (total < header_size || pos + header_size + data_size > file.size()) return std::nullopt;
        Texture t;
        t.levels = file[pos + 0x11];
        t.format = file[pos + 0x13];
        t.width = be16(&file[pos + 0x14]);
        t.height = be16(&file[pos + 0x16]);
        t.data.assign(file.begin() + static_cast<ptrdiff_t>(pos + header_size),
                      file.begin() + static_cast<ptrdiff_t>(pos + header_size + data_size));
        if (t.level0_size() > t.data.size()) return std::nullopt;
        textures.push_back(std::move(t));
        pos += total;
    }
    return textures;
}

std::optional<Rgba> decode(const Texture& t) {
    switch (t.format) {
    case kBc1: return decode_bc(1, t.data.data(), t.width, t.height);
    case kBc2: return decode_bc(2, t.data.data(), t.width, t.height);
    case kBc3: return decode_bc(3, t.data.data(), t.width, t.height);
    case kArgb8: {
        Rgba out = blank(t.width, t.height);
        for (size_t i = 0; i < size_t{t.width} * t.height; ++i) {
            out.pixels[4 * i + 0] = t.data[4 * i + 1];
            out.pixels[4 * i + 1] = t.data[4 * i + 2];
            out.pixels[4 * i + 2] = t.data[4 * i + 3];
            out.pixels[4 * i + 3] = t.data[4 * i + 0];
        }
        return out;
    }
    default: return std::nullopt;
    }
}

} // namespace ntp3
} // namespace cafe::title
