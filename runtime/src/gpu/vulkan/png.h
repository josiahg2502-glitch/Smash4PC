#pragma once

// Minimal PNG writer (uncompressed deflate) for frame captures.

#include <cstdint>
#include <string>

namespace cafe::gpu::vk {

// `rgba` holds width * height pixels, 4 bytes each, rows top to bottom.
bool write_png(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height);

} // namespace cafe::gpu::vk
