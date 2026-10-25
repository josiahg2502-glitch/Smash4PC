#pragma once

// Latte surface tiling: the element layouts of linear, micro-tiled and
// macro-tiled surfaces, through AMD's address library.

#include "gx2/types.h"

#include <cstdint>

namespace cafe::gpu {

struct TiledLevel {
    uint8_t* data;        // the level's first byte (host pointer to guest memory)
    uint32_t base256b;    // the level's address register value (address >> 8), swizzle included
    uint32_t tile_mode;   // tile mode of this level (AddrTileMode)
    uint32_t bpp;         // bits per element
    uint32_t pitch;       // in elements
    uint32_t height;      // aligned height in elements
    uint32_t slices;      // slices the level holds
    uint32_t samples;
    bool depth;           // depth buffer tiling
};

// Copies elements [0, width) x [0, height) of slices [first, first + count)
// of `level` to `out`, rows of `width` elements, slice after slice.
void detile(const TiledLevel& level, uint32_t width, uint32_t height, uint32_t first_slice, uint32_t slice_count,
            uint8_t* out);
// The inverse: writes `in` (laid out as detile() produces it) into `level`.
void tile(const TiledLevel& level, uint32_t width, uint32_t height, uint32_t first_slice, uint32_t slice_count,
          const uint8_t* in);

// Level `level` of a GX2 surface in guest memory; `width` and `height` get
// its size in elements and `bytes` the level's size.
TiledLevel memory_level(const gx2::Surface& s, uint32_t level, uint32_t& width, uint32_t& height, uint32_t& bytes);

// GX2CopySurface done by the CPU, retiling; false if the surfaces are not
// compatible (element size) or not in committed memory. `written` gets the
// destination range.
bool copy_surface_memory(const gx2::Surface& src, uint32_t src_level, uint32_t src_slice, const gx2::Surface& dst,
                         uint32_t dst_level, uint32_t dst_slice, uint32_t& written_address, uint32_t& written_size);

} // namespace cafe::gpu
