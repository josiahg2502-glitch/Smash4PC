// GX2 textures and samplers: the SQ_TEX_RESOURCE and SQ_TEX_SAMPLER words
// GX2 computes into the title's GX2Texture / GX2Sampler, and binding them
// to shader stages.

#include "gx2/internal.h"

#include "cafe/export.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace cafe::gx2 {
namespace {

using namespace latte;

void GX2InitTextureRegs(Texture* texture) {
    Surface& s = texture->surface;
    if (texture->view_num_mips == 0u) texture->view_num_mips = 1u;
    if (texture->view_num_slices == 0u) texture->view_num_slices = 1u;
    if (s.width == 0u) s.width = 1u;
    if (s.height == 0u) s.height = 1u;
    if (s.depth == 0u) s.depth = 1u;
    if (s.mip_levels == 0u) s.mip_levels = 1u;

    const uint32_t format = s.format;
    const uint32_t hw = format & 0x3F;
    const uint32_t dim = s.dim;
    uint32_t pitch = s.pitch;
    if (fmt::is_compressed(hw)) pitch *= 4; // in pixels
    pitch = std::max(pitch, 8u);
    const bool depth_tiles = (s.use & surface_use::kDepthBuffer) != 0;
    texture->word0 = field(dim, 0, 3) | field(s.tile_mode, 3, 4) | field(depth_tiles, 7, 1) |
                     field(pitch / 8 - 1, 8, 11) | field(s.width - 1, 19, 13);

    uint32_t depth = 0;
    if (dim == kDimCube) depth = s.depth / 6 - 1;
    else if (dim == kDim3D || dim == kDim2DMsaaArray || dim == kDim2DArray || dim == kDim1DArray) depth = s.depth - 1;
    texture->word1 = field(s.height - 1, 0, 13) | field(depth, 13, 13) | field(hw, 26, 6);

    const uint32_t comp = (format & attrib_format::kSigned) ? 1 : 0;
    uint32_t num_format = 0;
    if (format & attrib_format::kScaled) num_format = 2;
    else if (format & attrib_format::kInteger) num_format = 1;
    const uint32_t map = texture->comp_map;
    texture->word4 = field(comp, 0, 2) | field(comp, 2, 2) | field(comp, 4, 2) | field(comp, 6, 2) |
                     field(num_format, 8, 2) | field((format & attrib_format::kDegamma) != 0, 11, 1) |
                     field(endian::kNone, 12, 2) | field(2, 14, 2) /* REQUEST_SIZE */ | field(map >> 24, 16, 3) |
                     field(map >> 16, 19, 3) | field(map >> 8, 22, 3) | field(map, 25, 3) |
                     field(texture->view_first_mip, 28, 4);

    uint32_t last_level = texture->view_first_mip + texture->view_num_mips - 1;
    if (s.aa != 0u) last_level = s.aa; // MSAA textures: LAST_LEVEL holds log2(samples)
    const bool cube_array = dim == kDimCube && depth != 0;
    texture->word5 = (texture->word5 & ~(field(~0u, 0, 4) | field(~0u, 4, 13) | field(~0u, 17, 13) | field(~0u, 30, 2))) |
                     field(last_level, 0, 4) | field(texture->view_first_slice, 4, 13) |
                     field(texture->view_first_slice + texture->view_num_slices - 1, 17, 13) | field(cube_array, 30, 2);
    texture->word6 = (texture->word6 & ~(field(~0u, 2, 3) | field(~0u, 5, 3) | field(~0u, 30, 2))) |
                     field(4, 2, 3) /* MAX_ANISO_RATIO */ | field(7, 5, 3) /* PERF_MODULATION */ |
                     resource::kTypeValidTexture;
}

void set_texture(const Texture* texture, uint32_t stage_base, uint32_t unit) {
    const Surface& s = texture->surface;
    uint32_t image = s.image;
    uint32_t mips = s.mipmaps;
    if (is_macro_tiled(s.tile_mode) && ((s.swizzle >> 16) & 0xFF) != 0) {
        image ^= s.swizzle & 0xFFFF;
        mips ^= s.swizzle & 0xFFFF;
    }
    const uint32_t words[resource::kWords] = {
        texture->word0, texture->word1, image >> 8, mips >> 8, texture->word4, texture->word5, texture->word6,
    };
    set_resource(stage_base + unit, words);
}

void GX2SetPixelTexture(Texture* texture, uint32_t unit) {
    ApiLock lock;
    set_texture(texture, resource::kPsTexture, unit);
}
void GX2SetVertexTexture(Texture* texture, uint32_t unit) {
    ApiLock lock;
    set_texture(texture, resource::kVsTexture, unit);
}
void GX2SetGeometryTexture(Texture* texture, uint32_t unit) {
    ApiLock lock;
    set_texture(texture, resource::kGsTexture, unit);
}

// ----------------------------------------------------------------- samplers
uint32_t lod_4_6(float value) { return static_cast<uint32_t>(std::clamp(value, 0.0f, 16.0f) * 64.0f) & 0x3FF; }
uint32_t lod_bias_1_5_6(float value) {
    return static_cast<uint32_t>(static_cast<int32_t>(std::clamp(value, -32.0f, 32.0f) * 64.0f)) & 0xFFF;
}

void update(be<uint32_t>& word, int shift, int width, uint32_t value) {
    const uint32_t mask = field(~0u, shift, width);
    word = (word & ~mask) | field(value, shift, width);
}

void GX2InitSampler(Sampler* sampler, uint32_t clamp, uint32_t filter) {
    sampler->word0 = field(clamp, 0, 3) | field(clamp, 3, 3) | field(clamp, 6, 3) | field(filter, 9, 3) |
                     field(filter, 12, 3);
    sampler->word1 = field(1023, 10, 10); // MAX_LOD
    sampler->word2 = field(1, 31, 1);     // TYPE
}
void GX2InitSamplerBorderType(Sampler* sampler, uint32_t type) { update(sampler->word0, 22, 2, type); }
void GX2InitSamplerClamping(Sampler* sampler, uint32_t x, uint32_t y, uint32_t z) {
    update(sampler->word0, 0, 3, x);
    update(sampler->word0, 3, 3, y);
    update(sampler->word0, 6, 3, z);
}
void GX2InitSamplerDepthCompare(Sampler* sampler, uint32_t func) { update(sampler->word0, 26, 3, func); }
void GX2InitSamplerFilterAdjust(Sampler* sampler, bool high_precision, uint32_t perf_mip, uint32_t perf_z) {
    update(sampler->word2, 14, 1, high_precision);
    update(sampler->word2, 15, 3, perf_mip);
    update(sampler->word2, 18, 2, perf_z);
}
void GX2InitSamplerLOD(Sampler* sampler, float min_lod, float max_lod, float bias) {
    sampler->word1 = field(lod_4_6(min_lod), 0, 10) | field(lod_4_6(max_lod), 10, 10) |
                     field(lod_bias_1_5_6(bias), 20, 12);
}
void GX2InitSamplerLODAdjust(Sampler* sampler, float aniso_bias, bool lod_uses_minor_axis) {
    update(sampler->word2, 20, 6, static_cast<uint32_t>(std::clamp(aniso_bias, 0.0f, 2.0f) * 32.0f));
    update(sampler->word0, 31, 1, lod_uses_minor_axis);
}
void GX2InitSamplerRoundingMode(Sampler* sampler, uint32_t mode) { update(sampler->word2, 28, 1, mode); }
void GX2InitSamplerXYFilter(Sampler* sampler, uint32_t mag, uint32_t min, uint32_t max_aniso) {
    update(sampler->word0, 9, 3, mag);
    update(sampler->word0, 12, 3, min);
    update(sampler->word0, 19, 3, max_aniso);
}
void GX2InitSamplerZMFilter(Sampler* sampler, uint32_t z, uint32_t mip) {
    update(sampler->word0, 15, 2, z);
    update(sampler->word0, 17, 2, mip);
}

void GX2SetPixelSampler(Sampler* sampler, uint32_t unit) {
    ApiLock lock;
    set_sampler(sampler::kPs + unit, sampler->word0, sampler->word1, sampler->word2);
}
void GX2SetVertexSampler(Sampler* sampler, uint32_t unit) {
    ApiLock lock;
    set_sampler(sampler::kVs + unit, sampler->word0, sampler->word1, sampler->word2);
}
void GX2SetGeometrySampler(Sampler* sampler, uint32_t unit) {
    ApiLock lock;
    set_sampler(sampler::kGs + unit, sampler->word0, sampler->word1, sampler->word2);
}

void set_border_color(uint32_t base, uint32_t unit, float r, float g, float b, float a) {
    ApiLock lock;
    const uint32_t values[] = {std::bit_cast<uint32_t>(r), std::bit_cast<uint32_t>(g), std::bit_cast<uint32_t>(b),
                               std::bit_cast<uint32_t>(a)};
    set_config_regs(base + 16 * unit, values);
}
void GX2SetPixelSamplerBorderColor(uint32_t unit, float r, float g, float b, float a) {
    set_border_color(reg::TD_PS_SAMPLER_BORDER0_RED, unit, r, g, b, a);
}
void GX2SetVertexSamplerBorderColor(uint32_t unit, float r, float g, float b, float a) {
    set_border_color(reg::TD_VS_SAMPLER_BORDER0_RED, unit, r, g, b, a);
}
void GX2SetGeometrySamplerBorderColor(uint32_t unit, float r, float g, float b, float a) {
    set_border_color(reg::TD_GS_SAMPLER_BORDER0_RED, unit, r, g, b, a);
}

} // namespace

CAFE_EXPORT(gx2, GX2InitTextureRegs, GX2InitTextureRegs);
CAFE_EXPORT(gx2, GX2SetPixelTexture, GX2SetPixelTexture);
CAFE_EXPORT(gx2, GX2SetVertexTexture, GX2SetVertexTexture);
CAFE_EXPORT(gx2, GX2SetGeometryTexture, GX2SetGeometryTexture);
CAFE_EXPORT(gx2, GX2InitSampler, GX2InitSampler);
CAFE_EXPORT(gx2, GX2InitSamplerBorderType, GX2InitSamplerBorderType);
CAFE_EXPORT(gx2, GX2InitSamplerClamping, GX2InitSamplerClamping);
CAFE_EXPORT(gx2, GX2InitSamplerDepthCompare, GX2InitSamplerDepthCompare);
CAFE_EXPORT(gx2, GX2InitSamplerFilterAdjust, GX2InitSamplerFilterAdjust);
CAFE_EXPORT(gx2, GX2InitSamplerLOD, GX2InitSamplerLOD);
CAFE_EXPORT(gx2, GX2InitSamplerLODAdjust, GX2InitSamplerLODAdjust);
CAFE_EXPORT(gx2, GX2InitSamplerRoundingMode, GX2InitSamplerRoundingMode);
CAFE_EXPORT(gx2, GX2InitSamplerXYFilter, GX2InitSamplerXYFilter);
CAFE_EXPORT(gx2, GX2InitSamplerZMFilter, GX2InitSamplerZMFilter);
CAFE_EXPORT(gx2, GX2SetPixelSampler, GX2SetPixelSampler);
CAFE_EXPORT(gx2, GX2SetVertexSampler, GX2SetVertexSampler);
CAFE_EXPORT(gx2, GX2SetGeometrySampler, GX2SetGeometrySampler);
CAFE_EXPORT(gx2, GX2SetPixelSamplerBorderColor, GX2SetPixelSamplerBorderColor);
CAFE_EXPORT(gx2, GX2SetVertexSamplerBorderColor, GX2SetVertexSamplerBorderColor);
CAFE_EXPORT(gx2, GX2SetGeometrySamplerBorderColor, GX2SetGeometrySamplerBorderColor);

} // namespace cafe::gx2
