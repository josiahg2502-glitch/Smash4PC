#pragma once

// The title's textures ("NTP3": the PlayStation 3 layout, which the Wii U
// release keeps) and plain RGBA images to rework them.

#include <cstdint>
#include <optional>
#include <vector>

namespace cafe::title {

struct Rgba {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels; // 4 bytes per pixel, rows top first

    uint8_t* at(uint32_t x, uint32_t y) { return &pixels[(size_t{y} * width + x) * 4]; }
    const uint8_t* at(uint32_t x, uint32_t y) const { return &pixels[(size_t{y} * width + x) * 4]; }
};

Rgba blank(uint32_t width, uint32_t height);
Rgba crop(const Rgba& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
// Filtered to `width` x `height` (each output pixel the area average of
// the input it covers).
Rgba resize(const Rgba& image, uint32_t width, uint32_t height);
// `image` over `into` at (x, y), replacing what is there.
void put(Rgba& into, const Rgba& image, uint32_t x, uint32_t y);

namespace ntp3 {

// Texture formats of the container.
enum Format : uint8_t { kBc1 = 0, kBc2 = 1, kBc3 = 2, kArgb8 = 14 };

struct Texture {
    uint32_t width = 0, height = 0;
    uint8_t format = 0, levels = 0;
    std::vector<uint8_t> data; // level 0 first

    // The bytes of level 0, as the title's GX2 texture holds them once
    // loaded (block-compressed formats keep their blocks).
    size_t level0_size() const;
    // The GX2 surface format the title loads it as (low six bits).
    uint32_t gx2_format() const;
};

std::optional<std::vector<Texture>> parse(const std::vector<uint8_t>& file);

// Level 0 decoded. None for formats this does not read.
std::optional<Rgba> decode(const Texture& texture);

} // namespace ntp3

// Block-compressed (BC1 to BC3) pixel data, `width` x `height`, to RGBA.
Rgba decode_bc(int bc, const uint8_t* data, uint32_t width, uint32_t height);

} // namespace cafe::title
