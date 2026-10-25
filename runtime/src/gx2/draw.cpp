// GX2 draws, vertex buffers and clears.

#include "gx2/internal.h"

#include "cafe/export.h"
#include "cafe/runtime.h"

namespace cafe::gx2 {
namespace {

using namespace latte;

void GX2SetAttribBuffer(uint32_t index, uint32_t size, uint32_t stride, uint32_t buffer) {
    ApiLock lock;
    const uint32_t words[resource::kWords] = {
        buffer, size - 1, field(stride, 8, 11), 0, 0, 0, resource::kTypeValidBuffer,
    };
    set_resource(resource::kVsAttrib + index, words);
}

// VGT_DMA_INDEX_TYPE for a GX2IndexType: index size and the byte swap that
// turns the title's big-endian indices into the GPU's order.
uint32_t index_type_register(uint32_t type) {
    switch (type) {
    case kIndexU16: return field(0, 0, 2) | field(1, 2, 2);
    case kIndexU16Le: return field(0, 0, 2);
    case kIndexU32: return field(1, 0, 2) | field(2, 2, 2);
    case kIndexU32Le: return field(1, 0, 2);
    default: fatal("GX2: invalid index type %u", type);
    }
}

void draw_setup(uint32_t mode, uint32_t base_vertex) {
    set_ctl_const(reg::SQ_VTX_BASE_VTX_LOC, base_vertex);
    set_config_reg(reg::VGT_PRIMITIVE_TYPE, mode);
}

void GX2DrawEx(uint32_t mode, uint32_t count, uint32_t base_vertex, uint32_t instances) {
    ApiLock lock;
    draw_setup(mode, base_vertex);
    write_packet(pm4::kNumInstances, {instances});
    write_packet(pm4::kDrawIndexAuto, {count, field(2, 0, 2)}); // SOURCE_SELECT auto index
}

void GX2DrawEx2(uint32_t mode, uint32_t count, uint32_t base_vertex, uint32_t instances, uint32_t base_instance) {
    ApiLock lock;
    set_ctl_const(reg::SQ_VTX_START_INST_LOC, base_instance);
    GX2DrawEx(mode, count, base_vertex, instances);
    set_ctl_const(reg::SQ_VTX_START_INST_LOC, 0);
}

void GX2DrawIndexedEx(uint32_t mode, uint32_t count, uint32_t type, uint32_t indices, uint32_t base_vertex,
                      uint32_t instances) {
    ApiLock lock;
    draw_indexed(mode, count, type, indices, base_vertex, instances);
}

void GX2DrawIndexedEx2(uint32_t mode, uint32_t count, uint32_t type, uint32_t indices, uint32_t base_vertex,
                       uint32_t instances, uint32_t base_instance) {
    ApiLock lock;
    set_ctl_const(reg::SQ_VTX_START_INST_LOC, base_instance);
    GX2DrawIndexedEx(mode, count, type, indices, base_vertex, instances);
    set_ctl_const(reg::SQ_VTX_START_INST_LOC, 0);
}

void GX2DrawIndexedImmediateEx(uint32_t mode, uint32_t count, uint32_t type, uint32_t indices, uint32_t base_vertex,
                               uint32_t instances) {
    ApiLock lock;
    draw_setup(mode, base_vertex);
    write_packet(pm4::kIndexType, {index_type_register(type)});
    write_packet(pm4::kNumInstances, {instances});
    const bool wide = type == kIndexU32 || type == kIndexU32Le;
    const uint32_t words = wide ? count : (count + 1) / 2;
    std::vector<uint32_t> packet{pm4::type3(pm4::kDrawIndexImmd, words + 2), count, field(1, 0, 2)};
    const be<uint32_t>* source = guest<be<uint32_t>>(indices);
    for (uint32_t i = 0; i < words; ++i) {
        uint32_t value = source[i];
        // Packed 16-bit indices: the GPU reads the low half first.
        if (type == kIndexU16) value = (value >> 16) | (value << 16);
        packet.push_back(value);
    }
    write(packet);
}

void GX2SetPrimitiveRestartIndex(uint32_t index) {
    ApiLock lock;
    set_context_reg(reg::VGT_MULTI_PRIM_IB_RESET_INDX, index);
}

// Clears run on the console as draws with GX2-internal shaders; here they
// are single commands the backend performs.
void GX2ClearColor(ColorBuffer* buffer, float r, float g, float b, float a) {
    ApiLock lock;
    HlePacket(pm4::kHleClearColor).f32(r).f32(g).f32(b).f32(a).guest_struct(*buffer).write();
}

void GX2ClearDepthStencilEx(DepthBuffer* buffer, float depth, uint8_t stencil, uint32_t flags) {
    ApiLock lock;
    HlePacket(pm4::kHleClearDepthStencil).u32(flags).f32(depth).u32(stencil).guest_struct(*buffer).write();
}

void GX2ClearDepthStencil(DepthBuffer* buffer, uint32_t flags) {
    GX2ClearDepthStencilEx(buffer, buffer->depth_clear, static_cast<uint8_t>(buffer->stencil_clear), flags);
}

void GX2ClearBuffersEx(ColorBuffer* color, DepthBuffer* depth, float r, float g, float b, float a, float depth_value,
                       uint8_t stencil, uint32_t flags) {
    GX2ClearColor(color, r, g, b, a);
    GX2ClearDepthStencilEx(depth, depth_value, stencil, flags);
}

} // namespace

void draw_indexed(uint32_t mode, uint32_t count, uint32_t type, uint32_t indices, uint32_t base_vertex,
                  uint32_t instances) {
    draw_setup(mode, base_vertex);
    write_packet(pm4::kIndexType, {index_type_register(type)});
    write_packet(pm4::kNumInstances, {instances});
    // SOURCE_SELECT DMA; tessellated primitive types use MAJOR_MODE 1.
    const uint32_t initiator = field(0, 0, 2) | field((mode & 0x80) != 0, 2, 2);
    write_packet(pm4::kDrawIndex2, {0xFFFFFFFF, indices, 0, count, initiator});
}

CAFE_EXPORT(gx2, GX2SetAttribBuffer, GX2SetAttribBuffer);
CAFE_EXPORT(gx2, GX2DrawEx, GX2DrawEx);
CAFE_EXPORT(gx2, GX2DrawEx2, GX2DrawEx2);
CAFE_EXPORT(gx2, GX2DrawIndexedEx, GX2DrawIndexedEx);
CAFE_EXPORT(gx2, GX2DrawIndexedEx2, GX2DrawIndexedEx2);
CAFE_EXPORT(gx2, GX2DrawIndexedImmediateEx, GX2DrawIndexedImmediateEx);
CAFE_EXPORT(gx2, GX2SetPrimitiveRestartIndex, GX2SetPrimitiveRestartIndex);
CAFE_EXPORT(gx2, GX2ClearColor, GX2ClearColor);
CAFE_EXPORT(gx2, GX2ClearDepthStencilEx, GX2ClearDepthStencilEx);
CAFE_EXPORT(gx2, GX2ClearDepthStencil, GX2ClearDepthStencil);
CAFE_EXPORT(gx2, GX2ClearBuffersEx, GX2ClearBuffersEx);

} // namespace cafe::gx2
