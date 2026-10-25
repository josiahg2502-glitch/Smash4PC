#pragma once

// GX2 structures as titles lay them out in guest memory, and the GX2 enum
// values the runtime interprets. Layouts are fixed by the SDK the title was
// built with; every offset is checked.

#include "cafe/guest.h"

#include <cstddef>
#include <cstdint>

namespace cafe::gx2 {

// ------------------------------------------------------------------ enums
enum SurfaceDim : uint32_t {
    kDim1D = 0, kDim2D = 1, kDim3D = 2, kDimCube = 3, kDim1DArray = 4, kDim2DArray = 5,
    kDim2DMsaa = 6, kDim2DMsaaArray = 7,
};

enum TileMode : uint32_t {
    kTileDefault = 0, kTileLinearAligned = 1, kTile1DThin1 = 2, kTile1DThick = 3,
    kTile2DThin1 = 4, kTile2DThin2 = 5, kTile2DThin4 = 6, kTile2DThick = 7,
    kTile2BThin1 = 8, kTile2BThin2 = 9, kTile2BThin4 = 10, kTile2BThick = 11,
    kTile3DThin1 = 12, kTile3DThick = 13, kTile3BThin1 = 14, kTile3BThick = 15,
    kTileLinearSpecial = 16, kTileDefaultBadAlign = 0x20,
};
// Macro-tiled modes carry a bank/pipe swizzle in the base address.
constexpr bool is_macro_tiled(uint32_t mode) { return mode >= kTile2DThin1 && mode != kTileLinearSpecial; }

namespace surface_use {
constexpr uint32_t kTexture = 1u << 0, kColorBuffer = 1u << 1, kDepthBuffer = 1u << 2,
                   kScanBuffer = 1u << 3;
}

// GX2SurfaceFormat: hardware data format in bits 0-5, type in bits 8-11.
namespace surface_format {
constexpr uint32_t kTypeUnorm = 0x0, kTypeUint = 0x1, kTypeSnorm = 0x2, kTypeSint = 0x3,
                   kTypeSrgb = 0x4, kTypeFloat = 0x8;
constexpr uint32_t kUnormR16 = 0x005, kUnormR24X8 = 0x011, kUnormNv12 = 0x081,
                   kFloatR32 = 0x80E, kFloatD24S8 = 0x811, kFloatX8X24 = 0x81C;
constexpr uint32_t type(uint32_t format) { return format >> 8; }
constexpr uint32_t hw(uint32_t format) { return format & 0x3F; }
} // namespace surface_format

// GX2AttribFormat: type in bits 0-4, flags above.
namespace attrib_format {
constexpr uint32_t kInteger = 0x100, kSigned = 0x200, kDegamma = 0x400, kScaled = 0x800;
}

enum EndianSwap : uint32_t { kSwapNone = 0, kSwap8In16 = 1, kSwap8In32 = 2, kSwapDefault = 3 };

enum IndexType : uint32_t { kIndexU16Le = 0, kIndexU32Le = 1, kIndexU16 = 4, kIndexU32 = 9 };

namespace invalidate {
constexpr uint32_t kAttributeBuffer = 1u << 0, kTexture = 1u << 1, kUniformBlock = 1u << 2,
                   kShader = 1u << 3, kColorBuffer = 1u << 4, kDepthBuffer = 1u << 5,
                   kCpu = 1u << 6, kStreamOutBuffer = 1u << 7, kExportBuffer = 1u << 8;
}

namespace resource_flags {
constexpr uint32_t kBindTexture = 1u << 0, kBindColorBuffer = 1u << 1, kBindDepthBuffer = 1u << 2,
                   kBindScanBuffer = 1u << 3, kBindVertexBuffer = 1u << 4,
                   kBindIndexBuffer = 1u << 5, kBindUniformBlock = 1u << 6,
                   kBindShaderProgram = 1u << 7, kBindStreamOutput = 1u << 8,
                   kBindDisplayList = 1u << 9, kBindGsRing = 1u << 10, kCpuRead = 1u << 11,
                   kCpuWrite = 1u << 12, kGpuRead = 1u << 13, kGpuWrite = 1u << 14,
                   kDmaRead = 1u << 15, kDmaWrite = 1u << 16, kForceMem1 = 1u << 17,
                   kForceMem2 = 1u << 18, kDisableCpuInvalidate = 1u << 20,
                   kDisableGpuInvalidate = 1u << 21, kLockedReadOnly = 1u << 22,
                   kDestroyNoFree = 1u << 23, kAllocated = 1u << 29, kLocked = 1u << 30;
}

enum ShaderMode : uint32_t { kModeUniformRegister = 0, kModeUniformBlock = 1, kModeGeometry = 2, kModeCompute = 3 };
enum ScanTarget : uint32_t { kScanTv = 1, kScanDrc = 4 };

// --------------------------------------------------------------- surfaces
struct Surface {
    be<uint32_t> dim;
    be<uint32_t> width;
    be<uint32_t> height;
    be<uint32_t> depth;
    be<uint32_t> mip_levels;
    be<uint32_t> format;
    be<uint32_t> aa;
    be<uint32_t> use; // GX2SurfaceUse, or GX2RResourceFlags for GX2R surfaces
    be<uint32_t> image_size;
    be<uint32_t> image;
    be<uint32_t> mipmap_size;
    be<uint32_t> mipmaps;
    be<uint32_t> tile_mode;
    be<uint32_t> swizzle; // bits 8-15 base swizzle, 16-23 first non-macro-tiled level
    be<uint32_t> alignment;
    be<uint32_t> pitch;
    be<uint32_t> mip_level_offset[13];
};
static_assert(sizeof(Surface) == 0x74);
static_assert(offsetof(Surface, image) == 0x24 && offsetof(Surface, tile_mode) == 0x30);
static_assert(offsetof(Surface, mip_level_offset) == 0x40);

struct ColorBuffer {
    Surface surface;
    be<uint32_t> view_mip;
    be<uint32_t> view_first_slice;
    be<uint32_t> view_num_slices;
    be<uint32_t> aa_buffer;
    be<uint32_t> aa_size;
    be<uint32_t> cb_color_size;
    be<uint32_t> cb_color_info;
    be<uint32_t> cb_color_view;
    be<uint32_t> cb_color_mask;
    be<uint32_t> cmask_offset;
};
static_assert(sizeof(ColorBuffer) == 0x9C && offsetof(ColorBuffer, cb_color_size) == 0x88);

struct DepthBuffer {
    Surface surface;
    be<uint32_t> view_mip;
    be<uint32_t> view_first_slice;
    be<uint32_t> view_num_slices;
    be<uint32_t> hiz_ptr;
    be<uint32_t> hiz_size;
    be<float> depth_clear;
    be<uint32_t> stencil_clear;
    be<uint32_t> db_depth_size;
    be<uint32_t> db_depth_view;
    be<uint32_t> db_depth_info;
    be<uint32_t> db_htile_surface;
    be<uint32_t> db_prefetch_limit;
    be<uint32_t> db_preload_control;
    be<uint32_t> pa_poly_offset_cntl;
};
static_assert(sizeof(DepthBuffer) == 0xAC && offsetof(DepthBuffer, db_depth_size) == 0x90);

struct Texture {
    Surface surface;
    be<uint32_t> view_first_mip;
    be<uint32_t> view_num_mips;
    be<uint32_t> view_first_slice;
    be<uint32_t> view_num_slices;
    be<uint32_t> comp_map;
    be<uint32_t> word0;
    be<uint32_t> word1;
    be<uint32_t> word4;
    be<uint32_t> word5;
    be<uint32_t> word6;
};
static_assert(sizeof(Texture) == 0x9C && offsetof(Texture, word0) == 0x88);

struct Sampler {
    be<uint32_t> word0;
    be<uint32_t> word1;
    be<uint32_t> word2;
};
static_assert(sizeof(Sampler) == 0x0C);

// ------------------------------------------------------------------ GX2R
struct RBuffer {
    be<uint32_t> flags;
    be<uint32_t> elem_size;
    be<uint32_t> elem_count;
    be<uint32_t> buffer;
};
static_assert(sizeof(RBuffer) == 0x10);

// --------------------------------------------------------------- shaders
struct FetchShader {
    be<uint32_t> type;
    be<uint32_t> sq_pgm_resources_fs;
    be<uint32_t> size;
    be<uint32_t> data;
    be<uint32_t> attrib_count;
    be<uint32_t> num_divisors;
    be<uint32_t> divisors[2];
};
static_assert(sizeof(FetchShader) == 0x20);

struct AttribStream {
    be<uint32_t> location;
    be<uint32_t> buffer;
    be<uint32_t> offset;
    be<uint32_t> format;
    be<uint32_t> type; // 0 per vertex, 1 per instance
    be<uint32_t> alu_divisor;
    be<uint32_t> mask;
    be<uint32_t> endian_swap;
};
static_assert(sizeof(AttribStream) == 0x20);

struct LoopVar {
    be<uint32_t> offset;
    be<uint32_t> value;
};

struct VertexShader {
    be<uint32_t> sq_pgm_resources_vs;
    be<uint32_t> vgt_primitiveid_en;
    be<uint32_t> spi_vs_out_config;
    be<uint32_t> num_spi_vs_out_id;
    be<uint32_t> spi_vs_out_id[10];
    be<uint32_t> pa_cl_vs_out_cntl;
    be<uint32_t> sq_vtx_semantic_clear;
    be<uint32_t> num_sq_vtx_semantic;
    be<uint32_t> sq_vtx_semantic[32];
    be<uint32_t> vgt_strmout_buffer_en;
    be<uint32_t> vgt_vertex_reuse_block_cntl;
    be<uint32_t> vgt_hos_reuse_depth;
    be<uint32_t> size;
    be<uint32_t> data;
    be<uint32_t> mode;
    be<uint32_t> uniform_block_count;
    be<uint32_t> uniform_blocks;
    be<uint32_t> uniform_var_count;
    be<uint32_t> uniform_vars;
    be<uint32_t> initial_value_count;
    be<uint32_t> initial_values;
    be<uint32_t> loop_var_count;
    be<uint32_t> loop_vars;
    be<uint32_t> sampler_var_count;
    be<uint32_t> sampler_vars;
    be<uint32_t> attrib_var_count;
    be<uint32_t> attrib_vars;
    be<uint32_t> ring_item_size;
    be<uint32_t> has_stream_out;
    be<uint32_t> stream_out_stride[4];
    RBuffer gx2r_data;
};
static_assert(sizeof(VertexShader) == 0x134);
static_assert(offsetof(VertexShader, size) == 0xD0 && offsetof(VertexShader, loop_vars) == 0xF8);
static_assert(offsetof(VertexShader, gx2r_data) == 0x124);

// GX2GeometryShader: the geometry program and the copy shader that moves
// its output from the GS->VS ring to the rasterizer.
struct GeometryShader {
    be<uint32_t> sq_pgm_resources_gs;
    be<uint32_t> vgt_gs_out_prim_type;
    be<uint32_t> vgt_gs_mode;
    be<uint32_t> pa_cl_vs_out_cntl;
    be<uint32_t> sq_pgm_resources_vs;
    be<uint32_t> sq_gs_vert_itemsize;
    be<uint32_t> spi_vs_out_config;
    be<uint32_t> num_spi_vs_out_id;
    be<uint32_t> spi_vs_out_id[10];
    be<uint32_t> vgt_strmout_buffer_en;
    be<uint32_t> size;
    be<uint32_t> data;
    be<uint32_t> copy_size; // the copy shader (runs as the vertex shader)
    be<uint32_t> copy_data;
    be<uint32_t> mode;
    be<uint32_t> uniform_block_count;
    be<uint32_t> uniform_blocks;
    be<uint32_t> uniform_var_count;
    be<uint32_t> uniform_vars;
    be<uint32_t> initial_value_count;
    be<uint32_t> initial_values;
    be<uint32_t> loop_var_count;
    be<uint32_t> loop_vars;
    be<uint32_t> sampler_var_count;
    be<uint32_t> sampler_vars;
    be<uint32_t> ring_item_size;
    be<uint32_t> has_stream_out;
    be<uint32_t> stream_out_stride[4];
    RBuffer gx2r_data;
    RBuffer gx2r_copy_data;
};
static_assert(sizeof(GeometryShader) == 0xC0);
static_assert(offsetof(GeometryShader, size) == 0x4C && offsetof(GeometryShader, ring_item_size) == 0x88);

struct PixelShader {
    be<uint32_t> sq_pgm_resources_ps;
    be<uint32_t> sq_pgm_exports_ps;
    be<uint32_t> spi_ps_in_control_0;
    be<uint32_t> spi_ps_in_control_1;
    be<uint32_t> num_spi_ps_input_cntl;
    be<uint32_t> spi_ps_input_cntl[32];
    be<uint32_t> cb_shader_mask;
    be<uint32_t> cb_shader_control;
    be<uint32_t> db_shader_control;
    be<uint32_t> spi_input_z;
    be<uint32_t> size;
    be<uint32_t> data;
    be<uint32_t> mode;
    be<uint32_t> uniform_block_count;
    be<uint32_t> uniform_blocks;
    be<uint32_t> uniform_var_count;
    be<uint32_t> uniform_vars;
    be<uint32_t> initial_value_count;
    be<uint32_t> initial_values;
    be<uint32_t> loop_var_count;
    be<uint32_t> loop_vars;
    be<uint32_t> sampler_var_count;
    be<uint32_t> sampler_vars;
    RBuffer gx2r_data;
};
static_assert(sizeof(PixelShader) == 0xE8);
static_assert(offsetof(PixelShader, size) == 0xA4 && offsetof(PixelShader, gx2r_data) == 0xD8);

struct OutputStream {
    be<uint32_t> size;
    be<uint32_t> buffer;
    be<uint32_t> stride;
    RBuffer gx2r_data;
    be<uint32_t> context; // GX2StreamContext*: the GPU stores the write offset there
};
static_assert(sizeof(OutputStream) == 0x20);

// ---------------------------------------------------------- context state
// GX2ContextState: shadow copies of the register spaces the GPU loads on a
// context switch and updates while shadowing is enabled, then a display
// list that loads them.
namespace context_state {
constexpr uint32_t kConfig = 0x0000;   // 0xB00 dwords
constexpr uint32_t kContext = 0x2C00;  // 0x400
constexpr uint32_t kAlu = 0x3C00;      // 0x800
constexpr uint32_t kLoop = 0x5C00;     // 0x60
constexpr uint32_t kResource = 0x5E00; // 0xD9E
constexpr uint32_t kSampler = 0x9500;  // 0xA2
constexpr uint32_t kProfilingEnabled = 0x9800;
constexpr uint32_t kDisplayListSize = 0x9804;
constexpr uint32_t kDisplayList = 0x9E00;
constexpr uint32_t kDisplayListCapacity = 0x300; // bytes
constexpr uint32_t kSize = 0xA100;
} // namespace context_state

// ------------------------------------------------------- register structs
struct AlphaTestReg { be<uint32_t> sx_alpha_test_control; be<uint32_t> sx_alpha_ref; };
struct AlphaToMaskReg { be<uint32_t> db_alpha_to_mask; };
struct BlendControlReg { be<uint32_t> target; be<uint32_t> cb_blend_control; };
struct ColorControlReg { be<uint32_t> cb_color_control; };
struct DepthStencilControlReg { be<uint32_t> db_depth_control; };
struct StencilMaskReg { be<uint32_t> db_stencilrefmask; be<uint32_t> db_stencilrefmask_bf; };
struct PolygonControlReg { be<uint32_t> pa_su_sc_mode_cntl; };
struct PolygonOffsetReg {
    be<uint32_t> front_scale, front_offset, back_scale, back_offset, clamp;
};
struct ScissorReg { be<uint32_t> tl; be<uint32_t> br; };
struct TargetChannelMaskReg { be<uint32_t> cb_target_mask; };
struct ViewportReg {
    be<uint32_t> xscale, xoffset, yscale, yoffset, zscale, zoffset;
    be<uint32_t> vert_clip_adj, vert_disc_adj, horz_clip_adj, horz_disc_adj;
    be<uint32_t> zmin, zmax;
};
static_assert(sizeof(PolygonOffsetReg) == 0x14 && sizeof(ViewportReg) == 0x30);

} // namespace cafe::gx2
