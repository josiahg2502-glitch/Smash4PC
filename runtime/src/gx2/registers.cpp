// GX2's register-struct API: GX2Init*Reg encodes pipeline state into the
// hardware register values the title keeps in its own structures;
// GX2Set*Reg writes them to the GPU. The GX2Set* convenience functions do
// both.

#include "gx2/internal.h"

#include "cafe/export.h"

#include <algorithm>
#include <bit>

namespace cafe::gx2 {

using namespace latte;

uint32_t make_db_depth_control(bool depth_test, bool depth_write, uint32_t depth_compare, bool stencil_test,
                               bool backface_stencil, uint32_t front_func, uint32_t front_zpass,
                               uint32_t front_zfail, uint32_t front_fail, uint32_t back_func,
                               uint32_t back_zpass, uint32_t back_zfail, uint32_t back_fail) {
    return field(stencil_test, 0, 1) | field(depth_test, 1, 1) | field(depth_write, 2, 1) |
           field(depth_compare, 4, 3) | field(backface_stencil, 7, 1) | field(front_func, 8, 3) |
           field(front_fail, 11, 3) | field(front_zpass, 14, 3) | field(front_zfail, 17, 3) |
           field(back_func, 20, 3) | field(back_fail, 23, 3) | field(back_zpass, 26, 3) |
           field(back_zfail, 29, 3);
}

uint32_t make_pa_su_sc_mode_cntl(uint32_t front_face, bool cull_front, bool cull_back, uint32_t poly_mode,
                                 uint32_t poly_mode_front, uint32_t poly_mode_back, bool offset_front,
                                 bool offset_back, bool offset_para) {
    return field(cull_front, 0, 1) | field(cull_back, 1, 1) | field(front_face != 0, 2, 1) |
           field(poly_mode, 3, 2) | field(poly_mode_front, 5, 3) | field(poly_mode_back, 8, 3) |
           field(offset_front, 11, 1) | field(offset_back, 12, 1) | field(offset_para, 13, 1);
}

uint32_t make_cb_color_control(uint32_t rop3, uint32_t target_blend_enable, bool multi_write, bool color_write) {
    const uint32_t special_op = color_write ? 0 : 1; // NORMAL / DISABLE
    return field(multi_write, 1, 1) | field(special_op, 4, 3) | field(target_blend_enable, 8, 8) |
           field(rop3, 16, 8);
}

uint32_t make_cb_blend_control(uint32_t color_src, uint32_t color_dst, uint32_t color_combine, bool separate_alpha,
                               uint32_t alpha_src, uint32_t alpha_dst, uint32_t alpha_combine) {
    return field(color_src, 0, 5) | field(color_combine, 5, 3) | field(color_dst, 8, 5) | field(alpha_src, 16, 5) |
           field(alpha_combine, 21, 3) | field(alpha_dst, 24, 5) | field(separate_alpha, 29, 1);
}

uint32_t make_db_alpha_to_mask(bool enable, uint32_t mode) {
    // Per-pixel dither offsets for the four pixels of a quad.
    static constexpr uint8_t kOffsets[5][4] = {{2, 2, 2, 2}, {0, 2, 3, 1}, {2, 1, 0, 3}, {1, 3, 2, 0}, {3, 0, 1, 2}};
    const uint8_t* o = kOffsets[mode < 5 ? mode : 0];
    return field(enable, 0, 1) | field(o[0], 8, 2) | field(o[1], 10, 2) | field(o[2], 12, 2) | field(o[3], 14, 2);
}

// The title's clip control, restored when a copy draw (special state) ends.
uint32_t g_clip_cntl = 0;

void set_rasterizer_clip_control(bool rasterizer, bool z_clip, bool half_z) {
    g_clip_cntl = field(half_z, 19, 1) | field(!rasterizer, 22, 1) | field(!z_clip, 26, 1) | field(!z_clip, 27, 1);
    set_context_reg(reg::PA_CL_CLIP_CNTL, g_clip_cntl);
}

namespace {

uint32_t fixed_12_4(float value) { return static_cast<uint32_t>(value * 8.0f); }
uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }

// ------------------------------------------------------------ alpha test
void GX2InitAlphaTestReg(AlphaTestReg* reg, bool enable, uint32_t func, float ref) {
    reg->sx_alpha_test_control = field(func, 0, 3) | field(enable, 3, 1);
    reg->sx_alpha_ref = bits(ref);
}
void GX2SetAlphaTestReg(AlphaTestReg* reg) {
    ApiLock lock;
    set_context_reg(reg::SX_ALPHA_TEST_CONTROL, reg->sx_alpha_test_control);
    set_context_reg(reg::SX_ALPHA_REF, reg->sx_alpha_ref);
}
void GX2SetAlphaTest(bool enable, uint32_t func, float ref) {
    AlphaTestReg reg;
    GX2InitAlphaTestReg(&reg, enable, func, ref);
    GX2SetAlphaTestReg(&reg);
}

// --------------------------------------------------------- alpha to mask
void GX2InitAlphaToMaskReg(AlphaToMaskReg* reg, bool enable, uint32_t mode) {
    reg->db_alpha_to_mask = make_db_alpha_to_mask(enable, mode);
}
void GX2SetAlphaToMaskReg(AlphaToMaskReg* reg) {
    ApiLock lock;
    set_context_reg(reg::DB_ALPHA_TO_MASK, reg->db_alpha_to_mask);
}
void GX2SetAlphaToMask(bool enable, uint32_t mode) {
    ApiLock lock;
    set_context_reg(reg::DB_ALPHA_TO_MASK, make_db_alpha_to_mask(enable, mode));
}

// ----------------------------------------------------------------- blend
void GX2InitBlendControlReg(BlendControlReg* reg, uint32_t target, uint32_t color_src, uint32_t color_dst,
                            uint32_t color_combine, bool separate_alpha, uint32_t alpha_src, uint32_t alpha_dst,
                            uint32_t alpha_combine) {
    reg->target = target;
    reg->cb_blend_control = make_cb_blend_control(color_src, color_dst, color_combine, separate_alpha, alpha_src,
                                                  alpha_dst, alpha_combine);
}
void GX2SetBlendControlReg(BlendControlReg* reg) {
    ApiLock lock;
    set_context_reg(reg::CB_BLEND0_CONTROL + (reg->target & 7) * 4, reg->cb_blend_control);
}
void GX2SetBlendControl(uint32_t target, uint32_t color_src, uint32_t color_dst, uint32_t color_combine,
                        bool separate_alpha, uint32_t alpha_src, uint32_t alpha_dst, uint32_t alpha_combine) {
    ApiLock lock;
    set_context_reg(reg::CB_BLEND0_CONTROL + (target & 7) * 4,
                    make_cb_blend_control(color_src, color_dst, color_combine, separate_alpha, alpha_src, alpha_dst,
                                          alpha_combine));
}
void GX2SetBlendConstantColor(float r, float g, float b, float a) {
    ApiLock lock;
    const uint32_t values[] = {bits(r), bits(g), bits(b), bits(a)};
    set_context_regs(reg::CB_BLEND_RED, values);
}
// ----------------------------------------------------------- tessellation
// GX2TessellationMode (discrete, continuous, adaptive) and the patch type
// from the tessellated primitive mode: lines, triangles or quads.
void GX2SetTessellation(uint32_t mode, uint32_t primitive, uint32_t) {
    ApiLock lock;
    uint32_t patch = 1; // triangles
    switch (primitive & 0x7F) {
    case 0x02: case 0x03: patch = 0; break;
    case 0x13: case 0x14: patch = 2; break;
    }
    set_context_reg(reg::VGT_OUTPUT_PATH_CNTL, 1); // PATH_SELECT tessellation
    set_context_reg(reg::VGT_HOS_CNTL, mode & 3);
    set_context_reg(reg::VGT_GROUP_PRIM_TYPE, patch);
}
void GX2SetMaxTessellationLevel(float level) {
    ApiLock lock;
    set_context_reg(reg::VGT_HOS_MAX_TESS_LEVEL, bits(level));
}
void GX2SetMinTessellationLevel(float level) {
    ApiLock lock;
    set_context_reg(reg::VGT_HOS_MIN_TESS_LEVEL, bits(level));
}

// --------------------------------------------------------- special states
// GX2SpecialState: the states GX2's own clear and copy draws need. A copy
// draw gives positions in window coordinates: no viewport transform, no
// clipping. The runtime clears and copies without draws, so the other states
// change nothing it uses.
constexpr uint32_t kSpecialStateCopy = 2;
constexpr uint32_t kVteDefault = 0x43F;    // scale and offset X/Y/Z, W0 format
constexpr uint32_t kVteWindowXy = 0x400;   // W0 format only
constexpr uint32_t kClipDisable = 1u << 16;
void GX2SetSpecialState(uint32_t state, bool enable) {
    if (state != kSpecialStateCopy) return;
    ApiLock lock;
    set_context_reg(reg::PA_CL_VTE_CNTL, enable ? kVteWindowXy : kVteDefault);
    set_context_reg(reg::PA_CL_CLIP_CNTL, enable ? g_clip_cntl | kClipDisable : g_clip_cntl);
}

// GX2BlendConstantColorReg: CB_BLEND_RED..ALPHA as stored.
struct BlendConstantColorReg {
    be<uint32_t> color[4];
};
void GX2InitBlendConstantColorReg(BlendConstantColorReg* reg, float r, float g, float b, float a) {
    reg->color[0] = bits(r);
    reg->color[1] = bits(g);
    reg->color[2] = bits(b);
    reg->color[3] = bits(a);
}
void GX2SetBlendConstantColorReg(BlendConstantColorReg* reg) {
    ApiLock lock;
    const uint32_t values[] = {reg->color[0], reg->color[1], reg->color[2], reg->color[3]};
    set_context_regs(reg::CB_BLEND_RED, values);
}

// ---------------------------------------------------------- color control
void GX2InitColorControlReg(ColorControlReg* reg, uint32_t rop3, uint8_t target_blend_enable, bool multi_write,
                            bool color_write) {
    reg->cb_color_control = make_cb_color_control(rop3 & 0xFF, target_blend_enable, multi_write, color_write);
}
void GX2SetColorControlReg(ColorControlReg* reg) {
    ApiLock lock;
    set_context_reg(reg::CB_COLOR_CONTROL, reg->cb_color_control);
}
void GX2SetColorControl(uint32_t rop3, uint8_t target_blend_enable, bool multi_write, bool color_write) {
    ApiLock lock;
    set_context_reg(reg::CB_COLOR_CONTROL,
                    make_cb_color_control(rop3 & 0xFF, target_blend_enable, multi_write, color_write));
}

// ----------------------------------------------------------- depth/stencil
void GX2InitDepthStencilControlReg(DepthStencilControlReg* reg, bool depth_test, bool depth_write,
                                   uint32_t depth_compare, bool stencil_test, bool backface_stencil,
                                   uint32_t front_func, uint32_t front_zpass, uint32_t front_zfail,
                                   uint32_t front_fail, uint32_t back_func, uint32_t back_zpass, uint32_t back_zfail,
                                   uint32_t back_fail) {
    reg->db_depth_control = make_db_depth_control(depth_test, depth_write, depth_compare, stencil_test,
                                                  backface_stencil, front_func, front_zpass, front_zfail, front_fail,
                                                  back_func, back_zpass, back_zfail, back_fail);
}
void GX2SetDepthStencilControlReg(DepthStencilControlReg* reg) {
    ApiLock lock;
    set_context_reg(reg::DB_DEPTH_CONTROL, reg->db_depth_control);
}
void GX2SetDepthStencilControl(bool depth_test, bool depth_write, uint32_t depth_compare, bool stencil_test,
                               bool backface_stencil, uint32_t front_func, uint32_t front_zpass,
                               uint32_t front_zfail, uint32_t front_fail, uint32_t back_func, uint32_t back_zpass,
                               uint32_t back_zfail, uint32_t back_fail) {
    ApiLock lock;
    set_context_reg(reg::DB_DEPTH_CONTROL,
                    make_db_depth_control(depth_test, depth_write, depth_compare, stencil_test, backface_stencil,
                                          front_func, front_zpass, front_zfail, front_fail, back_func, back_zpass,
                                          back_zfail, back_fail));
}
void GX2SetDepthOnlyControl(bool depth_test, bool depth_write, uint32_t depth_compare) {
    GX2SetDepthStencilControl(depth_test, depth_write, depth_compare, false, false, 0, 0, 0, 0, 0, 0, 0, 0);
}

void GX2InitStencilMaskReg(StencilMaskReg* reg, uint8_t front_mask, uint8_t front_write_mask, uint8_t front_ref,
                           uint8_t back_mask, uint8_t back_write_mask, uint8_t back_ref) {
    reg->db_stencilrefmask = field(front_ref, 0, 8) | field(front_mask, 8, 8) | field(front_write_mask, 16, 8);
    reg->db_stencilrefmask_bf = field(back_ref, 0, 8) | field(back_mask, 8, 8) | field(back_write_mask, 16, 8);
}
void GX2SetStencilMaskReg(StencilMaskReg* reg) {
    ApiLock lock;
    set_context_reg(reg::DB_STENCILREFMASK, reg->db_stencilrefmask);
    set_context_reg(reg::DB_STENCILREFMASK_BF, reg->db_stencilrefmask_bf);
}
void GX2SetStencilMask(uint8_t front_mask, uint8_t front_write_mask, uint8_t front_ref, uint8_t back_mask,
                       uint8_t back_write_mask, uint8_t back_ref) {
    StencilMaskReg reg;
    GX2InitStencilMaskReg(&reg, front_mask, front_write_mask, front_ref, back_mask, back_write_mask, back_ref);
    GX2SetStencilMaskReg(&reg);
}

// ----------------------------------------------------------------- polygon
void GX2InitPolygonControlReg(PolygonControlReg* reg, uint32_t front_face, bool cull_front, bool cull_back,
                              uint32_t poly_mode, uint32_t poly_mode_front, uint32_t poly_mode_back,
                              bool offset_front, bool offset_back, bool offset_para) {
    reg->pa_su_sc_mode_cntl = make_pa_su_sc_mode_cntl(front_face, cull_front, cull_back, poly_mode, poly_mode_front,
                                                      poly_mode_back, offset_front, offset_back, offset_para);
}
void GX2SetPolygonControlReg(PolygonControlReg* reg) {
    ApiLock lock;
    set_context_reg(reg::PA_SU_SC_MODE_CNTL, reg->pa_su_sc_mode_cntl);
}
void GX2SetPolygonControl(uint32_t front_face, bool cull_front, bool cull_back, uint32_t poly_mode,
                          uint32_t poly_mode_front, uint32_t poly_mode_back, bool offset_front, bool offset_back,
                          bool offset_para) {
    ApiLock lock;
    set_context_reg(reg::PA_SU_SC_MODE_CNTL,
                    make_pa_su_sc_mode_cntl(front_face, cull_front, cull_back, poly_mode, poly_mode_front,
                                            poly_mode_back, offset_front, offset_back, offset_para));
}
void GX2SetCullOnlyControl(uint32_t front_face, bool cull_front, bool cull_back) {
    GX2SetPolygonControl(front_face, cull_front, cull_back, 0, 0, 0, false, false, false);
}

void GX2InitPolygonOffsetReg(PolygonOffsetReg* reg, float front_offset, float front_scale, float back_offset,
                             float back_scale, float clamp) {
    reg->front_scale = bits(front_scale * 16.0f);
    reg->front_offset = bits(front_offset);
    reg->back_scale = bits(back_scale * 16.0f);
    reg->back_offset = bits(back_offset);
    reg->clamp = bits(clamp);
}
void GX2SetPolygonOffsetReg(PolygonOffsetReg* reg) {
    ApiLock lock;
    const uint32_t values[] = {reg->front_scale, reg->front_offset, reg->back_scale, reg->back_offset};
    set_context_regs(reg::PA_SU_POLY_OFFSET_FRONT_SCALE, values);
    set_context_reg(reg::PA_SU_POLY_OFFSET_CLAMP, reg->clamp);
}
void GX2SetPolygonOffset(float front_offset, float front_scale, float back_offset, float back_scale, float clamp) {
    PolygonOffsetReg reg;
    GX2InitPolygonOffsetReg(&reg, front_offset, front_scale, back_offset, back_scale, clamp);
    GX2SetPolygonOffsetReg(&reg);
}

void GX2SetLineWidth(float width) {
    ApiLock lock;
    set_context_reg(reg::PA_SU_LINE_CNTL, field(fixed_12_4(width), 0, 16));
}
void GX2SetPointSize(float width, float height) {
    ApiLock lock;
    set_context_reg(reg::PA_SU_POINT_SIZE, field(fixed_12_4(height), 0, 16) | field(fixed_12_4(width), 16, 16));
}
void GX2SetPointLimits(float min, float max) {
    ApiLock lock;
    set_context_reg(reg::PA_SU_POINT_MINMAX, field(fixed_12_4(min), 0, 16) | field(fixed_12_4(max), 16, 16));
}
void GX2SetAAMask(uint8_t ul, uint8_t ur, uint8_t ll, uint8_t lr) {
    ApiLock lock;
    set_context_reg(reg::PA_SC_AA_MASK, field(ul, 0, 8) | field(ur, 8, 8) | field(ll, 16, 8) | field(lr, 24, 8));
}

// ----------------------------------------------------------------- scissor
void GX2InitScissorReg(ScissorReg* reg, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    reg->tl = field(x, 0, 14) | field(y, 16, 14);
    reg->br = field(x + width, 0, 14) | field(y + height, 16, 14);
}
void GX2SetScissorReg(ScissorReg* reg) {
    ApiLock lock;
    const uint32_t values[] = {reg->tl, reg->br};
    set_context_regs(reg::PA_SC_GENERIC_SCISSOR_TL, values);
}
void GX2SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    ScissorReg reg;
    GX2InitScissorReg(&reg, x, y, width, height);
    GX2SetScissorReg(&reg);
}

// ----------------------------------------------------------- channel masks
uint32_t channel_masks(uint32_t m0, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4, uint32_t m5, uint32_t m6,
                       uint32_t m7) {
    return field(m0, 0, 4) | field(m1, 4, 4) | field(m2, 8, 4) | field(m3, 12, 4) | field(m4, 16, 4) |
           field(m5, 20, 4) | field(m6, 24, 4) | field(m7, 28, 4);
}
void GX2InitTargetChannelMasksReg(TargetChannelMaskReg* reg, uint32_t m0, uint32_t m1, uint32_t m2, uint32_t m3,
                                  uint32_t m4, uint32_t m5, uint32_t m6, uint32_t m7) {
    reg->cb_target_mask = channel_masks(m0, m1, m2, m3, m4, m5, m6, m7);
}
void GX2SetTargetChannelMasksReg(TargetChannelMaskReg* reg) {
    ApiLock lock;
    set_context_reg(reg::CB_TARGET_MASK, reg->cb_target_mask);
}
void GX2SetTargetChannelMasks(uint32_t m0, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4, uint32_t m5,
                              uint32_t m6, uint32_t m7) {
    ApiLock lock;
    set_context_reg(reg::CB_TARGET_MASK, channel_masks(m0, m1, m2, m3, m4, m5, m6, m7));
}

// ---------------------------------------------------------------- viewport
void GX2InitViewportReg(ViewportReg* reg, float x, float y, float width, float height, float near_z, float far_z) {
    reg->xscale = bits(width * 0.5f);
    reg->xoffset = bits(x + width * 0.5f);
    reg->yscale = bits(height * -0.5f);
    reg->yoffset = bits(y + height * 0.5f);
    reg->zscale = bits((far_z - near_z) * 0.5f);
    reg->zoffset = bits((far_z + near_z) * 0.5f);
    // Guard band: how far primitives may extend past the viewport before
    // they are clipped, within the 16K rasteriser range.
    float horz = 1.0f, vert = 1.0f;
    if (width != 0.0f && height != 0.0f) {
        if (height < 0.0f) {
            y += height;
            height = -height;
        }
        float h = x + 8192.0f;
        if (h <= 0.0f) h = 8192.0f - (width + x);
        horz = (h + h + width) / width;
        float v = y + 8192.0f;
        if (v <= 0.0f) v = 8192.0f - (height + y);
        vert = (v + v + height) / height;
    }
    reg->vert_clip_adj = bits(vert);
    reg->vert_disc_adj = bits(1.0f);
    reg->horz_clip_adj = bits(horz);
    reg->horz_disc_adj = bits(1.0f);
    reg->zmin = bits(std::min(near_z, far_z));
    reg->zmax = bits(std::max(near_z, far_z));
}
void GX2SetViewportReg(ViewportReg* reg) {
    ApiLock lock;
    const uint32_t transform[] = {reg->xscale, reg->xoffset, reg->yscale, reg->yoffset, reg->zscale, reg->zoffset};
    set_context_regs(reg::PA_CL_VPORT_XSCALE_0, transform);
    const uint32_t guard_band[] = {reg->vert_clip_adj, reg->vert_disc_adj, reg->horz_clip_adj, reg->horz_disc_adj};
    set_context_regs(reg::PA_CL_GB_VERT_CLIP_ADJ, guard_band);
    const uint32_t depth_range[] = {reg->zmin, reg->zmax};
    set_context_regs(reg::PA_SC_VPORT_ZMIN_0, depth_range);
}
void GX2SetViewport(float x, float y, float width, float height, float near_z, float far_z) {
    ViewportReg reg;
    GX2InitViewportReg(&reg, x, y, width, height, near_z, far_z);
    GX2SetViewportReg(&reg);
}

// ------------------------------------------------------------- rasteriser
void GX2SetRasterizerClipControl(bool rasterizer, bool z_clip) {
    ApiLock lock;
    set_rasterizer_clip_control(rasterizer, z_clip, false);
}
void GX2SetRasterizerClipControlEx(bool rasterizer, bool z_clip, bool half_z) {
    ApiLock lock;
    set_rasterizer_clip_control(rasterizer, z_clip, half_z);
}

} // namespace

CAFE_EXPORT(gx2, GX2InitAlphaTestReg, GX2InitAlphaTestReg);
CAFE_EXPORT(gx2, GX2SetAlphaTestReg, GX2SetAlphaTestReg);
CAFE_EXPORT(gx2, GX2SetAlphaTest, GX2SetAlphaTest);
CAFE_EXPORT(gx2, GX2InitAlphaToMaskReg, GX2InitAlphaToMaskReg);
CAFE_EXPORT(gx2, GX2SetAlphaToMaskReg, GX2SetAlphaToMaskReg);
CAFE_EXPORT(gx2, GX2SetAlphaToMask, GX2SetAlphaToMask);
CAFE_EXPORT(gx2, GX2InitBlendControlReg, GX2InitBlendControlReg);
CAFE_EXPORT(gx2, GX2SetBlendControlReg, GX2SetBlendControlReg);
CAFE_EXPORT(gx2, GX2SetBlendControl, GX2SetBlendControl);
CAFE_EXPORT(gx2, GX2SetBlendConstantColor, GX2SetBlendConstantColor);
CAFE_EXPORT(gx2, GX2SetTessellation, GX2SetTessellation);
CAFE_EXPORT(gx2, GX2SetMaxTessellationLevel, GX2SetMaxTessellationLevel);
CAFE_EXPORT(gx2, GX2SetMinTessellationLevel, GX2SetMinTessellationLevel);
CAFE_EXPORT(gx2, GX2SetSpecialState, GX2SetSpecialState);
CAFE_EXPORT(gx2, GX2InitBlendConstantColorReg, GX2InitBlendConstantColorReg);
CAFE_EXPORT(gx2, GX2SetBlendConstantColorReg, GX2SetBlendConstantColorReg);
CAFE_EXPORT(gx2, GX2InitColorControlReg, GX2InitColorControlReg);
CAFE_EXPORT(gx2, GX2SetColorControlReg, GX2SetColorControlReg);
CAFE_EXPORT(gx2, GX2SetColorControl, GX2SetColorControl);
CAFE_EXPORT(gx2, GX2InitDepthStencilControlReg, GX2InitDepthStencilControlReg);
CAFE_EXPORT(gx2, GX2SetDepthStencilControlReg, GX2SetDepthStencilControlReg);
CAFE_EXPORT(gx2, GX2SetDepthStencilControl, GX2SetDepthStencilControl);
CAFE_EXPORT(gx2, GX2SetDepthOnlyControl, GX2SetDepthOnlyControl);
CAFE_EXPORT(gx2, GX2InitStencilMaskReg, GX2InitStencilMaskReg);
CAFE_EXPORT(gx2, GX2SetStencilMaskReg, GX2SetStencilMaskReg);
CAFE_EXPORT(gx2, GX2SetStencilMask, GX2SetStencilMask);
CAFE_EXPORT(gx2, GX2InitPolygonControlReg, GX2InitPolygonControlReg);
CAFE_EXPORT(gx2, GX2SetPolygonControlReg, GX2SetPolygonControlReg);
CAFE_EXPORT(gx2, GX2SetPolygonControl, GX2SetPolygonControl);
CAFE_EXPORT(gx2, GX2SetCullOnlyControl, GX2SetCullOnlyControl);
CAFE_EXPORT(gx2, GX2InitPolygonOffsetReg, GX2InitPolygonOffsetReg);
CAFE_EXPORT(gx2, GX2SetPolygonOffsetReg, GX2SetPolygonOffsetReg);
CAFE_EXPORT(gx2, GX2SetPolygonOffset, GX2SetPolygonOffset);
CAFE_EXPORT(gx2, GX2SetLineWidth, GX2SetLineWidth);
CAFE_EXPORT(gx2, GX2SetPointSize, GX2SetPointSize);
CAFE_EXPORT(gx2, GX2SetPointLimits, GX2SetPointLimits);
CAFE_EXPORT(gx2, GX2SetAAMask, GX2SetAAMask);
CAFE_EXPORT(gx2, GX2InitScissorReg, GX2InitScissorReg);
CAFE_EXPORT(gx2, GX2SetScissorReg, GX2SetScissorReg);
CAFE_EXPORT(gx2, GX2SetScissor, GX2SetScissor);
CAFE_EXPORT(gx2, GX2InitTargetChannelMasksReg, GX2InitTargetChannelMasksReg);
CAFE_EXPORT(gx2, GX2SetTargetChannelMasksReg, GX2SetTargetChannelMasksReg);
CAFE_EXPORT(gx2, GX2SetTargetChannelMasks, GX2SetTargetChannelMasks);
CAFE_EXPORT(gx2, GX2InitViewportReg, GX2InitViewportReg);
CAFE_EXPORT(gx2, GX2SetViewportReg, GX2SetViewportReg);
CAFE_EXPORT(gx2, GX2SetViewport, GX2SetViewport);
CAFE_EXPORT(gx2, GX2SetRasterizerClipControl, GX2SetRasterizerClipControl);
CAFE_EXPORT(gx2, GX2SetRasterizerClipControlEx, GX2SetRasterizerClipControlEx);

} // namespace cafe::gx2
