#pragma once

// The interface between translated Latte shaders and the renderer that
// runs them: descriptor bindings and the per-draw constant block.
//
// Translated shaders read guest memory directly through buffer device
// addresses (uniform blocks, vertex buffers, buffer fetches) and write it
// for stream-out. A buffer is described by one uvec4: the device address
// of its first byte (low word, high word), its size in bytes, and its
// stride. Shaders bound every access by the size; a size of 0 reads zeros.
// The low two bits of the address are the misalignment of the first byte:
// the shader rounds the address down and adds them to its offsets.

#include <cstdint>

namespace cafe::latte::abi {

// Descriptor set 0, pushed per draw.
constexpr uint32_t kDrawConstantsBinding = 0; // uniform DrawConstants
constexpr uint32_t kVsRegistersBinding = 1;   // uniform uvec4[256]: VS uniform registers
constexpr uint32_t kPsRegistersBinding = 2;   // uniform uvec4[256]: PS uniform registers
constexpr uint32_t kVsTextureBinding = 8;     // + texture slot, 18 slots
constexpr uint32_t kPsTextureBinding = 32;    // + texture slot, 18 slots
constexpr uint32_t kTextureSlots = 18;

struct Buffer {
    uint32_t address_lo; // bits 0-1: misalignment of the first byte
    uint32_t address_hi;
    uint32_t size;
    uint32_t stride;
};

// std140; every member is a multiple of 16 bytes.
struct DrawConstants {
    Buffer vs_cb[16];   // constant cache banks: uniform blocks (SQ_ALU_CONST_CACHE_VS_n)
    Buffer ps_cb[16];
    Buffer vs_buf[16];  // buffer resources read by VTX_FETCH in TEX clauses
    Buffer ps_buf[16];
    Buffer vb[16];      // vertex attribute buffers (fetch shader buffer ids 160-175)
    Buffer so[4];       // stream-out buffers, from the current write offset
    uint32_t base_vertex;     // SQ_VTX_BASE_VTX_LOC
    uint32_t start_instance;  // SQ_VTX_START_INST_LOC
    uint32_t step_rate[2];    // VGT_INSTANCE_STEP_RATE_0/1
    float alpha_ref;          // SX_ALPHA_REF
    float point_size;         // PA_SU_POINT_SIZE, in pixels
    // Upscaling: render targets the size of the screen (and its halvings)
    // are drawn at `texture_scale` times their size. A pass into them has
    // pixel_scale = texture_scale, others 1; the title's pixel coordinates
    // are gl_FragCoord / pixel_scale.
    float pixel_scale;
    float texture_scale;
    uint32_t vs_scaled_textures; // texture slots holding upscaled images
    uint32_t ps_scaled_textures;
    uint32_t pad[2];
};
static_assert(sizeof(DrawConstants) % 16 == 0);

} // namespace cafe::latte::abi
