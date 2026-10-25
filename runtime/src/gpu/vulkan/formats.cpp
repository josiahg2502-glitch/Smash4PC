#include "gpu/vulkan/formats.h"

#include "gpu/latte.h"
#include "gx2/types.h"

namespace cafe::gpu::vk {
namespace {

using namespace latte;

bool g_a4b4g4r4 = false;
bool g_a1b5g5r5 = false;
bool g_rgba16_snorm = false;
bool g_d24s8 = false;

constexpr VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B,
                             A = VK_COMPONENT_SWIZZLE_A, I = VK_COMPONENT_SWIZZLE_IDENTITY;

bool supported(VkPhysicalDevice physical, VkFormat format, VkFormatFeatureFlags features) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(physical, format, &p);
    return (p.optimalTilingFeatures & features) == features;
}

// Per type: unorm, uint, snorm, sint, srgb.
VkFormat by_type(uint32_t type, VkFormat unorm, VkFormat uint, VkFormat snorm, VkFormat sint, VkFormat srgb) {
    switch (type) {
    case gx2::surface_format::kTypeUint: return uint;
    case gx2::surface_format::kTypeSnorm: return snorm;
    case gx2::surface_format::kTypeSint: return sint;
    case gx2::surface_format::kTypeSrgb: return srgb != VK_FORMAT_UNDEFINED ? srgb : unorm;
    default: return unorm;
    }
}

} // namespace

void init_formats(VkPhysicalDevice physical) {
    const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    g_a4b4g4r4 = supported(physical, VK_FORMAT_A4B4G4R4_UNORM_PACK16, sampled);
    g_a1b5g5r5 = supported(physical, VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR, sampled);
    g_rgba16_snorm = supported(physical, VK_FORMAT_R16G16B16A16_SNORM,
                               VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    g_d24s8 = supported(physical, VK_FORMAT_D24_UNORM_S8_UINT,
                        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}

FormatInfo color_format(uint32_t gx2_format) {
    FormatInfo f;
    f.swizzle = {I, I, I, I};
    const uint32_t hw = gx2_format & 0x3F;
    const uint32_t type = (gx2_format >> 8) & 0xF;
    const bool is_float = type == gx2::surface_format::kTypeFloat;
    switch (hw) {
    case fmt::k8:
        f.format = by_type(type, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_SINT, VK_FORMAT_R8_SRGB);
        f.bytes = 1;
        break;
    case fmt::k4_4: // X in the low nibble; Vulkan's R4G4 has R in the high one
        f.format = VK_FORMAT_R4G4_UNORM_PACK8;
        f.swizzle = {G, R, I, I};
        f.bytes = 1;
        break;
    case fmt::k16:
        f.format = is_float ? VK_FORMAT_R16_SFLOAT
                            : by_type(type, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_SINT,
                                      VK_FORMAT_UNDEFINED);
        f.bytes = 2;
        break;
    case fmt::k16Float: f.format = VK_FORMAT_R16_SFLOAT; f.bytes = 2; break;
    case fmt::k8_8:
        f.format = by_type(type, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_SINT,
                           VK_FORMAT_R8G8_SRGB);
        f.bytes = 2;
        break;
    case fmt::k5_6_5: f.format = VK_FORMAT_B5G6R5_UNORM_PACK16; f.bytes = 2; break; // R in bits 0-4
    case fmt::k1_5_5_5: // R5_G5_B5_A1: R in bits 0-4, A in bit 15
        if (g_a1b5g5r5) {
            f.format = VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR;
        } else {
            f.format = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
            f.swizzle = {B, G, R, A};
        }
        f.bytes = 2;
        break;
    case fmt::k4_4_4_4: // R in bits 0-3
        if (g_a4b4g4r4) {
            f.format = VK_FORMAT_A4B4G4R4_UNORM_PACK16;
        } else {
            f.format = VK_FORMAT_R4G4B4A4_UNORM_PACK16;
            f.swizzle = {A, B, G, R};
        }
        f.bytes = 2;
        break;
    case fmt::k32:
        f.format = is_float ? VK_FORMAT_R32_SFLOAT : type == gx2::surface_format::kTypeSint ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
        f.bytes = 4;
        break;
    case fmt::k32Float: f.format = VK_FORMAT_R32_SFLOAT; f.bytes = 4; break;
    case fmt::k16_16:
        f.format = is_float ? VK_FORMAT_R16G16_SFLOAT
                            : by_type(type, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SNORM,
                                      VK_FORMAT_R16G16_SINT, VK_FORMAT_UNDEFINED);
        f.bytes = 4;
        break;
    case fmt::k16_16Float: f.format = VK_FORMAT_R16G16_SFLOAT; f.bytes = 4; break;
    case fmt::k10_11_11Float: // R11_G11_B10: R in bits 0-10
    case fmt::k10_11_11:
        f.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        f.bytes = 4;
        break;
    case fmt::k2_10_10_10: // R10_G10_B10_A2: R in bits 0-9
        f.format = type == gx2::surface_format::kTypeUint ? VK_FORMAT_A2B10G10R10_UINT_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        f.bytes = 4;
        // Signed: an unsigned image would clamp the title's negative values
        // to 0 (Smash's lighting buffers hold signed data).
        if (type == gx2::surface_format::kTypeSnorm && g_rgba16_snorm) {
            f.format = VK_FORMAT_R16G16B16A16_SNORM;
            f.widened = true;
        }
        break;
    case fmt::k8_8_8_8:
        f.format = by_type(type, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SNORM,
                           VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SRGB);
        f.bytes = 4;
        break;
    case fmt::k32_32:
        f.format = is_float ? VK_FORMAT_R32G32_SFLOAT : type == gx2::surface_format::kTypeSint ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT;
        f.bytes = 8;
        break;
    case fmt::k32_32Float: f.format = VK_FORMAT_R32G32_SFLOAT; f.bytes = 8; break;
    case fmt::k16_16_16_16:
        f.format = is_float ? VK_FORMAT_R16G16B16A16_SFLOAT
                            : by_type(type, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UINT,
                                      VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_UNDEFINED);
        f.bytes = 8;
        break;
    case fmt::k16_16_16_16Float: f.format = VK_FORMAT_R16G16B16A16_SFLOAT; f.bytes = 8; break;
    case fmt::k32_32_32_32:
        f.format = is_float ? VK_FORMAT_R32G32B32A32_SFLOAT
                   : type == gx2::surface_format::kTypeSint ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
        f.bytes = 16;
        break;
    case fmt::k32_32_32_32Float: f.format = VK_FORMAT_R32G32B32A32_SFLOAT; f.bytes = 16; break;
    // Depth data read as a texture: the depth value in X.
    case fmt::k8_24:
    case fmt::k8_24Float:
        f.format = VK_FORMAT_R32_SFLOAT;
        f.bytes = 4;
        break;
    case fmt::kX24_8_32Float:
        f.format = VK_FORMAT_R32G32_SFLOAT;
        f.bytes = 8;
        break;
    case fmt::kBc1:
        f.format = type == gx2::surface_format::kTypeSrgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        f.bytes = 8;
        f.compressed = true;
        break;
    case fmt::kBc2:
        f.format = type == gx2::surface_format::kTypeSrgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
        f.bytes = 16;
        f.compressed = true;
        break;
    case fmt::kBc3:
        f.format = type == gx2::surface_format::kTypeSrgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        f.bytes = 16;
        f.compressed = true;
        break;
    case fmt::kBc4:
        f.format = type == gx2::surface_format::kTypeSnorm ? VK_FORMAT_BC4_SNORM_BLOCK : VK_FORMAT_BC4_UNORM_BLOCK;
        f.bytes = 8;
        f.compressed = true;
        break;
    case fmt::kBc5:
        f.format = type == gx2::surface_format::kTypeSnorm ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK;
        f.bytes = 16;
        f.compressed = true;
        break;
    default: break;
    }
    return f;
}

FormatInfo depth_format(uint32_t gx2_format) {
    FormatInfo f;
    f.swizzle = {I, I, I, I};
    switch (gx2_format) {
    case gx2::surface_format::kUnormR16:
        f.format = VK_FORMAT_D16_UNORM;
        f.bytes = 2;
        f.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;
    case gx2::surface_format::kUnormR24X8:
        f.format = g_d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        f.bytes = 4;
        f.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        break;
    case gx2::surface_format::kFloatR32:
        f.format = VK_FORMAT_D32_SFLOAT;
        f.bytes = 4;
        f.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;
    case gx2::surface_format::kFloatD24S8:
    case gx2::surface_format::kFloatX8X24:
        f.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        f.bytes = gx2_format == gx2::surface_format::kFloatD24S8 ? 4 : 8;
        f.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        break;
    default: break;
    }
    return f;
}

uint32_t color_buffer_surface_format(uint32_t info) {
    const uint32_t hw = (info >> 2) & 0x3F;
    uint32_t type = 0;
    switch ((info >> 12) & 7) { // NUMBER_TYPE
    case 1: type = gx2::surface_format::kTypeSnorm; break;
    case 4: type = gx2::surface_format::kTypeUint; break;
    case 5: type = gx2::surface_format::kTypeSint; break;
    case 6: type = gx2::surface_format::kTypeSrgb; break;
    case 7: type = gx2::surface_format::kTypeFloat; break;
    default: break;
    }
    return hw | (type << 8);
}

uint32_t depth_buffer_surface_format(uint32_t info) {
    switch (info & 7) { // DB_FORMAT
    case 1: return gx2::surface_format::kUnormR16;
    case 3: return gx2::surface_format::kUnormR24X8;
    case 5: return gx2::surface_format::kFloatD24S8;
    case 6: return gx2::surface_format::kFloatR32;
    case 7: return gx2::surface_format::kFloatX8X24;
    default: return 0;
    }
}

uint32_t texture_surface_format(uint32_t word1, uint32_t word4) {
    const uint32_t hw = word1 >> 26;
    uint32_t type = 0;
    switch (hw) {
    case fmt::k16Float: case fmt::k32Float: case fmt::k16_16Float: case fmt::k10_11_11Float:
    case fmt::k11_11_10Float: case fmt::k32_32Float: case fmt::k16_16_16_16Float: case fmt::k32_32_32_32Float:
    case fmt::k32_32_32Float:
        type = gx2::surface_format::kTypeFloat;
        break;
    default: {
        const bool is_signed = (word4 & 3) == 1;          // FORMAT_COMP_X
        const uint32_t num_format = (word4 >> 8) & 3;     // NUM_FORMAT_ALL
        if (num_format == 1) type = is_signed ? gx2::surface_format::kTypeSint : gx2::surface_format::kTypeUint;
        else if ((word4 >> 11) & 1) type = gx2::surface_format::kTypeSrgb; // FORCE_DEGAMMA
        else type = is_signed ? gx2::surface_format::kTypeSnorm : gx2::surface_format::kTypeUnorm;
        break;
    }
    }
    return hw | (type << 8);
}

} // namespace cafe::gpu::vk
