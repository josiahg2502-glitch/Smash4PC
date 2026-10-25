// GX2 surfaces: layout (size, alignment, pitch, mip offsets, tile mode) as
// the console's GX2 computes it with AMD's address library, the color and
// depth buffer registers derived from it, and surface copies.
//
// Titles allocate memory from these numbers and load pre-tiled texture data
// laid out by the same rules, so they must match the console exactly.

#include "gx2/internal.h"

#include "gpu/gpu.h"
#include "gpu/tiling.h"

#include "cafe/export.h"
#include "cafe/layout.h"
#include "cafe/runtime.h"

#include <addrlib/addrinterface.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace cafe::gx2 {
namespace {

using namespace latte;

std::mutex g_addrlib_mutex;

// The Wii U GPU as addrlib knows it: an R7xx-family part, 4 pipes, 4 banks.
ADDR_HANDLE addrlib() {
    static ADDR_HANDLE handle = [] {
        ADDR_CREATE_INPUT in{};
        ADDR_CREATE_OUTPUT out{};
        in.size = sizeof(in);
        out.size = sizeof(out);
        in.chipEngine = CIASICIDGFXENGINE_R600;
        in.chipFamily = 0x51;
        in.chipRevision = 71;
        in.createFlags.fillSizeFields = 1;
        in.regValue.gbAddrConfig = 0x44902;
        in.callbacks.allocSysMem = [](const ADDR_ALLOCSYSMEM_INPUT* request) -> void* {
            return std::malloc(request->sizeInBytes);
        };
        in.callbacks.freeSysMem = [](const ADDR_FREESYSMEM_INPUT* request) -> ADDR_E_RETURNCODE {
            std::free(request->pVirtAddr);
            return ADDR_OK;
        };
        if (AddrCreate(&in, &out) != ADDR_OK) fatal("GX2: could not create the surface address library");
        return out.hLib;
    }();
    return handle;
}

uint32_t level_count_for(uint32_t size) { return size == 0 ? 0 : 32 - std::countl_zero(size); }

uint32_t max_levels(const Surface& s) {
    if (s.mip_levels <= 1) return 1;
    uint32_t levels = std::max(level_count_for(s.width), level_count_for(s.height));
    if (s.dim == kDim3D) levels = std::max(levels, level_count_for(s.depth));
    return levels;
}

} // namespace

void* address_library() { return addrlib(); }

SurfaceInfo surface_info(const Surface& s, uint32_t level) {
    const uint32_t format = s.format;
    const uint32_t hw = format & 0x3F;
    const uint32_t dim = s.dim;
    uint32_t width = std::max(1u, s.width >> level);
    uint32_t height = 1;
    uint32_t slices = 1;
    switch (dim) {
    case kDim1D: break;
    case kDim2D: case kDim2DMsaa: height = std::max(1u, s.height >> level); break;
    case kDim3D:
        height = std::max(1u, s.height >> level);
        slices = std::max(1u, s.depth >> level);
        break;
    case kDimCube:
        height = std::max(1u, s.height >> level);
        slices = std::max(6u, uint32_t{s.depth});
        break;
    case kDim1DArray: slices = s.depth; break;
    case kDim2DArray: case kDim2DMsaaArray:
        height = std::max(1u, s.height >> level);
        slices = s.depth;
        break;
    default: break;
    }

    SurfaceInfo info{};
    const uint32_t samples = 1u << s.aa;
    if (s.tile_mode == kTileLinearSpecial) {
        // Unaligned linear, handled by GX2 itself rather than addrlib.
        const uint32_t block = fmt::is_compressed(hw) ? 4 : 1;
        width = (width + block - 1) / block * block;
        height = (height + block - 1) / block * block;
        info.bpp = surface_format_bits_per_element(format);
        info.pitch = std::max(1u, width / block);
        info.height = std::max(1u, height / block);
        info.depth = slices;
        info.size = uint64_t{info.height} * info.pitch * samples * slices * (info.bpp / 8);
        info.base_align = info.pitch_align = info.height_align = 1;
        info.tile_mode = kTileLinearSpecial;
        return info;
    }

    ADDR_COMPUTE_SURFACE_INFO_INPUT in{};
    ADDR_COMPUTE_SURFACE_INFO_OUTPUT out{};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.tileMode = static_cast<AddrTileMode>(s.tile_mode & 0xF);
    in.format = static_cast<AddrFormat>(hw);
    in.bpp = surface_format_bits_per_element(format);
    in.width = width;
    in.height = height;
    in.numSlices = slices;
    in.numSamples = samples;
    in.numFrags = samples;
    in.mipLevel = level;
    in.flags.cube = dim == kDimCube;
    in.flags.volume = dim == kDim3D;
    in.flags.depth = (s.use & surface_use::kDepthBuffer) != 0;
    in.flags.display = (s.use & surface_use::kScanBuffer) != 0;
    in.flags.inputBaseMap = level == 0;
    {
        std::lock_guard lock(g_addrlib_mutex);
        if (AddrComputeSurfaceInfo(addrlib(), &in, &out) != ADDR_OK) {
            fatal("GX2: addrlib rejected a %ux%ux%u surface (format 0x%X, tile mode %u, level %u)", width, height,
                  slices, format, uint32_t{s.tile_mode}, level);
        }
    }
    info.pitch = out.pitch;
    info.height = out.height;
    info.depth = out.depth;
    info.size = out.surfSize;
    info.base_align = out.baseAlign;
    info.pitch_align = out.pitchAlign;
    info.height_align = out.heightAlign;
    info.tile_mode = out.tileMode;
    info.bpp = out.bpp;
    return info;
}

// GX2CalcSurfaceSizeAndAlignment. A "default" tile mode means 2D tiling
// (thick for a 3D surface that is not a colour buffer), or linear for a
// plain 1D surface; addrlib may then settle on another mode for the base
// level, and a surface smaller than one macro tile in both directions is
// micro tiled instead. The base level is one block; the mip chain another,
// each level after the previous one's end rounded up to its own alignment.
// Swizzle bits 16-23 record the first level that is micro tiled (0xD while
// every level is macro tiled), and from there on each level after the
// chain's first is also preceded by the swizzle's low 16 bits of padding.
namespace {

uint32_t default_tile_mode(const Surface& s) {
    const bool depth = (s.use & surface_use::kDepthBuffer) != 0;
    const bool color = (s.use & surface_use::kColorBuffer) != 0;
    if (s.dim == kDim1D && s.aa == 0 && !depth) return kTileLinearAligned;
    return s.dim == kDim3D && !color ? kTile2DThick : kTile2DThin1;
}

void set_micro_tiled_level(Surface& s, uint32_t level) { s.swizzle = (s.swizzle & 0xFF00FFFFu) | (level << 16); }

uint32_t padding_to(uint32_t offset, uint32_t alignment) { return (alignment - offset % alignment) % alignment; }

} // namespace

void calc_surface_size_and_alignment(Surface& s) {
    const bool bad_align = s.tile_mode == kTileDefaultBadAlign;
    // (Only a default that came out tiled is adjusted to the base level.)
    bool tiled_by_default = false;
    if (s.tile_mode == kTileDefault || bad_align) {
        s.tile_mode = default_tile_mode(s);
        tiled_by_default = s.tile_mode != kTileLinearAligned;
    }
    s.mip_levels = std::min(std::max<uint32_t>(s.mip_levels, 1), max_levels(s));
    set_micro_tiled_level(s, is_macro_tiled(s.tile_mode) ? 0xD : 0);

    SurfaceInfo base = surface_info(s, 0);
    if (tiled_by_default) {
        if (base.tile_mode != s.tile_mode) {
            s.tile_mode = base.tile_mode;
            base = surface_info(s, 0);
            if (!is_macro_tiled(s.tile_mode)) set_micro_tiled_level(s, 0);
        }
        // Titles built with a GX2 alignment bug compare a compressed
        // surface's size against alignments four times too large.
        const uint32_t scale = bad_align && fmt::is_compressed(s.format & 0x3F) ? 4 : 1;
        if (s.width < base.pitch_align * scale && s.height < base.height_align * scale) {
            s.tile_mode = s.tile_mode == kTile2DThick ? kTile1DThick : kTile1DThin1;
            base = surface_info(s, 0);
            set_micro_tiled_level(s, 0);
        }
    }
    s.image_size = static_cast<uint32_t>(base.size);
    s.alignment = base.base_align;
    s.pitch = base.pitch;

    // mip_level_offset[0] is level 1's offset from the base level; the
    // others are the next levels' offsets from level 1.
    uint32_t chain_tiling = s.tile_mode;
    uint32_t previous_size = s.image_size;
    uint32_t level1_offset = 0;
    uint32_t offset = 0;
    for (uint32_t level = 1; level < s.mip_levels; ++level) {
        const SurfaceInfo info = surface_info(s, level);
        uint32_t step = previous_size + padding_to(previous_size, info.base_align);
        if (is_macro_tiled(chain_tiling) && !is_macro_tiled(info.tile_mode)) {
            set_micro_tiled_level(s, level);
            chain_tiling = info.tile_mode;
            if (level > 1) step += s.swizzle & 0xFFFF;
        }
        if (level == 1) {
            level1_offset = step;
        } else {
            offset += step;
            s.mip_level_offset[level - 1] = offset;
        }
        previous_size = static_cast<uint32_t>(info.size);
    }
    s.mipmap_size = s.mip_levels > 1 ? offset + previous_size : 0;
    s.mip_level_offset[0] = level1_offset;
    if (s.format == surface_format::kUnormNv12) {
        // Luma, then the half-size chroma plane at the next aligned offset.
        s.mip_level_offset[0] = s.image_size + padding_to(s.image_size, s.alignment);
        s.image_size = s.mip_level_offset[0] + (s.image_size >> 1);
    }
}

namespace {

// Address of the level a color/depth buffer view renders to, with the
// bank/pipe swizzle folded in for macro-tiled levels.
uint32_t view_address(const Surface& s, uint32_t view_mip) {
    uint32_t address = s.image;
    if (view_mip != 0) {
        address = s.mipmaps;
        if (view_mip > 1) address += s.mip_level_offset[view_mip - 1];
    }
    if (is_macro_tiled(s.tile_mode) && view_mip < ((s.swizzle >> 16) & 0xFF)) address ^= s.swizzle & 0xFFFF;
    return address;
}

uint32_t size_register(const SurfaceInfo& info) {
    return field(info.pitch / 8 - 1, 0, 10) | field(info.pitch * info.height / 64 - 1, 10, 20);
}

// The MSAA auxiliary buffer (FMASK then CMASK) is private to the GPU: the
// title only allocates it. addrlib's R600 variant has no FMASK/CMASK
// calculation, so this sizes it generously from the per-sample FMASK bits
// and the 4 bits per 8x8 tile of CMASK.
void aux_info(const ColorBuffer& buffer, uint32_t& size, uint32_t& alignment, uint32_t& cmask_offset,
              uint32_t& color_mask) {
    const SurfaceInfo info = surface_info(buffer.surface, buffer.view_mip);
    const uint32_t samples = 1u << buffer.surface.aa;
    const uint32_t fmask_bits = samples <= 2 ? 4 : samples == 4 ? 8 : 32;
    const uint32_t pixels = info.pitch * info.height * std::max(1u, info.depth);
    const uint32_t fmask = (pixels * fmask_bits / 8 + 0x7FF) & ~0x7FFu;
    const uint32_t tiles = pixels / 64;
    const uint32_t cmask = ((tiles + 1) / 2 + 0x7FF) & ~0x7FFu;
    alignment = 0x800;
    cmask_offset = fmask;
    size = fmask + cmask;
    color_mask = field(std::max(1u, tiles / 128) - 1, 0, 12) | field(std::max(1u, tiles) - 1, 12, 20);
}

} // namespace

void init_color_buffer_regs(ColorBuffer& buffer) {
    const Surface& s = buffer.surface;
    const uint32_t format = s.format;
    const uint32_t hw = format & 0x3F;
    const uint32_t type = surface_format::type(format);
    const SurfaceInfo info = surface_info(s, buffer.view_mip);
    buffer.cb_color_size = size_register(info);

    const bool integer = type == surface_format::kTypeUint || type == surface_format::kTypeSint;
    const bool no_blend = integer || format == surface_format::kUnormR24X8 || format == surface_format::kFloatD24S8 ||
                          format == surface_format::kFloatX8X24;
    const bool clamp = !no_blend && (type == surface_format::kTypeUnorm || type == surface_format::kTypeSnorm ||
                                     type == surface_format::kTypeSrgb);
    const uint32_t comp_swap = (hw == fmt::k5_5_5_1 || hw == fmt::k10_10_10_2) ? 2 : 0; // STD_REV
    buffer.cb_color_info = field(color_buffer_format(format), 2, 6) | field(info.tile_mode, 8, 4) |
                           field(color_buffer_number_type(format), 12, 3) | field(comp_swap, 16, 2) |
                           field(s.aa != 0 ? 2 : 0, 18, 2) | // TILE_MODE FRAG_ENABLE
                           field(clamp, 20, 1) | field(no_blend, 22, 1) |
                           field(type == surface_format::kTypeFloat, 25, 1) | // ROUND_MODE TRUNCATE
                           field(integer ? 0 : color_buffer_source_format(format), 27, 1);
    buffer.cb_color_view = s.tile_mode == kTileLinearSpecial
                               ? 0u
                               : field(buffer.view_first_slice, 0, 11) |
                                     field(buffer.view_first_slice + buffer.view_num_slices - 1, 13, 11);
    buffer.cb_color_mask = 0;
    if (s.aa != 0) {
        uint32_t size, alignment, cmask_offset, color_mask;
        aux_info(buffer, size, alignment, cmask_offset, color_mask);
        buffer.cmask_offset = cmask_offset;
        buffer.cb_color_mask = color_mask;
    }
}

namespace {

void GX2CalcSurfaceSizeAndAlignment(Surface* surface) { calc_surface_size_and_alignment(*surface); }

uint32_t GX2GetSurfaceMipPitch(Surface* surface, uint32_t level) { return surface_info(*surface, level).pitch; }

uint32_t GX2GetSurfaceMipSliceSize(Surface* surface, uint32_t level) {
    const SurfaceInfo info = surface_info(*surface, level);
    return info.pitch * info.height * (1u << surface->aa) * (info.bpp / 8);
}

uint32_t GX2GetSurfaceSwizzle(Surface* surface) { return (surface->swizzle >> 8) & 0xFF; }

uint32_t GX2GetSurfaceSwizzleOffset(Surface* surface, uint32_t level) {
    if (!is_macro_tiled(surface->tile_mode) || level < ((surface->swizzle >> 16) & 0xFF)) return 0;
    return surface->swizzle & 0xFFFF;
}

void GX2SetSurfaceSwizzle(Surface* surface, uint32_t swizzle) {
    surface->swizzle = (surface->swizzle & 0xFFFF00FF) | (swizzle << 8);
}

void GX2InitColorBufferRegs(ColorBuffer* buffer) { init_color_buffer_regs(*buffer); }

void GX2CalcColorBufferAuxInfo(ColorBuffer* buffer, be<uint32_t>* out_size, be<uint32_t>* out_alignment) {
    uint32_t size, alignment, cmask_offset, color_mask;
    aux_info(*buffer, size, alignment, cmask_offset, color_mask);
    buffer->aa_size = size;
    if (out_size) *out_size = size;
    if (out_alignment) *out_alignment = alignment;
}

void GX2InitDepthBufferRegs(DepthBuffer* buffer) {
    const Surface& s = buffer->surface;
    const SurfaceInfo info = surface_info(s, buffer->view_mip);
    buffer->db_depth_size = size_register(info);
    buffer->db_depth_view = field(buffer->view_first_slice, 0, 11) |
                            field(buffer->view_first_slice + buffer->view_num_slices - 1, 13, 11);
    buffer->db_htile_surface = field(1, 0, 1) | field(1, 1, 1) | field(1, 3, 1); // 8x8 HTILE, full cache
    buffer->db_prefetch_limit = field((s.height / 8 - 1) & 0x3FF, 0, 10);
    buffer->db_preload_control = field(s.width / 32, 16, 8) | field(s.height / 32, 24, 8);

    uint32_t db_format = 0;     // DB_FORMAT
    uint32_t offset_bits = 0;   // POLY_OFFSET_NEG_NUM_DB_BITS
    bool float_depth = false;
    switch (uint32_t{s.format}) {
    case surface_format::kUnormR16: db_format = 1; offset_bits = 240; break;   // DEPTH_16
    case surface_format::kUnormR24X8: db_format = 3; offset_bits = 232; break; // DEPTH_8_24
    case surface_format::kFloatR32: db_format = 6; offset_bits = 233; float_depth = true; break;
    case surface_format::kFloatD24S8: db_format = 5; offset_bits = 236; float_depth = true; break;
    case surface_format::kFloatX8X24: db_format = 7; offset_bits = 233; float_depth = true; break;
    default: break;
    }
    buffer->db_depth_info = field(db_format, 0, 3) | field(1, 3, 1) /* READ_512_BITS */ |
                            field(info.tile_mode, 15, 4) | field(buffer->hiz_ptr != 0u, 25, 1);
    buffer->pa_poly_offset_cntl = field(offset_bits, 0, 8) | field(float_depth, 8, 1);
}

void GX2InitDepthBufferHiZEnable(DepthBuffer* buffer, bool enable) {
    buffer->db_depth_info = (buffer->db_depth_info & ~(1u << 25)) | field(enable, 25, 1);
}

void GX2CalcDepthBufferHiZInfo(DepthBuffer* buffer, be<uint32_t>* out_size, be<uint32_t>* out_alignment) {
    ADDR_COMPUTE_HTILE_INFO_INPUT in{};
    ADDR_COMPUTE_HTILE_INFO_OUTPUT out{};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.pitch = buffer->surface.pitch;
    in.height = buffer->surface.height;
    in.numSlices = buffer->surface.depth;
    in.blockWidth = ADDR_HTILE_BLOCKSIZE_8;
    in.blockHeight = ADDR_HTILE_BLOCKSIZE_8;
    {
        std::lock_guard lock(g_addrlib_mutex);
        AddrComputeHtileInfo(addrlib(), &in, &out);
    }
    buffer->hiz_size = static_cast<uint32_t>(out.htileBytes);
    if (out_size) *out_size = static_cast<uint32_t>(out.htileBytes);
    if (out_alignment) *out_alignment = out.baseAlign;
}

void GX2SetColorBuffer(ColorBuffer* buffer, uint32_t target) {
    ApiLock lock;
    const uint32_t address = view_address(buffer->surface, buffer->view_mip);
    uint32_t tile = 0, frag = 0;
    if (buffer->surface.aa != 0) {
        frag = buffer->aa_buffer;
        tile = frag + buffer->cmask_offset;
    }
    const uint32_t t = target * 4;
    set_context_reg(reg::CB_COLOR0_BASE + t, address >> 8);
    set_context_reg(reg::CB_COLOR0_SIZE + t, buffer->cb_color_size);
    set_context_reg(reg::CB_COLOR0_INFO + t, buffer->cb_color_info);
    set_context_reg(reg::CB_COLOR0_TILE + t, tile >> 8);
    set_context_reg(reg::CB_COLOR0_FRAG + t, frag >> 8);
    set_context_reg(reg::CB_COLOR0_VIEW + t, buffer->cb_color_view);
    set_context_reg(reg::CB_COLOR0_MASK + t, buffer->cb_color_mask);
}

void GX2SetDepthBuffer(DepthBuffer* buffer) {
    ApiLock lock;
    const uint32_t size_view[] = {buffer->db_depth_size, buffer->db_depth_view};
    set_context_regs(reg::DB_DEPTH_SIZE, size_view);
    const uint32_t base_info_htile[] = {view_address(buffer->surface, buffer->view_mip) >> 8,
                                        buffer->db_depth_info, uint32_t{buffer->hiz_ptr} >> 8};
    set_context_regs(reg::DB_DEPTH_BASE, base_info_htile);
    set_context_reg(reg::DB_HTILE_SURFACE, buffer->db_htile_surface);
    set_context_reg(reg::DB_PREFETCH_LIMIT, buffer->db_prefetch_limit);
    set_context_reg(reg::DB_PRELOAD_CONTROL, buffer->db_preload_control);
    set_context_reg(reg::PA_SU_POLY_OFFSET_DB_FMT_CNTL, buffer->pa_poly_offset_cntl);
    const uint32_t clear[] = {buffer->stencil_clear, std::bit_cast<uint32_t>(float{buffer->depth_clear})};
    set_context_regs(reg::DB_STENCIL_CLEAR, clear);
}

void GX2CopySurface(Surface* src, uint32_t src_level, uint32_t src_slice, Surface* dst, uint32_t dst_level,
                    uint32_t dst_slice) {
    if (src->format == 0u || src->width == 0u || src->height == 0u || dst->format == 0u) return;
    ApiLock lock;
    // With an unaligned linear surface on either side the console's GX2
    // copies on the CPU, at once: titles reuse the source straight away.
    if (src->tile_mode == uint32_t{kTileLinearSpecial} || dst->tile_mode == uint32_t{kTileLinearSpecial}) {
        uint32_t address = 0, size = 0;
        if (gpu::copy_surface_memory(*src, src_level, src_slice, *dst, dst_level, dst_slice, address, size)) {
            gpu::cpu_wrote(address, size);
        } else {
            std::fprintf(stderr, "ttt2: GX2CopySurface: format 0x%X -> 0x%X not supported\n", uint32_t{src->format},
                         uint32_t{dst->format});
        }
        return;
    }
    HlePacket(pm4::kHleCopySurface)
        .guest_struct(*src).u32(src_level).u32(src_slice)
        .guest_struct(*dst).u32(dst_level).u32(dst_slice)
        .write();
}

void GX2ResolveAAColorBuffer(ColorBuffer* src, Surface* dst, uint32_t dst_level, uint32_t dst_slice) {
    if (src->surface.format == 0u || src->surface.width == 0u || dst->format == 0u) return;
    ApiLock lock;
    HlePacket(pm4::kHleResolveColor).guest_struct(*src).guest_struct(*dst).u32(dst_level).u32(dst_slice).write();
}

void GX2ExpandDepthBuffer(DepthBuffer* buffer) {
    ApiLock lock;
    HlePacket(pm4::kHleExpandDepth).guest_struct(*buffer).write();
}

void GX2ConvertDepthBufferToTextureSurface(DepthBuffer* src, Surface* dst, uint32_t dst_level, uint32_t dst_slice) {
    ApiLock lock;
    HlePacket(pm4::kHleConvertDepth).guest_struct(*src).guest_struct(*dst).u32(dst_level).u32(dst_slice).write();
}

} // namespace

// ---------------------------------------------------------- tiling apertures
// A linear view of one slice of a surface level for the CPU: filled detiled
// when allocated, tiled back into the surface when freed. `endian` swaps the
// bytes of each 16/32/64-bit unit (GX2EndianSwapMode 1/2/3), as the aperture
// hardware does for big-endian CPU access.
namespace {

struct Aperture {
    Surface surface;
    uint32_t level = 0, slice = 0, endian = 0, address = 0, bytes = 0;
    bool used = false;
};
std::mutex g_aperture_mutex;
Aperture g_apertures[layout::kTilingApertureSlots];

void swap_units(uint8_t* data, uint32_t bytes, uint32_t endian) {
    const uint32_t unit = endian == 1 ? 2 : endian == 2 ? 4 : endian == 3 ? 8 : 1;
    for (uint32_t i = 0; unit > 1 && i + unit <= bytes; i += unit) std::reverse(data + i, data + i + unit);
}

void GX2AllocateTilingApertureEx(Surface* surface, uint32_t level, uint32_t slice, uint32_t endian,
                                 be<uint32_t>* handle, be<uint32_t>* address) {
    std::lock_guard lock(g_aperture_mutex);
    uint32_t width, height, level_bytes;
    const gpu::TiledLevel tiled = gpu::memory_level(*surface, level, width, height, level_bytes);
    const uint32_t bytes = width * height * (tiled.bpp / 8);
    for (uint32_t i = 0; i < layout::kTilingApertureSlots; ++i) {
        Aperture& a = g_apertures[i];
        if (a.used) continue;
        if (bytes > layout::kTilingApertureSlot) fatal("tiling aperture of 0x%X bytes is larger than a slot", bytes);
        a = {*surface, level, slice, endian, layout::kTilingApertureBase + i * layout::kTilingApertureSlot, bytes, true};
        commit_guest_memory(a.address, bytes);
        gpu::detile(tiled, width, height, slice, 1, guest_pointer(a.address));
        swap_units(guest_pointer(a.address), bytes, endian);
        *handle = i + 1;
        *address = a.address;
        return;
    }
    fatal("more than %u tiling apertures are open", layout::kTilingApertureSlots);
}

void GX2FreeTilingAperture(uint32_t handle) {
    std::lock_guard lock(g_aperture_mutex);
    if (handle == 0 || handle > layout::kTilingApertureSlots || !g_apertures[handle - 1].used) return;
    Aperture& a = g_apertures[handle - 1];
    uint32_t width, height, level_bytes;
    const gpu::TiledLevel tiled = gpu::memory_level(a.surface, a.level, width, height, level_bytes);
    swap_units(guest_pointer(a.address), a.bytes, a.endian);
    gpu::tile(tiled, width, height, a.slice, 1, guest_pointer(a.address));
    a.used = false;
}

// Expanding a multisampled color buffer's compression for sampling.
// shortcut: no-op, the backend keeps color buffers uncompressed; expand if a title samples an MSAA buffer wrongly.
void GX2ExpandAAColorBuffer(ColorBuffer*) {}

} // namespace

CAFE_EXPORT(gx2, GX2AllocateTilingApertureEx, GX2AllocateTilingApertureEx);
CAFE_EXPORT(gx2, GX2FreeTilingAperture, GX2FreeTilingAperture);
CAFE_EXPORT(gx2, GX2ExpandAAColorBuffer, GX2ExpandAAColorBuffer);
CAFE_EXPORT(gx2, GX2CalcSurfaceSizeAndAlignment, GX2CalcSurfaceSizeAndAlignment);
CAFE_EXPORT(gx2, GX2GetSurfaceMipPitch, GX2GetSurfaceMipPitch);
CAFE_EXPORT(gx2, GX2GetSurfaceMipSliceSize, GX2GetSurfaceMipSliceSize);
CAFE_EXPORT(gx2, GX2GetSurfaceSwizzle, GX2GetSurfaceSwizzle);
CAFE_EXPORT(gx2, GX2GetSurfaceSwizzleOffset, GX2GetSurfaceSwizzleOffset);
CAFE_EXPORT(gx2, GX2SetSurfaceSwizzle, GX2SetSurfaceSwizzle);
CAFE_EXPORT(gx2, GX2InitColorBufferRegs, GX2InitColorBufferRegs);
CAFE_EXPORT(gx2, GX2CalcColorBufferAuxInfo, GX2CalcColorBufferAuxInfo);
CAFE_EXPORT(gx2, GX2InitDepthBufferRegs, GX2InitDepthBufferRegs);
CAFE_EXPORT(gx2, GX2InitDepthBufferHiZEnable, GX2InitDepthBufferHiZEnable);
CAFE_EXPORT(gx2, GX2CalcDepthBufferHiZInfo, GX2CalcDepthBufferHiZInfo);
CAFE_EXPORT(gx2, GX2SetColorBuffer, GX2SetColorBuffer);
CAFE_EXPORT(gx2, GX2SetDepthBuffer, GX2SetDepthBuffer);
CAFE_EXPORT(gx2, GX2CopySurface, GX2CopySurface);
CAFE_EXPORT(gx2, GX2ResolveAAColorBuffer, GX2ResolveAAColorBuffer);
CAFE_EXPORT(gx2, GX2ExpandDepthBuffer, GX2ExpandDepthBuffer);
CAFE_EXPORT(gx2, GX2ConvertDepthBufferToTextureSurface, GX2ConvertDepthBufferToTextureSurface);

} // namespace cafe::gx2
