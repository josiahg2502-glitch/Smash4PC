// ShaderEnvironment from the register file: what the context registers,
// resources and SQ_CONFIG say about how a draw's shaders link and run.

#include "latte/translate.h"

#include "gpu/latte.h"

#include <bit>
#include <algorithm>

namespace cafe::latte {

void build_environment(const uint32_t* regs, Stage stage, ShaderEnvironment& env) {
    const auto r = [&](uint32_t address) { return regs[address >> 2]; };
    env = ShaderEnvironment{};
    env.stage = stage;
    env.uniform_registers = (r(reg::SQ_CONFIG) >> 2) & 1; // DX9_CONSTS
    env.num_gprs = static_cast<uint8_t>(r(stage == Stage::kVertex ? reg::SQ_PGM_RESOURCES_VS : reg::SQ_PGM_RESOURCES_PS));

    const uint32_t texture_base = stage == Stage::kVertex ? resource::kVsTexture : resource::kPsTexture;
    for (uint32_t slot = 0; slot < 18; ++slot) {
        const uint32_t base = kResourceBase + (texture_base + slot) * resource::kWords * 4;
        ShaderEnvironment::Texture& t = env.textures[slot];
        if ((r(base + 24) >> 30) != (resource::kTypeValidTexture >> 30)) continue; // unbound: 2D float
        t.dim = static_cast<uint8_t>(r(base) & 7);
        const uint32_t word4 = r(base + 16);
        if (((word4 >> 8) & 3) == 1) { // NUM_FORMAT_ALL: integer
            t.kind = (word4 & 3) == 1 ? TextureKind::kSint : TextureKind::kUint;
        }
    }

    // SQ_VTX_SEMANTIC_CLEAR marks the entries the vertex shader does not use.
    const uint32_t cleared = r(reg::SQ_VTX_SEMANTIC_CLEAR);
    for (int n = 0; n < 32; ++n) {
        env.vtx_semantic[n] = (cleared >> n) & 1 ? 0xFF : static_cast<uint8_t>(r(reg::SQ_VTX_SEMANTIC_0 + n * 4));
    }
    for (int n = 0; n < 32; ++n) {
        env.vs_out_semantic[n] = static_cast<uint8_t>(r(reg::SPI_VS_OUT_ID_0 + (n / 4) * 4) >> ((n % 4) * 8));
    }
    env.clip_space_dx = (r(reg::PA_CL_CLIP_CNTL) >> 19) & 1;
    // OpenGL-style clip space is z * zscale + zoffset on the console. The remap to [0, 1] and the viewport depth
    // range [zoffset - zscale, zoffset + zscale] give that only while the range stays inside [0, 1], which Vulkan
    // clamps it to; outside (Smash's Smash Ball and effects: zscale 1, zoffset 0) the DX path's range
    // [zoffset, zoffset + zscale], without the remap, gives it for every visible depth.
    if (!env.clip_space_dx) {
        const float zscale = std::bit_cast<float>(r(reg::PA_CL_VPORT_XSCALE_0 + 16));
        const float zoffset = std::bit_cast<float>(r(reg::PA_CL_VPORT_XSCALE_0 + 20));
        env.clip_space_dx = zoffset - zscale < -1e-6f || zoffset + zscale > 1.0f + 1e-6f;
    }
    env.point_size_export = (r(reg::PA_CL_VS_OUT_CNTL) >> 24) & 1;
    if (r(reg::VGT_STRMOUT_EN) & 1) {
        env.stream_out_mask = static_cast<uint8_t>(r(reg::VGT_STRMOUT_BUFFER_EN) & 0xF);
        for (int i = 0; i < 4; ++i) {
            env.stream_out_stride[i] = static_cast<uint16_t>(r(reg::VGT_STRMOUT_VTX_STRIDE_0 + i * 16) * 4);
        }
    }

    const uint32_t in0 = r(reg::SPI_PS_IN_CONTROL_0);
    const uint32_t in1 = r(reg::SPI_PS_IN_CONTROL_0 + 4);
    env.ps_input_count = static_cast<uint8_t>(std::min<uint32_t>(in0 & 0x3F, 32));
    if ((in0 >> 8) & 1) env.ps_position_input = static_cast<int8_t>((in0 >> 10) & 0x1F);
    for (int k = 0; k < env.ps_input_count; ++k) {
        const uint32_t c = r(reg::SPI_PS_INPUT_CNTL_0 + k * 4);
        ShaderEnvironment::PsInput& in = env.ps_inputs[k];
        in.semantic = static_cast<uint8_t>(c);
        in.default_value = static_cast<uint8_t>((c >> 8) & 3);
        in.flat = (c >> 10) & 1;
        in.centroid = (c >> 11) & 1;
        in.linear = (c >> 12) & 1;
        in.point_sprite = (c >> 17) & 1;
    }
    if ((in0 >> 15) & 0xF) env.ps_param_gen_gpr = static_cast<int8_t>((in0 >> 19) & 0x7F);
    const uint32_t interp = r(reg::SPI_INTERP_CONTROL_0);
    for (int i = 0; i < 4; ++i) env.point_sprite_override[i] = static_cast<uint8_t>((interp >> (2 + 3 * i)) & 7);
    env.point_sprite_top_1 = (interp >> 14) & 1;
    env.ps_front_face = (in1 >> 8) & 1;
    env.ps_front_face_chan = static_cast<uint8_t>((in1 >> 9) & 3);
    env.ps_front_face_all_bits = (in1 >> 11) & 1;
    env.ps_front_face_gpr = static_cast<uint8_t>((in1 >> 12) & 0x1F);

    env.cb_shader_mask = r(reg::CB_SHADER_MASK);
    env.multiwrite = (r(reg::CB_COLOR_CONTROL) >> 1) & 1;
    for (int t = 0; t < 8; ++t) {
        switch ((r(reg::CB_COLOR0_INFO + t * 4) >> 12) & 7) { // NUMBER_TYPE
        case 4: env.color_kind[t] = TextureKind::kUint; break;
        case 5: env.color_kind[t] = TextureKind::kSint; break;
        default: env.color_kind[t] = TextureKind::kFloat; break;
        }
    }
    const uint32_t alpha = r(reg::SX_ALPHA_TEST_CONTROL);
    env.alpha_func = (alpha >> 3) & 1 ? static_cast<uint8_t>(alpha & 7) : 7;
}

namespace {

struct Hasher {
    uint64_t h = 0xCBF29CE484222325ull;
    void add(uint64_t v) {
        for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
    }
};

} // namespace

uint64_t environment_key(const ShaderEnvironment& env) {
    Hasher h;
    h.add(static_cast<uint64_t>(env.stage));
    h.add(env.uniform_registers);
    for (const auto& t : env.textures) h.add(t.dim | (static_cast<uint32_t>(t.kind) << 8));
    // The linkage between the stages matters to both.
    for (uint8_t s : env.vs_out_semantic) h.add(s);
    h.add(env.ps_input_count);
    h.add(static_cast<uint8_t>(env.ps_position_input));
    for (int k = 0; k < env.ps_input_count; ++k) {
        const auto& in = env.ps_inputs[k];
        h.add(in.semantic | (in.default_value << 8) | (in.flat << 16) | (in.linear << 17) | (in.centroid << 18) |
              (in.point_sprite << 19));
    }
    if (env.stage == Stage::kVertex) {
        for (uint8_t s : env.vtx_semantic) h.add(s);
        h.add(env.clip_space_dx | (env.point_size_export << 1) | (env.stream_out_mask << 2));
        for (uint16_t s : env.stream_out_stride) h.add(s);
    } else {
        h.add(env.ps_front_face | (env.ps_front_face_all_bits << 1) | (env.ps_front_face_gpr << 2) |
              (env.ps_front_face_chan << 10));
        h.add(static_cast<uint8_t>(env.ps_param_gen_gpr) | (env.point_sprite_top_1 << 8) |
              (env.point_sprite_override[0] << 9) | (env.point_sprite_override[1] << 12) |
              (env.point_sprite_override[2] << 15) | (env.point_sprite_override[3] << 18));
        h.add(env.cb_shader_mask);
        h.add(env.multiwrite);
        for (TextureKind k : env.color_kind) h.add(static_cast<uint8_t>(k));
        h.add(env.alpha_func);
    }
    return h.h;
}

} // namespace cafe::latte
