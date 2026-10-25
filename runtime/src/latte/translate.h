#pragma once

// Latte shader -> GLSL (Vulkan, 4.60) translation.
//
// Each shader runs per invocation exactly as one thread of the console GPU:
// GPRs are 32-bit integers (ivec4 R[n]) and float instructions bit-cast
// them; an ALU group computes every slot from the registers as they were
// before the group, then writes; PV/PS hold the previous group's results.
// Control flow keeps the hardware's per-thread active state and push/pop
// stack in variables, so every clause is guarded by "this thread is
// active". JUMPs only skip work when no thread is active, so they are not
// needed per thread.
//
// What the microcode alone does not decide comes from ShaderEnvironment,
// built from the register file at draw time and part of the shader key.

#include "latte/program.h"

#include <cstdint>
#include <string>

namespace cafe::latte {

enum class Stage : uint8_t { kVertex, kPixel };

// Texture slot shapes as GLSL needs them.
enum class TextureKind : uint8_t { kFloat, kUint, kSint };

struct ShaderEnvironment {
    Stage stage = Stage::kVertex;
    bool uniform_registers = false; // constants come from C0-C255 (SQ_CONFIG.DX9_CONSTS), not kcache
    // SQ_PGM_RESOURCES_*.NUM_GPRS: the registers the program has, which
    // relative addressing ranges over. A property of the program (so not
    // part of the key).
    uint8_t num_gprs = 0;

    struct Texture {
        uint8_t dim = 1; // SQ_TEX_DIM (gx2::SurfaceDim)
        TextureKind kind = TextureKind::kFloat;
    };
    Texture textures[18];

    // Vertex shader. SQ_VTX_SEMANTIC_n: the semantic id the fetch shader
    // loads into R(n+1); 0xFF unused.
    uint8_t vtx_semantic[32];
    // SPI_VS_OUT_ID: semantic id of parameter export n; 0xFF unused.
    uint8_t vs_out_semantic[32];
    bool clip_space_dx = false;   // PA_CL_CLIP_CNTL.DX_CLIP_SPACE_DEF: z in [0, w]
    bool point_size_export = false; // PA_CL_VS_OUT_CNTL.USE_VTX_POINT_SIZE
    uint8_t stream_out_mask = 0;    // buffers with stream-out writes enabled
    uint16_t stream_out_stride[4] = {}; // bytes per vertex

    // Pixel shader inputs (SPI_PS_INPUT_CNTL_n), shared by both stages so
    // vertex outputs land at the locations the pixel shader reads.
    struct PsInput {
        uint8_t semantic = 0xFF;
        uint8_t default_value = 0; // 0: 0000, 1: 0001, 2: 1110, 3: 1111
        bool flat = false;
        bool linear = false;    // no perspective correction
        bool centroid = false;
        bool point_sprite = false; // PT_SPRITE_TEX: receives the point sprite coordinate
    };
    uint8_t ps_input_count = 0;
    PsInput ps_inputs[32];
    int8_t ps_position_input = -1;  // input index replaced by the fragment position
    // Point sprites: PARAM_GEN loads the sprite coordinate into a GPR;
    // PT_SPRITE_TEX inputs get it with SPI_INTERP_CONTROL_0's overrides
    // (0, 1, S, T per component).
    int8_t ps_param_gen_gpr = -1;
    uint8_t point_sprite_override[4] = {2, 3, 0, 1};
    bool point_sprite_top_1 = false; // T is 1 at the top of the sprite
    bool ps_front_face = false;     // SPI_PS_IN_CONTROL_1.FRONT_FACE_ENA
    bool ps_front_face_all_bits = false;
    uint8_t ps_front_face_gpr = 0;
    uint8_t ps_front_face_chan = 0;

    // Pixel shader outputs.
    uint32_t cb_shader_mask = 0;   // CB_SHADER_MASK: 4 bits per render target
    bool multiwrite = false;       // CB_COLOR_CONTROL.MULTIWRITE_ENABLE: export 0 to every target
    TextureKind color_kind[8] = {};
    uint8_t alpha_func = 7;        // SX_ALPHA_TEST_CONTROL function when enabled, 7 (always) otherwise

    ShaderEnvironment() {
        for (uint8_t& v : vtx_semantic) v = 0xFF;
        for (uint8_t& v : vs_out_semantic) v = 0xFF;
    }
};

struct TranslatedShader {
    std::string glsl;
    uint32_t texture_mask = 0;   // texture slots the shader samples
    uint32_t shadow_mask = 0;    // slots sampled with a depth compare (SAMPLE_C*)
    bool uses_registers = false; // reads the uniform register binding
    uint32_t constant_bank_mask = 0; // uniform blocks (kcache banks) read
    uint32_t constant_bank_extent[16] = {}; // bytes of each read from its start (UINT32_MAX: any, relative index)
    uint32_t buffer_mask = 0;        // buffer resources fetched from (VTX_FETCH in TEX clauses)
};

// The environment of a draw's `stage` shader from the GPU register file
// (indexed by register byte address / 4).
void build_environment(const uint32_t* regs, Stage stage, ShaderEnvironment& env);

// A hash of the parts of `env` that the translation of its stage depends on.
uint64_t environment_key(const ShaderEnvironment& env);

// `fetch` is the fetch shader a vertex shader calls (CALL_FS), or null.
// Returns false with a message for microcode the translator cannot handle.
bool translate(const Program& program, const Program* fetch, const ShaderEnvironment& env, TranslatedShader& out,
               std::string& error);

} // namespace cafe::latte
