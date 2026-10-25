// GX2 shaders: fetch shader generation, binding vertex/pixel/geometry
// shaders, uniforms, the shader mode and stream-out.
//
// Shader programs are Latte microcode the title ships (compiled offline);
// GX2 only points the GPU at them. Fetch shaders, which load vertex
// attributes for the vertex shader, are generated here from the title's
// attribute streams, as GX2 does on the console. Microcode is stored as
// little-endian dwords, the GPU's byte order.

#include "gx2/internal.h"

#include "cafe/export.h"
#include "cafe/runtime.h"

#include <cstring>

namespace cafe::gx2 {

using namespace latte;

namespace {

constexpr uint32_t kFetchesPerClause = 16;
constexpr uint32_t kVertexFetchBytes = 16;
constexpr uint32_t kControlFlowBytes = 8;
constexpr uint32_t kAluBytes = 8;

// Fetch shader types: none, line, triangle, quad tessellation.
uint32_t fetches_per_attrib(uint32_t type) { return type == 0 ? 1 : type + 1; }
uint32_t fetch_count(uint32_t attribs, uint32_t type) {
    return type == 0 ? attribs : fetches_per_attrib(type) * (attribs - 2);
}
uint32_t alu_count(uint32_t type, uint32_t tess_mode) {
    if (type == 0) return 0;
    if (tess_mode == 2) { // adaptive
        static constexpr uint32_t kAdaptive[4] = {4, 11, 57, 43};
        return kAdaptive[type & 3] + 4;
    }
    return 4 + 4;
}
uint32_t cf_count(uint32_t fetches, uint32_t type) {
    return (fetches + kFetchesPerClause - 1) / kFetchesPerClause + (type != 0 ? 2 : 0) + 1;
}

uint32_t calc_fetch_shader_size(uint32_t attribs, uint32_t type, uint32_t tess_mode) {
    const uint32_t fetches = fetch_count(attribs, type);
    const uint32_t cf_and_alu = kControlFlowBytes * cf_count(fetches, type) + kAluBytes * alu_count(type, tess_mode);
    return kVertexFetchBytes * fetches + ((cf_and_alu + 15) & ~15u);
}

void store_le32(uint32_t address, uint32_t value) {
    uint8_t* p = guest_pointer(address);
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t GX2CalcFetchShaderSizeEx(uint32_t attribs, uint32_t type, uint32_t tess_mode) {
    return calc_fetch_shader_size(attribs, type, tess_mode);
}

// A VTX_TC clause per 16 semantic vertex fetches, then RETURN. Each fetch
// loads one attribute stream into the vertex shader input with the
// attribute's semantic id (its location).
void GX2InitFetchShaderEx(FetchShader* shader, uint32_t buffer, uint32_t attrib_count, AttribStream* attribs,
                          uint32_t type, uint32_t tess_mode) {
    if (type != 0 || tess_mode != 0) {
        fatal("GX2InitFetchShaderEx: tessellation fetch shaders (type %u, mode %u) are not implemented", type,
              tess_mode);
    }
    const uint32_t fetches = fetch_count(attrib_count, type);
    const uint32_t cfs = cf_count(fetches, type);
    const uint32_t fetch_offset = (cfs * kControlFlowBytes + 15) & ~15u;
    const uint32_t size = calc_fetch_shader_size(attrib_count, type, tess_mode);

    shader->type = type;
    shader->attrib_count = attrib_count;
    shader->data = buffer;
    shader->size = size;
    shader->num_divisors = 0;
    shader->divisors[0] = 0;
    shader->divisors[1] = 0;
    std::memset(guest_pointer(buffer), 0, size);

    uint32_t out = buffer + fetch_offset;
    for (uint32_t i = 0; i < attrib_count; ++i) {
        const AttribStream& a = attribs[i];
        if (a.buffer == 16u) continue; // tessellation parameters, no fetch
        const uint32_t format = a.format;
        uint32_t word0 = field(1, 0, 5) /* VTX_INST SEMANTIC */ |
                         field(resource::kVsAttrib + a.buffer - resource::kVsTexture, 8, 8) |
                         field(attrib_format_bits(format) / 8 - 1, 26, 6); // MEGA_FETCH_COUNT
        if (a.type != 0u) {
            // Per instance: the index source is the instance id, divided by
            // one of the step rates in VGT_INSTANCE_STEP_RATE_0/1.
            uint32_t select = 0;
            const uint32_t divisor = a.alu_divisor;
            if (divisor == 1) {
                select = 3; // W: instance id
            } else if (shader->num_divisors > 0 && divisor == shader->divisors[0]) {
                select = 1;
            } else if (shader->num_divisors > 1 && divisor == shader->divisors[1]) {
                select = 2;
            } else {
                if (shader->num_divisors >= 2) fatal("GX2InitFetchShaderEx: more than two instance step rates");
                shader->divisors[shader->num_divisors] = divisor;
                select = shader->num_divisors == 0 ? 1 : 2;
                shader->num_divisors = shader->num_divisors + 1;
            }
            word0 |= field(1, 5, 2) /* INSTANCE_DATA */ | field(select, 24, 2);
        }
        uint32_t num_format = 0; // NORM
        if (format & attrib_format::kScaled) num_format = 2;
        else if (format & attrib_format::kInteger) num_format = 1;
        const uint32_t mask = a.mask;
        const uint32_t word1 = field(a.location, 0, 8) | field(mask >> 24, 9, 3) | field(mask >> 16, 12, 3) |
                               field(mask >> 8, 15, 3) | field(mask, 18, 3) |
                               field(attrib_format_data_format(format), 22, 6) | field(num_format, 28, 2) |
                               field((format & attrib_format::kSigned) != 0, 30, 1);
        const uint32_t swap = a.endian_swap == uint32_t{kSwapDefault} ? attrib_format_endian(format)
                                                                        : swap_mode_endian(a.endian_swap);
        const uint32_t word2 = field(a.offset, 0, 16) | field(swap, 16, 2) | field(1, 19, 1); // MEGA_FETCH
        store_le32(out + 0, word0);
        store_le32(out + 4, word1);
        store_le32(out + 8, word2);
        store_le32(out + 12, 0);
        out += kVertexFetchBytes;
    }

    uint32_t cf = buffer;
    for (uint32_t clause = 0; clause + 1 < cfs; ++clause) {
        const uint32_t first = clause * kFetchesPerClause;
        const uint32_t count = std::min(kFetchesPerClause, fetches - first);
        store_le32(cf, (fetch_offset + first * kVertexFetchBytes) / 8);
        store_le32(cf + 4, field((count - 1) & 7, 10, 3) | field((count - 1) >> 3, 19, 1) |
                               field(3, 23, 7) /* VTX_TC */);
        cf += kControlFlowBytes;
    }
    store_le32(cf, 0);
    store_le32(cf + 4, field(0x14, 23, 7) /* RETURN */ | field(1, 31, 1) /* BARRIER */);
    shader->sq_pgm_resources_fs = 0; // NUM_GPRS 0 without tessellation
}

void GX2SetFetchShader(FetchShader* shader) {
    ApiLock lock;
    const uint32_t program[] = {uint32_t{shader->data} >> 8, uint32_t{shader->size} >> 3, 0x100000, 0x100000,
                                shader->sq_pgm_resources_fs};
    set_context_regs(reg::SQ_PGM_START_FS, program);
    const uint32_t step_rates[] = {shader->divisors[0], shader->divisors[1]};
    set_context_regs(reg::VGT_INSTANCE_STEP_RATE_0, step_rates);
}

// A shader's program: its own pointer, or its GX2R buffer.
void program_of(uint32_t data, uint32_t size, const RBuffer& gx2r, uint32_t& address, uint32_t& bytes) {
    address = data;
    bytes = size;
    if (address == 0) {
        address = gx2r.buffer;
        bytes = gx2r.elem_count * gx2r.elem_size;
    }
    if (address == 0 || bytes == 0) fatal("GX2: a shader has no program");
}

void set_loop_vars(uint32_t base, uint32_t count, uint32_t vars) {
    for (uint32_t i = 0; i < count; ++i) {
        const LoopVar& var = guest<LoopVar>(vars)[i];
        set_loop_const(base + var.offset, var.value);
    }
}

void GX2SetVertexShader(VertexShader* shader) {
    ApiLock lock;
    uint32_t address, bytes;
    program_of(shader->data, shader->size, shader->gx2r_data, address, bytes);
    const uint32_t program[] = {address >> 8, bytes >> 3, 0x100000, 0x100000, shader->sq_pgm_resources_vs};
    if (shader->mode != uint32_t{kModeGeometry}) {
        set_context_regs(reg::SQ_PGM_START_VS, program);
        set_context_reg(reg::VGT_PRIMITIVEID_EN, shader->vgt_primitiveid_en);
        set_context_reg(reg::SPI_VS_OUT_CONFIG, shader->spi_vs_out_config);
        set_context_reg(reg::PA_CL_VS_OUT_CNTL, shader->pa_cl_vs_out_cntl);
        const uint32_t outputs = std::min<uint32_t>(shader->num_spi_vs_out_id, 10);
        if (outputs > 0) {
            uint32_t ids[10];
            for (uint32_t i = 0; i < outputs; ++i) ids[i] = shader->spi_vs_out_id[i];
            set_context_regs(reg::SPI_VS_OUT_ID_0, {ids, outputs});
        }
        set_context_reg(reg::SQ_PGM_CF_OFFSET_VS, 0);
        if (shader->has_stream_out) {
            for (uint32_t i = 0; i < 4; ++i) {
                set_context_reg(reg::VGT_STRMOUT_VTX_STRIDE_0 + i * 16, shader->stream_out_stride[i] >> 2);
            }
        }
        set_context_reg(reg::VGT_STRMOUT_BUFFER_EN, shader->vgt_strmout_buffer_en);
    } else {
        set_context_regs(reg::SQ_PGM_START_ES, program);
        set_context_reg(reg::SQ_ESGS_RING_ITEMSIZE, shader->ring_item_size);
    }
    set_context_reg(reg::SQ_VTX_SEMANTIC_CLEAR, shader->sq_vtx_semantic_clear);
    const uint32_t semantics = std::min<uint32_t>(shader->num_sq_vtx_semantic, 32);
    if (semantics > 0) {
        uint32_t ids[32];
        for (uint32_t i = 0; i < semantics; ++i) ids[i] = shader->sq_vtx_semantic[i];
        set_context_regs(reg::SQ_VTX_SEMANTIC_0, {ids, semantics});
    }
    set_context_reg(reg::VGT_VERTEX_REUSE_BLOCK_CNTL, shader->vgt_vertex_reuse_block_cntl);
    set_context_reg(reg::VGT_HOS_REUSE_DEPTH, shader->vgt_hos_reuse_depth);
    set_loop_vars(reg::SQ_LOOP_CONST_VS_0, shader->loop_var_count, shader->loop_vars);
}

// In geometry mode the vertex shader runs as the export shader (ES, set by
// GX2SetVertexShader), the geometry shader as GS, and the GS's copy shader
// in the VS slot, moving GS output from the ring to the rasterizer.
void GX2SetGeometryShader(GeometryShader* shader) {
    ApiLock lock;
    uint32_t address, bytes, copy_address, copy_bytes;
    program_of(shader->data, shader->size, shader->gx2r_data, address, bytes);
    program_of(shader->copy_data, shader->copy_size, shader->gx2r_copy_data, copy_address, copy_bytes);
    const uint32_t program[] = {address >> 8, bytes >> 3, 0x100000, 0x100000, shader->sq_pgm_resources_gs};
    set_context_regs(reg::SQ_PGM_START_GS, program);
    const uint32_t copy[] = {copy_address >> 8, copy_bytes >> 3, 0x100000, 0x100000, shader->sq_pgm_resources_vs};
    set_context_regs(reg::SQ_PGM_START_VS, copy);
    set_context_reg(reg::SQ_PGM_CF_OFFSET_GS, 0);
    set_context_reg(reg::SQ_PGM_CF_OFFSET_VS, 0);
    set_context_reg(reg::VGT_GS_OUT_PRIM_TYPE, shader->vgt_gs_out_prim_type);
    set_context_reg(reg::VGT_GS_MODE, shader->vgt_gs_mode);
    set_context_reg(reg::SQ_GS_VERT_ITEMSIZE, shader->sq_gs_vert_itemsize);
    set_context_reg(reg::SQ_GSVS_RING_ITEMSIZE, shader->ring_item_size & 0x7FFF);
    set_context_reg(reg::PA_CL_VS_OUT_CNTL, shader->pa_cl_vs_out_cntl);
    set_context_reg(reg::SPI_VS_OUT_CONFIG, shader->spi_vs_out_config);
    const uint32_t outputs = std::min<uint32_t>(shader->num_spi_vs_out_id, 10);
    if (outputs > 0) {
        uint32_t ids[10];
        for (uint32_t i = 0; i < outputs; ++i) ids[i] = shader->spi_vs_out_id[i];
        set_context_regs(reg::SPI_VS_OUT_ID_0, {ids, outputs});
    }
    if (shader->has_stream_out) {
        for (uint32_t i = 0; i < 4; ++i) {
            set_context_reg(reg::VGT_STRMOUT_VTX_STRIDE_0 + i * 16, shader->stream_out_stride[i] >> 2);
        }
    }
    set_context_reg(reg::VGT_STRMOUT_BUFFER_EN, shader->vgt_strmout_buffer_en);
    set_loop_vars(reg::SQ_LOOP_CONST_GS_0, shader->loop_var_count, shader->loop_vars);
}

// The rings between the stages: ES->GS (input) and GS->VS (output).
void GX2SetGeometryShaderInputRingBuffer(uint32_t buffer, uint32_t size) {
    ApiLock lock;
    const uint32_t ring[] = {buffer >> 8, size >> 8};
    set_config_regs(reg::SQ_ESGS_RING_BASE, ring);
}
void GX2SetGeometryShaderOutputRingBuffer(uint32_t buffer, uint32_t size) {
    ApiLock lock;
    const uint32_t ring[] = {buffer >> 8, size >> 8};
    set_config_regs(reg::SQ_GSVS_RING_BASE, ring);
}

void GX2SetPixelShader(PixelShader* shader) {
    ApiLock lock;
    uint32_t address, bytes;
    program_of(shader->data, shader->size, shader->gx2r_data, address, bytes);
    const uint32_t program[] = {address >> 8, bytes >> 3, 0x100000, 0x100000, shader->sq_pgm_resources_ps,
                                shader->sq_pgm_exports_ps};
    set_context_regs(reg::SQ_PGM_START_PS, program);
    const uint32_t in_control[] = {shader->spi_ps_in_control_0, shader->spi_ps_in_control_1};
    set_context_regs(reg::SPI_PS_IN_CONTROL_0, in_control);
    const uint32_t inputs = std::min<uint32_t>(shader->num_spi_ps_input_cntl, 32);
    if (inputs > 0) {
        uint32_t cntl[32];
        for (uint32_t i = 0; i < inputs; ++i) cntl[i] = shader->spi_ps_input_cntl[i];
        set_context_regs(reg::SPI_PS_INPUT_CNTL_0, {cntl, inputs});
    }
    set_context_reg(reg::CB_SHADER_MASK, shader->cb_shader_mask);
    set_context_reg(reg::CB_SHADER_CONTROL, shader->cb_shader_control);
    set_context_reg(reg::DB_SHADER_CONTROL, shader->db_shader_control | (1u << 9)); // DUAL_EXPORT_ENABLE
    set_context_reg(reg::SPI_INPUT_Z, shader->spi_input_z);
    set_loop_vars(reg::SQ_LOOP_CONST_PS_0, shader->loop_var_count, shader->loop_vars);
}

// Uniform blocks are buffer resources the shader reads through the
// constant cache.
void set_uniform_block(uint32_t resource_base, uint32_t cache_reg, uint32_t size_reg, uint32_t location,
                       uint32_t size, uint32_t data) {
    if (data & 0xFF) fatal("GX2: uniform block at 0x%08X is not 256-byte aligned", data);
    const uint32_t words[resource::kWords] = {
        data,
        size - 1,
        field(16, 8, 11) /* STRIDE */ | field(fmt::k32_32_32_32, 20, 6) | field(1, 28, 1) /* SIGNED */,
        field(1, 0, 2), // MEM_REQUEST_SIZE
        0,
        0,
        resource::kTypeValidBuffer,
    };
    set_resource(resource_base + location, words);
    set_context_reg(cache_reg + location * 4, data >> 8);
    set_context_reg(size_reg + location * 4, ((size + 255) >> 8) & 0x1FF);
}

void GX2SetVertexUniformBlock(uint32_t location, uint32_t size, uint32_t data) {
    ApiLock lock;
    set_uniform_block(resource::kVsBuffer, reg::SQ_ALU_CONST_CACHE_VS_0, reg::SQ_ALU_CONST_BUFFER_SIZE_VS_0, location,
                      size, data);
}
void GX2SetPixelUniformBlock(uint32_t location, uint32_t size, uint32_t data) {
    ApiLock lock;
    set_uniform_block(resource::kPsBuffer, reg::SQ_ALU_CONST_CACHE_PS_0, reg::SQ_ALU_CONST_BUFFER_SIZE_PS_0, location,
                      size, data);
}
void GX2SetGeometryUniformBlock(uint32_t location, uint32_t size, uint32_t data) {
    ApiLock lock;
    set_uniform_block(resource::kGsBuffer, reg::SQ_ALU_CONST_CACHE_GS_0, reg::SQ_ALU_CONST_BUFFER_SIZE_GS_0, location,
                      size, data);
}

void set_uniform_regs(uint32_t alu_base, uint32_t loop_base, uint32_t offset, uint32_t count, be<uint32_t>* data) {
    if (const uint32_t loop = offset >> 16) {
        set_loop_const(loop_base + 4 * loop, data[0]);
        offset &= 0x7FFF;
    }
    std::vector<uint32_t> packet{pm4::type3(pm4::kSetAluConst, count + 1), (alu_base - kAluConstBase) / 4 + offset};
    for (uint32_t i = 0; i < count; ++i) packet.push_back(data[i]);
    write(packet);
}
void GX2SetVertexUniformReg(uint32_t offset, uint32_t count, be<uint32_t>* data) {
    ApiLock lock;
    set_uniform_regs(reg::SQ_ALU_CONSTANT0_256, reg::SQ_LOOP_CONST_VS_0, offset, count, data);
}
void GX2SetPixelUniformReg(uint32_t offset, uint32_t count, be<uint32_t>* data) {
    ApiLock lock;
    set_uniform_regs(reg::SQ_ALU_CONSTANT0_0, reg::SQ_LOOP_CONST_PS_0, offset, count, data);
}

void GX2SetShaderModeEx(uint32_t mode, uint32_t vs_gprs, uint32_t vs_stack, uint32_t gs_gprs, uint32_t gs_stack,
                        uint32_t ps_gprs, uint32_t ps_stack) {
    ApiLock lock;
    set_shader_mode(mode, vs_gprs, vs_stack, gs_gprs, gs_stack, ps_gprs, ps_stack);
}

// ------------------------------------------------------------- stream out
void GX2SetStreamOutBuffer(uint32_t index, OutputStream* stream) {
    ApiLock lock;
    set_stream_out_buffer(index, *stream);
}

void GX2SetStreamOutEnable(bool enable) {
    ApiLock lock;
    set_stream_out_enable(enable);
}

// Where the GPU continues writing stream `index`: after what an earlier
// pass stored in the stream's context (append), at the start, or at an
// explicit offset passed in place of the stream pointer.
void GX2SetStreamOutContext(uint32_t index, uint32_t stream, uint32_t mode) {
    ApiLock lock;
    uint32_t control = field(index, 8, 2);
    uint32_t source = 0;
    switch (mode) {
    case 0: // append
        control |= field(2, 1, 2); // offset from memory
        source = guest<OutputStream>(stream)->context;
        break;
    case 1: break; // from the start: offset 0 from the packet
    case 2: source = stream; break;
    default: fatal("GX2SetStreamOutContext: invalid mode %u", mode);
    }
    write_packet(pm4::kStrmoutBufferUpdate, {control, 0, 0, source, 0});
}

void GX2SaveStreamOutContext(uint32_t index, OutputStream* stream) {
    ApiLock lock;
    const uint32_t control = field(1, 0, 1) /* store filled size */ | field(3, 1, 2) /* no offset update */ |
                             field(index, 8, 2);
    write_packet(pm4::kStrmoutBufferUpdate, {control, stream->context, 0, 0, 0});
}

uint32_t GX2CalcGeometryShaderInputRingBufferSize(uint32_t item_size) { return item_size * 16384; }
uint32_t GX2CalcGeometryShaderOutputRingBufferSize(uint32_t item_size) { return item_size * 16384; }

} // namespace

void set_shader_mode(uint32_t mode, uint32_t vs_gprs, uint32_t vs_stack, uint32_t gs_gprs, uint32_t gs_stack,
                     uint32_t ps_gprs, uint32_t ps_stack) {
    if (mode != kModeGeometry) {
        // VGT_GS_MODE: off, or the compute scenario.
        const uint32_t gs_mode = mode == kModeCompute
                                     ? field(3, 0, 2) | field(1, 14, 1) | field(1, 15, 1) | field(1, 17, 1)
                                     : 0;
        set_context_reg(reg::VGT_GS_MODE, gs_mode);
    }
    // SQ_CONFIG: prefer vector ALU slots, stage priorities, DX9 constant
    // mode for uniform registers.
    uint32_t sq_config = field(1, 3, 1);
    sq_config |= mode == kModeCompute ? field(0, 24, 2) | field(1, 26, 2) | field(2, 28, 2) | field(3, 30, 2)
                                      : field(3, 24, 2) | field(2, 26, 2) | field(1, 28, 2) | field(0, 30, 2);
    if (mode == kModeUniformRegister) sq_config |= field(1, 2, 1);
    uint32_t gpr1 = 0, gpr2 = 0, threads = 0, stack1 = 0, stack2 = 0;
    if (mode == kModeGeometry) {
        gpr1 = field(ps_gprs, 0, 8) | field(64, 16, 8) | field(4, 28, 4);
        gpr2 = field(gs_gprs, 0, 8) | field(vs_gprs, 16, 8);
        stack1 = field(ps_stack, 0, 12);
        stack2 = field(gs_stack, 0, 12) | field(vs_stack, 16, 12);
        threads = field(124, 0, 8) | field(32, 8, 8) | field(8, 16, 8) | field(28, 24, 8);
    } else if (mode == kModeCompute) {
        gpr1 = field(4, 28, 4);
        gpr2 = field(248, 16, 8);
        stack2 = field(256, 16, 12);
        threads = field(1, 0, 8) | field(1, 8, 8) | field(1, 16, 8) | field(189, 24, 8);
    } else {
        gpr1 = field(ps_gprs, 0, 8) | field(vs_gprs, 16, 8);
        stack1 = field(ps_stack, 0, 12) | field(vs_stack, 16, 12);
        threads = field(136, 0, 8) | field(48, 8, 8) | field(4, 16, 8) | field(4, 24, 8);
    }
    const uint32_t values[] = {sq_config, gpr1, gpr2, threads, stack1, stack2};
    set_config_regs(reg::SQ_CONFIG, values);
    if (mode == kModeCompute) {
        const uint32_t rings[] = {0, 0xFFFFFF, 0, 0xFFFFFF};
        set_config_regs(reg::SQ_ESGS_RING_BASE, rings);
        const uint32_t item_sizes[] = {0, 1};
        set_context_regs(reg::SQ_ESGS_RING_ITEMSIZE, item_sizes);
        set_context_reg(reg::VGT_STRMOUT_EN, 0);
    }
}

void set_stream_out_enable(bool enable) { set_context_reg(reg::VGT_STRMOUT_EN, enable ? 1 : 0); }

void set_stream_out_buffer(uint32_t index, const OutputStream& stream) {
    uint32_t address = stream.buffer;
    uint32_t size = stream.size;
    if (address == 0) {
        address = stream.gx2r_data.buffer;
        size = stream.gx2r_data.elem_count * stream.gx2r_data.elem_size;
    }
    set_context_reg(reg::VGT_STRMOUT_BUFFER_SIZE_0 + 16 * index, size >> 2);
    set_context_reg(reg::VGT_STRMOUT_BUFFER_BASE_0 + 16 * index, address >> 8);
    write_packet(pm4::kStrmoutBaseUpdate, {index, address >> 8});
}

CAFE_EXPORT(gx2, GX2CalcFetchShaderSizeEx, GX2CalcFetchShaderSizeEx);
CAFE_EXPORT(gx2, GX2InitFetchShaderEx, GX2InitFetchShaderEx);
CAFE_EXPORT(gx2, GX2SetFetchShader, GX2SetFetchShader);
CAFE_EXPORT(gx2, GX2SetVertexShader, GX2SetVertexShader);
CAFE_EXPORT(gx2, GX2SetPixelShader, GX2SetPixelShader);
CAFE_EXPORT(gx2, GX2SetGeometryShader, GX2SetGeometryShader);
CAFE_EXPORT(gx2, GX2SetGeometryShaderInputRingBuffer, GX2SetGeometryShaderInputRingBuffer);
CAFE_EXPORT(gx2, GX2SetGeometryShaderOutputRingBuffer, GX2SetGeometryShaderOutputRingBuffer);
CAFE_EXPORT(gx2, GX2SetVertexUniformBlock, GX2SetVertexUniformBlock);
CAFE_EXPORT(gx2, GX2SetPixelUniformBlock, GX2SetPixelUniformBlock);
CAFE_EXPORT(gx2, GX2SetGeometryUniformBlock, GX2SetGeometryUniformBlock);
CAFE_EXPORT(gx2, GX2SetVertexUniformReg, GX2SetVertexUniformReg);
CAFE_EXPORT(gx2, GX2SetPixelUniformReg, GX2SetPixelUniformReg);
CAFE_EXPORT(gx2, GX2SetShaderModeEx, GX2SetShaderModeEx);
CAFE_EXPORT(gx2, GX2SetStreamOutBuffer, GX2SetStreamOutBuffer);
CAFE_EXPORT(gx2, GX2SetStreamOutEnable, GX2SetStreamOutEnable);
CAFE_EXPORT(gx2, GX2SetStreamOutContext, GX2SetStreamOutContext);
CAFE_EXPORT(gx2, GX2SaveStreamOutContext, GX2SaveStreamOutContext);
CAFE_EXPORT(gx2, GX2CalcGeometryShaderInputRingBufferSize, GX2CalcGeometryShaderInputRingBufferSize);
CAFE_EXPORT(gx2, GX2CalcGeometryShaderOutputRingBufferSize, GX2CalcGeometryShaderOutputRingBufferSize);

} // namespace cafe::gx2
