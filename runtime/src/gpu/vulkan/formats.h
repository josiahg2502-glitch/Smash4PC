#pragma once

// Latte surface formats as Vulkan formats.
//
// GX2 surface formats are the hardware data format (bits 0-5) and a numeric
// type (bits 8-11: 0 unorm, 1 uint, 2 snorm, 3 sint, 4 srgb, 8 float).
// Components are stored from the least significant bits up (X first), the
// GPU's little-endian order; packed Vulkan formats that list components
// the other way round are used with a swizzle.

#include <vulkan/vulkan.h>

#include <cstdint>

namespace cafe::gpu::vk {

struct FormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkComponentMapping swizzle{};       // applied when sampling
    uint32_t bytes = 0;                 // per element (texel, or 4x4 block)
    bool compressed = false;            // 4x4 blocks
    // Signed 10_10_10_2, held as R16G16B16A16_SNORM (Vulkan has no signed
    // 10-bit format to render to): `bytes` stays the guest's 4, the image's
    // texels are 8, and uploads widen the data.
    bool widened = false;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

// Decides between equivalent formats by device support. Call once.
void init_formats(VkPhysicalDevice physical);

FormatInfo color_format(uint32_t gx2_format);
// GX2 depth buffer formats (GX2SurfaceFormat of a depth buffer surface).
FormatInfo depth_format(uint32_t gx2_format);

// GX2 surface format of a color buffer (CB_COLORn_INFO), a depth buffer
// (DB_DEPTH_INFO) and a texture resource (SQ_TEX_RESOURCE words 1 and 4).
uint32_t color_buffer_surface_format(uint32_t cb_color_info);
uint32_t depth_buffer_surface_format(uint32_t db_depth_info);
uint32_t texture_surface_format(uint32_t word1, uint32_t word4);

} // namespace cafe::gpu::vk
