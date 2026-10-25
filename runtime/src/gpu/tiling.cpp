#include "gpu/tiling.h"

#include "gpu/latte.h"
#include "gx2/internal.h"

#include "cafe/runtime.h"

#include <addrlib/addrinterface.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cafe::gpu {

namespace {

struct Addressing {
    void* lib;
    ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in{};
    ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out{};

    explicit Addressing(const TiledLevel& level) : lib(gx2::address_library()) {
        ADDR_EXTRACT_BANKPIPE_SWIZZLE_INPUT sin{};
        ADDR_EXTRACT_BANKPIPE_SWIZZLE_OUTPUT sout{};
        sin.size = sizeof(sin);
        sout.size = sizeof(sout);
        sin.base256b = level.base256b;
        AddrExtractBankPipeSwizzle(lib, &sin, &sout);
        in.size = sizeof(in);
        out.size = sizeof(out);
        in.bpp = level.bpp;
        in.pitch = level.pitch;
        in.height = level.height;
        in.numSlices = level.slices;
        in.numSamples = level.samples;
        in.numFrags = level.samples;
        in.tileMode = static_cast<AddrTileMode>(level.tile_mode);
        in.isDepth = level.depth;
        in.tileType = level.depth ? ADDR_NON_DISPLAYABLE : ADDR_DISPLAYABLE;
        in.pipeSwizzle = sout.pipeSwizzle;
        in.bankSwizzle = sout.bankSwizzle;
    }
    uint64_t operator()(uint32_t x, uint32_t y, uint32_t slice) {
        in.x = x;
        in.y = y;
        in.slice = slice;
        AddrComputeSurfaceAddrFromCoord(lib, &in, &out);
        return out.addr;
    }
};

// Thin single-sample tile modes: every 8x8 micro tile has the same layout
// relative to its first element (within a pipe interleave group, and above
// it across the pipe and bank bits), so one address per micro tile does.
bool uniform_micro_tiles(const TiledLevel& level) {
    switch (level.tile_mode) {
    case ADDR_TM_1D_TILED_THIN1: case ADDR_TM_2D_TILED_THIN1: case ADDR_TM_2D_TILED_THIN2:
    case ADDR_TM_2D_TILED_THIN4: case ADDR_TM_2B_TILED_THIN1: case ADDR_TM_2B_TILED_THIN2:
    case ADDR_TM_2B_TILED_THIN4: case ADDR_TM_3D_TILED_THIN1: case ADDR_TM_3B_TILED_THIN1:
        return level.samples <= 1 && level.pitch % 8 == 0;
    default: return false;
    }
}

// Calls `element(tiled_offset, linear_offset, bytes)` for every element (or
// row of elements for linear surfaces).
template <typename F>
void for_each_element(const TiledLevel& level, uint32_t width, uint32_t height, uint32_t first_slice,
                      uint32_t slice_count, F element, bool fast = true) {
    const uint32_t bytes = level.bpp / 8;
    const size_t row = size_t{width} * bytes;
    // Linear general / aligned, and GX2's unaligned linear (16): rows of `pitch` elements.
    if (level.tile_mode <= 1 || level.tile_mode == 16) {
        const size_t pitch_bytes = size_t{level.pitch} * bytes;
        const size_t slice_bytes = pitch_bytes * level.height;
        for (uint32_t s = 0; s < slice_count; ++s) {
            for (uint32_t y = 0; y < height; ++y) {
                element((first_slice + s) * slice_bytes + y * pitch_bytes, (s * size_t{height} + y) * row, row);
            }
        }
        return;
    }
    Addressing address(level);
    if (uniform_micro_tiles(level) && fast) {
        // The layout of the first micro tile, relative to its first element.
        uint64_t pattern[64];
        const uint64_t origin = address(0, 0, first_slice);
        for (uint32_t i = 0; i < 64; ++i) pattern[i] = address(i % 8, i / 8, first_slice) - origin;
        for (uint32_t s = 0; s < slice_count; ++s) {
            for (uint32_t ty = 0; ty < height; ty += 8) {
                for (uint32_t tx = 0; tx < width; tx += 8) {
                    const uint64_t base = address(tx, ty, first_slice + s);
                    const uint32_t w = std::min(8u, width - tx), h = std::min(8u, height - ty);
                    for (uint32_t y = 0; y < h; ++y) {
                        const size_t linear = (s * size_t{height} + ty + y) * row + size_t{tx} * bytes;
                        for (uint32_t x = 0; x < w; ++x) element(base + pattern[y * 8 + x], linear + size_t{x} * bytes, bytes);
                    }
                }
            }
        }
        return;
    }
    for (uint32_t s = 0; s < slice_count; ++s) {
        for (uint32_t y = 0; y < height; ++y) {
            const size_t linear = (s * size_t{height} + y) * row;
            for (uint32_t x = 0; x < width; ++x) element(address(x, y, first_slice + s), linear + size_t{x} * bytes, bytes);
        }
    }
}

} // namespace

void detile(const TiledLevel& level, uint32_t width, uint32_t height, uint32_t first_slice, uint32_t slice_count,
            uint8_t* out) {
    for_each_element(level, width, height, first_slice, slice_count, [&](size_t tiled, size_t linear, size_t n) {
        std::memcpy(out + linear, level.data + tiled, n);
    });
    // TTT2_CHECK_TILING=1: compare with an address per element.
    static const bool check = std::getenv("TTT2_CHECK_TILING") != nullptr;
    if (check && uniform_micro_tiles(level)) {
        std::vector<uint8_t> slow(size_t{width} * height * slice_count * (level.bpp / 8));
        for_each_element(
            level, width, height, first_slice, slice_count,
            [&](size_t tiled, size_t linear, size_t n) { std::memcpy(slow.data() + linear, level.data + tiled, n); },
            false);
        const bool same = std::memcmp(slow.data(), out, slow.size()) == 0;
        std::fprintf(stderr, "check: detile %ux%u tile %u bpp %u pitch %u: %s\n", width, height, level.tile_mode,
                     level.bpp, level.pitch, same ? "ok" : "MISMATCH");
    }
}

void tile(const TiledLevel& level, uint32_t width, uint32_t height, uint32_t first_slice, uint32_t slice_count,
          const uint8_t* in) {
    for_each_element(level, width, height, first_slice, slice_count, [&](size_t tiled, size_t linear, size_t n) {
        std::memcpy(level.data + tiled, in + linear, n);
    });
}

TiledLevel memory_level(const gx2::Surface& s, uint32_t level, uint32_t& width, uint32_t& height, uint32_t& bytes) {
    const gx2::SurfaceInfo li = gx2::surface_info(s, level);
    const uint32_t address = level == 0 ? uint32_t{s.image} : s.mipmaps + (level > 1 ? uint32_t{s.mip_level_offset[level - 1]} : 0u);
    const bool swizzled = gx2::is_macro_tiled(li.tile_mode) && level < ((s.swizzle >> 16) & 0xFF);
    const uint32_t block = latte::fmt::is_compressed(s.format & 0x3F) ? 4 : 1;
    width = (std::max(1u, s.width >> level) + block - 1) / block;
    height = (std::max(1u, s.height >> level) + block - 1) / block;
    if (s.dim == gx2::kDim1D || s.dim == gx2::kDim1DArray) height = 1;
    TiledLevel t{};
    t.data = guest_pointer(address);
    t.base256b = (address ^ (swizzled ? s.swizzle & 0xFFFF : 0)) >> 8;
    t.tile_mode = li.tile_mode;
    t.bpp = li.bpp;
    t.pitch = li.pitch;
    t.height = li.height;
    t.slices = std::max(li.depth, 1u);
    t.samples = 1;
    t.depth = (s.use & gx2::surface_use::kDepthBuffer) != 0;
    bytes = static_cast<uint32_t>(li.size);
    return t;
}

bool copy_surface_memory(const gx2::Surface& src, uint32_t src_level, uint32_t src_slice, const gx2::Surface& dst,
                         uint32_t dst_level, uint32_t dst_slice, uint32_t& written_address, uint32_t& written_size) {
    uint32_t sw, sh, sbytes, dw, dh, dbytes;
    const TiledLevel a = memory_level(src, src_level, sw, sh, sbytes);
    const TiledLevel b = memory_level(dst, dst_level, dw, dh, dbytes);
    const uint32_t from = static_cast<uint32_t>(a.data - guest_pointer(0));
    const uint32_t to = static_cast<uint32_t>(b.data - guest_pointer(0));
    if (a.bpp != b.bpp || !guest_memory_committed(from, sbytes) || !guest_memory_committed(to, dbytes)) return false;
    const uint32_t w = std::min(sw, dw), h = std::min(sh, dh);
    std::vector<uint8_t> elements(size_t{w} * h * (a.bpp / 8));
    detile(a, w, h, src_slice, 1, elements.data());
    tile(b, w, h, dst_slice, 1, elements.data());
    written_address = to;
    written_size = dbytes;
    return true;
}

} // namespace cafe::gpu
