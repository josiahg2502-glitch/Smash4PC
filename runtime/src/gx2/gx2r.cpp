// GX2R: buffers, surfaces and display lists as resources, allocated through
// the title's allocator (GX2RSetAllocator) and kept coherent by flushing the
// GPU caches their bind flags name.

#include "gx2/internal.h"

#include "os/kernel.h"

#include "cafe/export.h"
#include "cafe/runtime.h"

namespace cafe::gx2 {
namespace {

using namespace latte;
namespace rf = resource_flags;

uint32_t g_alloc = 0; // guest functions, 0 = default heap
uint32_t g_free = 0;

// Bits 19-23 are per-call options; the rest describe the resource.
constexpr uint32_t kOptionMask = 0xF80000;

uint32_t gx2r_alloc(PPCContext& ctx, uint32_t flags, uint32_t size, uint32_t alignment) {
    if (g_alloc == 0) return os::default_heap_alloc(ctx, size, alignment);
    return call_guest(ctx, g_alloc, {flags, size, alignment});
}

void gx2r_free(PPCContext& ctx, uint32_t flags, uint32_t block) {
    if (g_free == 0) return os::default_heap_free(ctx, block);
    call_guest(ctx, g_free, {flags, block});
}

uint32_t buffer_size(const RBuffer& b) { return b.elem_size * b.elem_count; }

// The GPU caches to flush for a resource with these flags.
uint32_t invalidate_mode(uint32_t flags) {
    uint32_t mode = 0;
    if (flags & rf::kBindTexture) mode |= invalidate::kTexture;
    if (flags & rf::kBindColorBuffer) mode |= invalidate::kColorBuffer;
    if (flags & rf::kBindDepthBuffer) mode |= invalidate::kDepthBuffer;
    if (flags & rf::kBindScanBuffer) mode |= invalidate::kColorBuffer | invalidate::kDepthBuffer;
    if (flags & (rf::kBindVertexBuffer | rf::kBindIndexBuffer)) mode |= invalidate::kAttributeBuffer;
    if (flags & rf::kBindUniformBlock) mode |= invalidate::kUniformBlock;
    if (flags & rf::kBindShaderProgram) mode |= invalidate::kShader;
    if (flags & rf::kBindStreamOutput) mode |= invalidate::kStreamOutBuffer;
    if (flags & (rf::kCpuRead | rf::kCpuWrite)) mode |= invalidate::kCpu;
    if (flags & rf::kDisableCpuInvalidate) mode &= ~invalidate::kCpu;
    if (flags & rf::kDisableGpuInvalidate) mode &= invalidate::kCpu;
    return mode;
}

void GX2RInvalidateMemory(uint32_t flags, uint32_t address, uint32_t size) {
    ApiLock lock;
    invalidate_caches(invalidate_mode(flags), address, size);
}

void GX2RSetAllocator(uint32_t alloc, uint32_t free) {
    g_alloc = alloc;
    g_free = free;
}

uint32_t GX2RGetBufferAlignment(uint32_t flags) {
    if (flags & (rf::kBindStreamOutput | rf::kBindUniformBlock | rf::kBindShaderProgram | rf::kBindGsRing)) {
        return 0x100;
    }
    if (flags & (rf::kBindVertexBuffer | rf::kBindIndexBuffer | rf::kBindDisplayList)) return 0x40;
    return 0x100;
}

uint32_t GX2RGetBufferAllocationSize(RBuffer* buffer) { return (buffer_size(*buffer) + 0x3F) & ~0x3Fu; }

bool GX2RBufferExists(RBuffer* buffer) { return buffer != nullptr && buffer->buffer != 0u; }
void GX2RSetBufferName(RBuffer*, uint32_t) {}

bool GX2RCreateBuffer(PPCContext& ctx, RBuffer* buffer) {
    const uint32_t flags = (buffer->flags & ~rf::kLocked) | rf::kAllocated;
    buffer->flags = flags;
    buffer->buffer = gx2r_alloc(ctx, flags, GX2RGetBufferAllocationSize(buffer), GX2RGetBufferAlignment(flags));
    return buffer->buffer != 0u;
}

bool GX2RCreateBufferUserMemory(RBuffer* buffer, uint32_t memory, uint32_t) {
    buffer->buffer = memory;
    buffer->flags = buffer->flags & ~(rf::kLocked | rf::kAllocated);
    return true;
}

void GX2RDestroyBufferEx(PPCContext& ctx, RBuffer* buffer, uint32_t flags) {
    if (buffer == nullptr || buffer->buffer == 0u) return;
    if ((buffer->flags & rf::kAllocated) && !((flags & kOptionMask) & rf::kDestroyNoFree)) {
        gx2r_free(ctx, buffer->flags | (flags & kOptionMask), buffer->buffer);
    }
    buffer->buffer = 0u;
}

void GX2RInvalidateBuffer(RBuffer* buffer, uint32_t flags) {
    GX2RInvalidateMemory((buffer->flags & ~kOptionMask) | (flags & kOptionMask), buffer->buffer,
                         buffer_size(*buffer));
}

uint32_t GX2RLockBufferEx(RBuffer* buffer, uint32_t flags) {
    buffer->flags = buffer->flags | rf::kLocked | (flags & rf::kLockedReadOnly);
    return buffer->buffer;
}

void GX2RUnlockBufferEx(RBuffer* buffer, uint32_t flags) {
    if (!(buffer->flags & rf::kLockedReadOnly)) GX2RInvalidateBuffer(buffer, flags);
    buffer->flags = buffer->flags & ~(rf::kLocked | rf::kLockedReadOnly);
}

// ------------------------------------------------------------------ draws
void GX2RSetAttributeBuffer(RBuffer* buffer, uint32_t index, uint32_t stride, uint32_t offset) {
    ApiLock lock;
    const uint32_t size = buffer_size(*buffer);
    const uint32_t words[resource::kWords] = {
        buffer->buffer + offset, size - offset - 1, field(stride, 8, 11), 0, 0, 0, resource::kTypeValidBuffer,
    };
    set_resource(resource::kVsAttrib + index, words);
}

void GX2RDrawIndexed(uint32_t mode, RBuffer* indices, uint32_t type, uint32_t count, uint32_t first_index,
                     uint32_t base_vertex, uint32_t instances) {
    ApiLock lock;
    draw_indexed(mode, count, type, indices->buffer + first_index * indices->elem_size, base_vertex, instances);
}

void GX2RSetStreamOutBuffer(uint32_t index, OutputStream* stream) {
    ApiLock lock;
    set_stream_out_buffer(index, *stream);
}

// --------------------------------------------------------------- surfaces
// [address, size] of one level of a surface; level -1 is the whole mip chain.
void level_data(const Surface& s, int32_t level, uint32_t& address, uint32_t& size) {
    if (level == 0) {
        address = s.image;
        size = s.image_size;
    } else if (level < 0) {
        address = s.mipmaps;
        size = s.mipmap_size;
    } else {
        const uint32_t begin = level > 1 ? uint32_t{s.mip_level_offset[level - 1]} : 0u;
        const uint32_t end = static_cast<uint32_t>(level) + 1 >= s.mip_levels ? uint32_t{s.mipmap_size}
                                                                              : uint32_t{s.mip_level_offset[level]};
        address = s.mipmaps + begin;
        size = end - begin;
    }
}

bool GX2RCreateSurface(PPCContext& ctx, Surface* surface, uint32_t flags) {
    surface->use = (flags & ~rf::kLocked) | rf::kAllocated;
    calc_surface_size_and_alignment(*surface);
    const uint32_t block =
        gx2r_alloc(ctx, surface->use, surface->image_size + surface->mipmap_size, surface->alignment);
    surface->image = block;
    surface->mipmaps = (block != 0 && surface->mipmap_size != 0u) ? block + surface->image_size : 0u;
    return block != 0;
}

bool GX2RCreateSurfaceUserMemory(Surface* surface, uint32_t image, uint32_t mipmaps, uint32_t flags) {
    surface->use = flags & ~(rf::kLocked | rf::kAllocated);
    calc_surface_size_and_alignment(*surface);
    surface->image = image;
    surface->mipmaps = mipmaps;
    return true;
}

void GX2RDestroySurfaceEx(PPCContext& ctx, Surface* surface, uint32_t flags) {
    if (surface == nullptr || surface->image == 0u) return;
    if (surface->use & rf::kAllocated) gx2r_free(ctx, surface->use | (flags & kOptionMask), surface->image);
    surface->image = 0u;
}

bool GX2RSurfaceExists(Surface* surface) {
    return surface != nullptr && surface->image != 0u &&
           (surface->use & (rf::kCpuRead | rf::kCpuWrite | rf::kGpuRead | rf::kGpuWrite)) != 0;
}

void GX2RInvalidateSurface(Surface* surface, int32_t level, uint32_t flags) {
    uint32_t address, size;
    level_data(*surface, level, address, size);
    GX2RInvalidateMemory(surface->use | (flags & kOptionMask), address, size);
}

uint32_t GX2RLockSurfaceEx(Surface* surface, int32_t, uint32_t flags) {
    surface->use = surface->use | rf::kLocked | (flags & rf::kLockedReadOnly);
    return surface->image;
}

void GX2RUnlockSurfaceEx(Surface* surface, int32_t level, uint32_t flags) {
    if (!(surface->use & rf::kLockedReadOnly)) GX2RInvalidateSurface(surface, level, flags);
    surface->use = surface->use & ~(rf::kLocked | rf::kLockedReadOnly);
}

// ---------------------------------------------------------- display lists
void GX2RBeginDisplayListEx(RBuffer* list, bool, uint32_t) {
    if (list == nullptr || list->buffer == 0u) return;
    ApiLock lock;
    begin_display_list(list->buffer, buffer_size(*list));
}

uint32_t GX2REndDisplayList(RBuffer*) {
    ApiLock lock;
    return end_display_list();
}

void GX2RCallDisplayList(RBuffer* list, uint32_t size) {
    if (list == nullptr || list->buffer == 0u) return;
    ApiLock lock;
    call_display_list(list->buffer, size);
}

void GX2RDirectCallDisplayList(RBuffer* list, uint32_t size) {
    if (list == nullptr || list->buffer == 0u) return;
    ApiLock lock;
    direct_call_display_list(list->buffer, size);
}

} // namespace

CAFE_EXPORT(gx2, GX2RInvalidateMemory, GX2RInvalidateMemory);
CAFE_EXPORT(gx2, GX2RSetAllocator, GX2RSetAllocator);
CAFE_EXPORT(gx2, GX2RGetBufferAlignment, GX2RGetBufferAlignment);
CAFE_EXPORT(gx2, GX2RGetBufferAllocationSize, GX2RGetBufferAllocationSize);
CAFE_EXPORT(gx2, GX2RBufferExists, GX2RBufferExists);
CAFE_EXPORT(gx2, GX2RSetBufferName, GX2RSetBufferName);
CAFE_EXPORT(gx2, GX2RCreateBuffer, GX2RCreateBuffer);
CAFE_EXPORT(gx2, GX2RCreateBufferUserMemory, GX2RCreateBufferUserMemory);
CAFE_EXPORT(gx2, GX2RDestroyBufferEx, GX2RDestroyBufferEx);
CAFE_EXPORT(gx2, GX2RInvalidateBuffer, GX2RInvalidateBuffer);
CAFE_EXPORT(gx2, GX2RLockBufferEx, GX2RLockBufferEx);
CAFE_EXPORT(gx2, GX2RUnlockBufferEx, GX2RUnlockBufferEx);
CAFE_EXPORT(gx2, GX2RSetAttributeBuffer, GX2RSetAttributeBuffer);
CAFE_EXPORT(gx2, GX2RDrawIndexed, GX2RDrawIndexed);
CAFE_EXPORT(gx2, GX2RSetStreamOutBuffer, GX2RSetStreamOutBuffer);
CAFE_EXPORT(gx2, GX2RCreateSurface, GX2RCreateSurface);
CAFE_EXPORT(gx2, GX2RCreateSurfaceUserMemory, GX2RCreateSurfaceUserMemory);
CAFE_EXPORT(gx2, GX2RDestroySurfaceEx, GX2RDestroySurfaceEx);
CAFE_EXPORT(gx2, GX2RSurfaceExists, GX2RSurfaceExists);
CAFE_EXPORT(gx2, GX2RInvalidateSurface, GX2RInvalidateSurface);
CAFE_EXPORT(gx2, GX2RLockSurfaceEx, GX2RLockSurfaceEx);
CAFE_EXPORT(gx2, GX2RUnlockSurfaceEx, GX2RUnlockSurfaceEx);
CAFE_EXPORT(gx2, GX2RBeginDisplayListEx, GX2RBeginDisplayListEx);
CAFE_EXPORT(gx2, GX2REndDisplayList, GX2REndDisplayList);
CAFE_EXPORT(gx2, GX2RCallDisplayList, GX2RCallDisplayList);
CAFE_EXPORT(gx2, GX2RDirectCallDisplayList, GX2RDirectCallDisplayList);

} // namespace cafe::gx2
