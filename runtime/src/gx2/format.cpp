// GX2 surface and vertex attribute formats: element sizes and the hardware
// encodings GX2 derives from them.

#include "gx2/internal.h"

#include "cafe/export.h"
#include "cafe/runtime.h"

namespace cafe::gx2 {
namespace {

using namespace latte;

// Bits per element by hardware format (compressed formats: per 4x4 block).
constexpr uint8_t kBitsPerElement[64] = {
    0,  8,  8,  0,  0,  16, 16, 16, 16, 16, 16, 16, 16, 32,  32,  32, //
    32, 32, 0,  32, 0,  0,  32, 0,  0,  32, 32, 32, 64, 64,  64,  64, //
    64, 0,  128, 128, 0, 0,  0,  16, 16, 32, 32, 32, 0,  0,  0,   96, //
    96, 64, 128, 128, 64, 128, 0, 0,  0,  0,  0,  0,  0,  0,  0,   0,
};

// Whether shader exports to a color buffer of this format can use the
// 16-bit normalised path (CB_SOURCE_FORMAT EXPORT_NORM) rather than full
// precision.
constexpr uint8_t kExportNorm[64] = {
    1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, //
    1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, //
    1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, //
    0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

} // namespace

uint32_t surface_format_bits_per_element(uint32_t format) { return kBitsPerElement[format & 0x3F]; }
uint32_t surface_format_bytes_per_element(uint32_t format) { return kBitsPerElement[format & 0x3F] / 8; }

uint32_t color_buffer_format(uint32_t format) {
    // CB_FORMAT shares SQ_DATA_FORMAT's numbering for every renderable format.
    const uint32_t hw = format & 0x3F;
    switch (hw) {
    case fmt::k8: case fmt::k4_4: case fmt::k3_3_2: case fmt::k16: case fmt::k16Float: case fmt::k8_8:
    case fmt::k5_6_5: case fmt::k6_5_5: case fmt::k1_5_5_5: case fmt::k4_4_4_4: case fmt::k5_5_5_1:
    case fmt::k32: case fmt::k32Float: case fmt::k16_16: case fmt::k16_16Float: case fmt::k8_24:
    case fmt::k8_24Float: case fmt::k24_8: case fmt::k24_8Float: case fmt::k10_11_11:
    case fmt::k10_11_11Float: case fmt::k11_11_10: case fmt::k11_11_10Float: case fmt::k2_10_10_10:
    case fmt::k8_8_8_8: case fmt::k10_10_10_2: case fmt::kX24_8_32Float: case fmt::k32_32:
    case fmt::k32_32Float: case fmt::k16_16_16_16: case fmt::k16_16_16_16Float: case fmt::k32_32_32_32:
    case fmt::k32_32_32_32Float:
        return hw;
    default:
        return 0; // COLOR_INVALID
    }
}

uint32_t color_buffer_number_type(uint32_t format) {
    switch (surface_format::type(format)) {
    case surface_format::kTypeUnorm: return 0;
    case surface_format::kTypeSnorm: return 1;
    case surface_format::kTypeUint: return 4;
    case surface_format::kTypeSint: return 5;
    case surface_format::kTypeSrgb: return 6;
    case surface_format::kTypeFloat: return 7;
    default: return 0;
    }
}

uint32_t color_buffer_source_format(uint32_t format) { return kExportNorm[format & 0x3F]; }

uint32_t attrib_format_bits(uint32_t format) {
    switch (format & 0x1F) {
    case 0x00: case 0x01: return 8;                       // 8, 4_4
    case 0x02: case 0x03: case 0x04: return 16;           // 16, 16 float, 8_8
    case 0x05: case 0x06: case 0x07: case 0x08:           // 32, 32 float, 16_16, 16_16 float
    case 0x09: case 0x0A: case 0x0B: return 32;           // 10_11_11 float, 8_8_8_8, 10_10_10_2
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: return 64; // 32_32, 32_32 float, 16x4, 16x4 float
    case 0x10: case 0x11: return 96;
    case 0x12: case 0x13: return 128;
    default: fatal("GX2: invalid attribute format 0x%X", format);
    }
}

uint32_t attrib_format_data_format(uint32_t format) {
    static constexpr uint32_t kDataFormat[20] = {
        fmt::k8, fmt::k4_4, fmt::k16, fmt::k16Float, fmt::k8_8, fmt::k32, fmt::k32Float, fmt::k16_16,
        fmt::k16_16Float, fmt::k10_11_11Float, fmt::k8_8_8_8, fmt::k10_10_10_2, fmt::k32_32,
        fmt::k32_32Float, fmt::k16_16_16_16, fmt::k16_16_16_16Float, fmt::k32_32_32, fmt::k32_32_32Float,
        fmt::k32_32_32_32, fmt::k32_32_32_32Float,
    };
    const uint32_t type = format & 0x1F;
    if (type >= 20) fatal("GX2: invalid attribute format 0x%X", format);
    return kDataFormat[type];
}

uint32_t attrib_format_endian(uint32_t format) {
    switch (format & 0x1F) {
    case 0x00: case 0x01: case 0x04: case 0x0A: return endian::kNone;
    case 0x02: case 0x03: case 0x07: case 0x08: case 0x0E: case 0x0F: return endian::k8In16;
    default: return endian::k8In32;
    }
}

uint32_t swap_mode_endian(uint32_t mode) {
    // GX2EndianSwapMode None, 8in16, 8in32 map directly; Default is decided
    // by the format (callers resolve it first).
    return mode & 3;
}

namespace {

uint32_t GX2GetSurfaceFormatBits(uint32_t format) {
    uint32_t bits = surface_format_bits_per_element(format);
    if (fmt::is_compressed(format & 0x3F)) bits >>= 4;
    return bits;
}
uint32_t GX2GetSurfaceFormatBitsPerElement(uint32_t format) { return surface_format_bits_per_element(format); }
bool GX2SurfaceIsCompressed(uint32_t format) { return fmt::is_compressed(format & 0x3F); }
uint32_t GX2GetAttribFormatBits(uint32_t format) { return attrib_format_bits(format); }

} // namespace

CAFE_EXPORT(gx2, GX2GetSurfaceFormatBits, GX2GetSurfaceFormatBits);
CAFE_EXPORT(gx2, GX2GetSurfaceFormatBitsPerElement, GX2GetSurfaceFormatBitsPerElement);
CAFE_EXPORT(gx2, GX2SurfaceIsCompressed, GX2SurfaceIsCompressed);
CAFE_EXPORT(gx2, GX2GetAttribFormatBits, GX2GetAttribFormatBits);

} // namespace cafe::gx2
