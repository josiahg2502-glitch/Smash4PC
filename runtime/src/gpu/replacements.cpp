#include "gpu/replacements.h"

#include <atomic>
#include <cstring>

namespace cafe::gpu {
namespace {

std::vector<TextureReplacement> g_replacements;
std::atomic<uint64_t> g_generation{1};
PlaneFilter g_plane_filter;

} // namespace

void add_texture_replacement(TextureReplacement replacement) { g_replacements.push_back(std::move(replacement)); }
const std::vector<TextureReplacement>& texture_replacements() { return g_replacements; }
void texture_replacements_changed() { g_generation.fetch_add(1, std::memory_order_release); }
uint64_t texture_replacements_generation() { return g_generation.load(std::memory_order_acquire); }

void set_plane_filter(PlaneFilter filter) { g_plane_filter = std::move(filter); }
const PlaneFilter& plane_filter() { return g_plane_filter; }

namespace {

// A BC1 colour block's four colours (`opaque`: always four, as BC2 and BC3 use it).
void bc_colors(const uint8_t* b, bool opaque, uint8_t out[4][4]) {
    const uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    const auto rgb = [](uint16_t c, uint8_t* o) {
        o[0] = static_cast<uint8_t>((c >> 11) * 255 / 31);
        o[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        o[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
        o[3] = 255;
    };
    rgb(c0, out[0]);
    rgb(c1, out[1]);
    for (int k = 0; k < 3; ++k) {
        if (opaque || c0 > c1) {
            out[2][k] = static_cast<uint8_t>((2 * out[0][k] + out[1][k]) / 3);
            out[3][k] = static_cast<uint8_t>((out[0][k] + 2 * out[1][k]) / 3);
        } else {
            out[2][k] = static_cast<uint8_t>((out[0][k] + out[1][k]) / 2);
            out[3][k] = 0;
        }
    }
    out[2][3] = 255;
    out[3][3] = opaque || c0 > c1 ? 255 : 0;
}

} // namespace

bool decode_texels(uint32_t format, const uint8_t* data, uint32_t width, uint32_t height, uint8_t* rgba) {
    format &= 0x3F;
    if (format == 0x1A) { // RGBA8
        std::memcpy(rgba, data, size_t{width} * height * 4);
        return true;
    }
    if (format < 0x31 || format > 0x33) return false;
    const uint32_t block_bytes = format == 0x31 ? 8 : 16;
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = data + (size_t{by} * bw + bx) * block_bytes;
            const uint8_t* color = format == 0x31 ? b : b + 8;
            uint8_t colors[4][4];
            bc_colors(color, format != 0x31, colors);
            uint8_t alphas[8] = {};
            if (format == 0x33) {
                alphas[0] = b[0];
                alphas[1] = b[1];
                for (int i = 0; i < 6; ++i) {
                    alphas[i + 2] = b[0] > b[1] ? static_cast<uint8_t>(((6 - i) * b[0] + (i + 1) * b[1]) / 7)
                                    : i < 4    ? static_cast<uint8_t>(((4 - i) * b[0] + (i + 1) * b[1]) / 5)
                                    : i == 4   ? 0
                                               : 255;
                }
            }
            uint64_t alpha_bits = 0;
            for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t{b[2 + i]} << (8 * i);
            const uint32_t indices = color[4] | color[5] << 8 | color[6] << 16 | uint32_t{color[7]} << 24;
            for (uint32_t py = 0; py < 4; ++py) {
                for (uint32_t px = 0; px < 4; ++px) {
                    const uint32_t x = bx * 4 + px, y = by * 4 + py, i = py * 4 + px;
                    if (x >= width || y >= height) continue;
                    uint8_t* o = rgba + (size_t{y} * width + x) * 4;
                    std::memcpy(o, colors[(indices >> (2 * i)) & 3], 4);
                    if (format == 0x32) o[3] = static_cast<uint8_t>(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                    if (format == 0x33) o[3] = alphas[(alpha_bits >> (3 * i)) & 7];
                }
            }
        }
    }
    return true;
}

} // namespace cafe::gpu
