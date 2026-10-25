// Render targets: color and depth buffers as GPU images, found by the
// address, format and size the registers (or the GX2 structures of the HLE
// packets) give. Clears and surface copies work on them.

#include "gpu/vulkan/renderer.h"

#include "gpu/latte.h"
#include "gpu/tiling.h"
#include "gx2/internal.h"

#include "cafe/runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cafe::gpu::vk {
namespace {

using namespace latte;

// Address of the level a color/depth buffer view renders to, swizzle
// included, as GX2SetColorBuffer/GX2SetDepthBuffer program it.
uint32_t view_address(const gx2::Surface& s, uint32_t view_mip) {
    uint32_t address = s.image;
    if (view_mip != 0) {
        address = s.mipmaps;
        if (view_mip > 1) address += s.mip_level_offset[view_mip - 1];
    }
    if (gx2::is_macro_tiled(s.tile_mode) && view_mip < ((s.swizzle >> 16) & 0xFF)) address ^= s.swizzle & 0xFFFF;
    return address;
}

// Macro-tiled surfaces carry the bank/pipe swizzle in address bits 8-10.
uint32_t strip_swizzle(uint32_t address, uint32_t tile_mode) {
    return gx2::is_macro_tiled(tile_mode) ? address & ~0x700u : address;
}

} // namespace

// The screen (1280x720) and the buffers the title derives from it by
// halving, up to three times: their heights, rounded up to 16 rows, are
// 720, 368, 192 and 96 (widths vary with the viewport). Shadow maps,
// texture-compression targets and the GamePad screen are other sizes.
bool Renderer::upscaled_size(uint32_t width, uint32_t height) const {
    if (target_scale() <= 1.0f || width < 64) return false;
    const uint32_t rows = tv_height();
    for (uint32_t k = 0; k < 4; ++k) {
        if (height == ((rows >> k) + 15) / 16 * 16) return true;
    }
    return false;
}

Target* Renderer::find_target(uint32_t address, uint32_t format, bool depth) {
    Target* best = nullptr;
    for (auto& t : targets_) {
        if (t->address != address || t->depth != depth) continue;
        if (format != 0 && (t->format & 0x3F) != (format & 0x3F)) continue;
        if (best == nullptr || t->written > best->written) best = t.get();
    }
    return best;
}

Target* Renderer::color_target(uint32_t base_reg, uint32_t size_reg, uint32_t info_reg) {
    const uint32_t tile_mode = (info_reg >> 8) & 0xF;
    const uint32_t address = strip_swizzle(base_reg << 8, tile_mode);
    const uint32_t format = color_buffer_surface_format(info_reg);
    const uint32_t pitch = ((size_reg & 0x3FF) + 1) * 8;
    const uint32_t height = (((size_reg >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    for (auto& t : targets_) {
        if (!t->depth && t->address == address && t->format == format && t->pitch == pitch && t->height == height) {
            t->base_reg = base_reg;
            return t.get();
        }
    }
    const FormatInfo f = color_format(format);
    if (f.format == VK_FORMAT_UNDEFINED || f.compressed) {
        static uint32_t reported = 0;
        if (reported != format) std::fprintf(stderr, "ttt2: gpu: unsupported color buffer format 0x%03X\n", format);
        reported = format;
        return nullptr;
    }
    auto t = std::make_unique<Target>();
    t->address = address;
    t->format = format;
    t->pitch = pitch;
    t->height = height;
    t->tile_mode = tile_mode;
    t->base_reg = base_reg;
    t->scale = upscaled_size(pitch, height) ? target_scale() : 1;
    t->end = static_cast<uint32_t>(std::min<uint64_t>(uint64_t{address} + uint64_t{pitch} * height * std::max(1u, f.bytes), 0xFFFFFFFFu));
    // Written back if small, at the title's size and stored as in guest
    // memory (no component swizzle).
    const auto same = [](VkComponentSwizzle c) { return c == VK_COMPONENT_SWIZZLE_IDENTITY; };
    // shortcut: widened targets (signed 10_10_10_2) are not written back; pack them back to 10 bits if a title reads one.
    t->write_back = !f.widened && t->scale == 1 && uint64_t{pitch} * height * f.bytes <= kWriteBackBytes && same(f.swizzle.r) &&
                    same(f.swizzle.g) && same(f.swizzle.b) && same(f.swizzle.a);
    const uint32_t width = scaled(pitch, t->scale), rows = scaled(height, t->scale);
    t->image = create_image(f.format, VK_IMAGE_ASPECT_COLOR_BIT, width, rows, 1, 1,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_VIEW_TYPE_2D);
    t->written = stamp();
    t->written_frame = frame_number_;
    if (std::getenv("TTT2_TRACE_TARGETS")) {
        std::fprintf(stderr, "ttt2: gpu: new color target 0x%08X fmt 0x%03X %ux%u tile %u scale %g (size 0x%08X info 0x%08X)\n",
                     address, format, pitch, height, tile_mode, static_cast<double>(t->scale), size_reg, info_reg);
    }
    targets_.push_back(std::move(t));
    return targets_.back().get();
}

Target* Renderer::color_target(const gx2::ColorBuffer& buffer) {
    return color_target(view_address(buffer.surface, buffer.view_mip) >> 8, buffer.cb_color_size, buffer.cb_color_info);
}

Target* Renderer::depth_target(uint32_t base_reg, uint32_t size_reg, uint32_t info_reg) {
    const uint32_t tile_mode = (info_reg >> 15) & 0xF;
    const uint32_t address = strip_swizzle(base_reg << 8, tile_mode);
    const uint32_t format = depth_buffer_surface_format(info_reg);
    const uint32_t pitch = ((size_reg & 0x3FF) + 1) * 8;
    const uint32_t height = (((size_reg >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    for (auto& t : targets_) {
        if (t->depth && t->address == address && t->format == format && t->pitch == pitch && t->height == height) {
            return t.get();
        }
    }
    const FormatInfo f = depth_format(format);
    if (f.format == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr, "ttt2: gpu: unsupported depth buffer format 0x%03X\n", format);
        return nullptr;
    }
    auto t = std::make_unique<Target>();
    t->address = address;
    t->format = format;
    t->pitch = pitch;
    t->height = height;
    t->tile_mode = tile_mode;
    t->depth = true;
    t->scale = upscaled_size(pitch, height) ? target_scale() : 1;
    t->end = static_cast<uint32_t>(std::min<uint64_t>(uint64_t{address} + uint64_t{pitch} * height * 4, 0xFFFFFFFFu));
    if (std::getenv("TTT2_TRACE_TARGETS")) {
        std::fprintf(stderr, "ttt2: gpu: new depth target 0x%08X fmt 0x%03X %ux%u tile %u scale %g (size 0x%08X info 0x%08X)\n",
                     address, format, pitch, height, tile_mode, static_cast<double>(t->scale), size_reg, info_reg);
    }
    t->image = create_image(f.format, f.aspect, scaled(pitch, t->scale), scaled(height, t->scale), 1, 1,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_VIEW_TYPE_2D);
    t->written = stamp();
    t->written_frame = frame_number_;
    targets_.push_back(std::move(t));
    return targets_.back().get();
}

Target* Renderer::depth_target(const gx2::DepthBuffer& buffer) {
    return depth_target(view_address(buffer.surface, buffer.view_mip) >> 8, buffer.db_depth_size,
                        buffer.db_depth_info);
}

// ----------------------------------------------------------------- clears

void Renderer::clear_color(const Registers&, const gx2::ColorBuffer& buffer, const float rgba[4]) {
    Target* t = color_target(buffer);
    if (t == nullptr) return;
    end_rendering();
    barrier();
    VkClearColorValue value{};
    const uint32_t type = (t->format >> 8) & 0xF;
    for (int i = 0; i < 4; ++i) {
        if (type == gx2::surface_format::kTypeUint) value.uint32[i] = static_cast<uint32_t>(rgba[i]);
        else if (type == gx2::surface_format::kTypeSint) value.int32[i] = static_cast<int32_t>(rgba[i]);
        else value.float32[i] = rgba[i];
    }
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    barrier();
    profile_mark("clear color", t->address, t->image.width, t->image.height);
    t->written = stamp();
    note_target_written(t);
    if (tracing()) {
        std::fprintf(stderr, "trace: clear color 0x%08X fmt 0x%03X %ux%u (%g %g %g %g)\n", t->address, t->format,
                     t->pitch, t->height, rgba[0], rgba[1], rgba[2], rgba[3]);
    }
}

void Renderer::clear_depth_stencil(const Registers&, const gx2::DepthBuffer& buffer, uint32_t flags, float depth,
                                   uint32_t stencil) {
    Target* t = depth_target(buffer);
    if (t == nullptr) return;
    VkImageAspectFlags aspect = 0;
    if (flags & 1) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT; // GX2_CLEAR_FLAGS_DEPTH
    if (flags & 2) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    aspect &= t->image.aspect;
    if (aspect == 0) return;
    end_rendering();
    barrier();
    VkClearDepthStencilValue value{std::clamp(depth, 0.0f, 1.0f), stencil & 0xFF};
    VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    barrier();
    profile_mark("clear depth", t->address, t->image.width, t->image.height);
    t->written = stamp();
    note_target_written(t);
    if (tracing()) {
        std::fprintf(stderr, "trace: clear depth 0x%08X fmt 0x%03X %ux%u flags %u (%g, %u)\n", t->address, t->format,
                     t->pitch, t->height, flags, depth, stencil);
    }
}

// ----------------------------------------------------------------- copies

void Renderer::copy_target_region(const Target& src, const Image& dst, float dst_scale, uint32_t dst_layer,
                                  uint32_t width, uint32_t height) {
    note_target_read(&src);
    end_rendering();
    barrier();
    const VkImageAspectFlags aspect = dst.aspect & src.image.aspect;
    if (src.scale == dst_scale) {
        VkImageCopy region{};
        region.srcSubresource = {aspect, 0, 0, 1};
        region.dstSubresource = {aspect, 0, dst_layer, 1};
        region.extent = {std::min({scaled(width, dst_scale), src.image.width, dst.width}),
                         std::min({scaled(height, dst_scale), src.image.height, dst.height}), 1};
        vkCmdCopyImage(cmd(), src.image.image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    } else {
        // Between an upscaled and a title-sized image: filtered for colour.
        // (The far corners round up, as at 1.5 an odd size ends in half a
        // pixel; they stay inside the images, which the size fits.)
        width = std::min({width, unscaled(src.image.width, src.scale), unscaled(dst.width, dst_scale)});
        height = std::min({height, unscaled(src.image.height, src.scale), unscaled(dst.height, dst_scale)});
        const auto corner = [&](float scale) {
            return VkOffset3D{static_cast<int32_t>(scaled(width, scale)), static_cast<int32_t>(scaled(height, scale)),
                              1};
        };
        VkImageBlit blit{};
        blit.srcSubresource = {aspect, 0, 0, 1};
        blit.srcOffsets[1] = corner(src.scale);
        blit.dstSubresource = {aspect, 0, dst_layer, 1};
        blit.dstOffsets[1] = corner(dst_scale);
        const bool linear = !(aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) &&
                            (ctx_.format_features(dst.format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        vkCmdBlitImage(cmd(), src.image.image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    barrier();
    profile_mark("copy target", src.address, width, height);
}

namespace {

// The registers a color buffer over level `level` of `s` would have.
void surface_level_registers(const gx2::Surface& s, uint32_t level, uint32_t& base, uint32_t& size, uint32_t& info) {
    const gx2::SurfaceInfo li = gx2::surface_info(s, level);
    base = view_address(s, level) >> 8;
    size = field(li.pitch / 8 - 1, 0, 10) | field(li.pitch * li.height / 64 - 1, 10, 20);
    // CB_COLOR_INFO's FORMAT, ARRAY_MODE and NUMBER_TYPE, which is all
    // color_target() reads.
    const uint32_t type = gx2::surface_format::type(s.format);
    uint32_t number = 0;
    switch (type) {
    case gx2::surface_format::kTypeSnorm: number = 1; break;
    case gx2::surface_format::kTypeUint: number = 4; break;
    case gx2::surface_format::kTypeSint: number = 5; break;
    case gx2::surface_format::kTypeSrgb: number = 6; break;
    case gx2::surface_format::kTypeFloat: number = 7; break;
    default: break;
    }
    info = field(s.format & 0x3F, 2, 6) | field(li.tile_mode, 8, 4) | field(number, 12, 3);
}

// DB_FORMAT for a GX2 depth surface format.
uint32_t db_format_of(uint32_t format) {
    switch (format) {
    case gx2::surface_format::kUnormR16: return 1;
    case gx2::surface_format::kFloatR32: return 6;
    case gx2::surface_format::kFloatD24S8: return 5;
    case gx2::surface_format::kFloatX8X24: return 7;
    default: return 3; // DEPTH_8_24
    }
}

} // namespace

void Renderer::copy_surface(const gx2::Surface& src, uint32_t src_level, uint32_t src_slice, const gx2::Surface& dst,
                            uint32_t dst_level, uint32_t dst_slice) {
    if (tracing() || std::getenv("TTT2_TRACE_COPIES")) {
        std::fprintf(stderr, "copy: src img 0x%08X mip 0x%08X dim %u %ux%ux%u levels %u fmt 0x%03X tile %u swz 0x%08X pitch %u use 0x%X | lvl %u slice %u\n",
                     uint32_t{src.image}, uint32_t{src.mipmaps}, uint32_t{src.dim}, uint32_t{src.width}, uint32_t{src.height},
                     uint32_t{src.depth}, uint32_t{src.mip_levels}, uint32_t{src.format}, uint32_t{src.tile_mode},
                     uint32_t{src.swizzle}, uint32_t{src.pitch}, uint32_t{src.use}, src_level, src_slice);
        std::fprintf(stderr, "copy: dst img 0x%08X mip 0x%08X dim %u %ux%ux%u levels %u fmt 0x%03X tile %u swz 0x%08X pitch %u use 0x%X | lvl %u slice %u\n",
                     uint32_t{dst.image}, uint32_t{dst.mipmaps}, uint32_t{dst.dim}, uint32_t{dst.width}, uint32_t{dst.height},
                     uint32_t{dst.depth}, uint32_t{dst.mip_levels}, uint32_t{dst.format}, uint32_t{dst.tile_mode},
                     uint32_t{dst.swizzle}, uint32_t{dst.pitch}, uint32_t{dst.use}, dst_level, dst_slice);
    }
    if (tracing()) {
        std::fprintf(stderr, "trace: copy surface 0x%08X fmt 0x%03X %ux%u tile %u level %u slice %u -> 0x%08X fmt 0x%03X %ux%u tile %u level %u slice %u\n",
                     uint32_t{src.image}, uint32_t{src.format}, uint32_t{src.width}, uint32_t{src.height},
                     uint32_t{src.tile_mode}, src_level, src_slice, uint32_t{dst.image}, uint32_t{dst.format},
                     uint32_t{dst.width}, uint32_t{dst.height}, uint32_t{dst.tile_mode}, dst_level, dst_slice);
    }
    uint32_t sb, ss, si, db, ds, di;
    surface_level_registers(src, src_level, sb, ss, si);
    surface_level_registers(dst, dst_level, db, ds, di);
    const bool src_depth = (src.use & gx2::surface_use::kDepthBuffer) != 0;
    Target* from = find_target(strip_swizzle(sb << 8, (si >> 8) & 0xF), src.format, src_depth);
    if (from == nullptr) {
        // Only in guest memory: copy the elements, retiling. (Done by the
        // frame itself: a replay must not write guest memory.)
        if (replaying_) return;
        uint32_t address = 0, bytes = 0;
        if (!copy_surface_memory(src, src_level, src_slice, dst, dst_level, dst_slice, address, bytes)) {
            std::fprintf(stderr, "ttt2: gpu: GX2CopySurface fmt 0x%X -> 0x%X not supported\n", uint32_t{src.format},
                         uint32_t{dst.format});
            return;
        }
        cpu_wrote(address, bytes);
        return;
    }
    if (src_slice != 0 || dst_slice != 0) {
        std::fprintf(stderr, "ttt2: gpu: GX2CopySurface between render target slices %u -> %u not supported\n",
                     src_slice, dst_slice);
        return;
    }
    if (src_depth) {
        Target* to = depth_target(db, ds, db_format_of(dst.format) | field(gx2::surface_info(dst, dst_level).tile_mode, 15, 4));
        if (to == nullptr) return;
        copy_target_region(*from, to->image, to->scale, 0, from->pitch, from->height);
        to->written = stamp();
        note_target_written(to);
        return;
    }
    Target* to = color_target(db, ds, di);
    if (to == nullptr) return;
    copy_target_region(*from, to->image, to->scale, 0, std::max(1u, src.width >> src_level),
                       std::max(1u, src.height >> src_level));
    to->written = stamp();
    note_target_written(to);
}

void Renderer::resolve_color(const gx2::ColorBuffer& src, const gx2::Surface& dst, uint32_t dst_level, uint32_t dst_slice) {
    if (tracing()) std::fprintf(stderr, "trace: resolve 0x%08X -> 0x%08X\n", uint32_t{src.surface.image}, uint32_t{dst.image});
    // Render targets are single-sampled here: a resolve is a copy.
    Target* from = color_target(src);
    if (from == nullptr) return;
    uint32_t db, ds, di;
    surface_level_registers(dst, dst_level, db, ds, di);
    if (dst_slice != 0) std::fprintf(stderr, "ttt2: gpu: resolve to slice %u not supported\n", dst_slice);
    Target* to = color_target(db, ds, di);
    if (to == nullptr) return;
    copy_target_region(*from, to->image, to->scale, 0, src.surface.width, src.surface.height);
    to->written = stamp();
    note_target_written(to);
}

void Renderer::convert_depth(const gx2::DepthBuffer& src, const gx2::Surface& dst, uint32_t dst_level,
                             uint32_t dst_slice) {
    if (tracing()) std::fprintf(stderr, "trace: convert depth 0x%08X -> 0x%08X fmt 0x%03X\n", uint32_t{src.surface.image}, uint32_t{dst.image}, uint32_t{dst.format});
    // The depth values become a texture the title samples; keep them as a
    // depth image at the destination address.
    Target* from = depth_target(src);
    if (from == nullptr) return;
    const gx2::SurfaceInfo li = gx2::surface_info(dst, dst_level);
    const uint32_t base = view_address(dst, dst_level) >> 8;
    const uint32_t size = field(li.pitch / 8 - 1, 0, 10) | field(li.pitch * li.height / 64 - 1, 10, 20);
    const uint32_t db_format = db_format_of(dst.format);
    if (dst_slice != 0) std::fprintf(stderr, "ttt2: gpu: depth conversion to slice %u not supported\n", dst_slice);
    Target* to = depth_target(base, size, db_format | field(li.tile_mode, 15, 4));
    if (to == nullptr) return;
    copy_target_region(*from, to->image, to->scale, 0, src.surface.width, src.surface.height);
    to->written = stamp();
    note_target_written(to);
}


// ------------------------------------------------------------ write-back

void Renderer::queue_write_back(Target* t) {
    if (!t->write_back || replaying_ || t->write_back_queued) return;
    t->write_back_queued = true;
    write_back_targets_.push_back(t);
}

// At the end of a submission: the targets it wrote, copied to staging memory.
void Renderer::copy_write_backs() {
    if (write_back_targets_.empty()) return;
    end_rendering();
    barrier();
    const uint64_t value = submitted_ + 1; // this submission's
    std::lock_guard lock(write_back_mutex_);
    for (Target* t : write_back_targets_) {
        t->write_back_queued = false;
        const uint32_t bytes = color_format(t->format).bytes;
        if (!guest_memory_committed(t->address, t->pitch * t->height * bytes)) continue;
        if (write_back_free_.empty()) {
            write_back_free_.push_back(static_cast<uint32_t>(write_back_staging_.size()));
            write_back_staging_.push_back(ctx_.create_buffer(
                kWriteBackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        }
        const uint32_t staging = write_back_free_.back();
        write_back_free_.pop_back();
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {t->pitch, t->height, 1};
        vkCmdCopyImageToBuffer(cmd(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, write_back_staging_[staging].buffer, 1,
                               &region);
        write_backs_.push_back({value, staging, t->address, t->base_reg, t->tile_mode, bytes * 8, t->pitch, t->height});
        if (tracing()) {
            std::fprintf(stderr, "trace: write back 0x%08X fmt 0x%03X %ux%u tile %u\n", t->address, t->format, t->pitch,
                         t->height, t->tile_mode);
        }
    }
    write_back_targets_.clear();
    // For the host, and before the next submission's commands: its first
    // draw may render into a target these copies read.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd(), &di);
}

// (Retirement thread.) The copies of the submissions up to `value`, tiled
// into guest memory.
void Renderer::retired(uint64_t value) {
    for (;;) {
        WriteBack w;
        const uint8_t* data;
        {
            std::lock_guard lock(write_back_mutex_);
            if (write_backs_.empty() || write_backs_.front().value > value) return;
            w = write_backs_.front();
            write_backs_.pop_front();
            data = write_back_staging_[w.staging].mapped;
        }
        TiledLevel level{};
        level.data = guest_pointer(w.address);
        level.base256b = w.base_reg;
        level.tile_mode = w.tile_mode;
        level.bpp = w.bpp;
        level.pitch = w.pitch;
        level.height = w.height;
        level.slices = 1;
        level.samples = 1;
        tile(level, w.pitch, w.height, 0, 1, data);
        std::lock_guard lock(write_back_mutex_);
        write_back_free_.push_back(w.staging);
    }
}

} // namespace cafe::gpu::vk
