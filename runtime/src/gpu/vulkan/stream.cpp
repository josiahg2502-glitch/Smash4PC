// Netplay hosting (host/netplay.h): the TV image for the guests.
#include "gpu/vulkan/renderer.h"
#include "host/netplay.h"

namespace cafe::gpu::vk {

// Each frame the title shows (not the extra frames), while a guest is connected: scaled to the stream's
// size and copied to a host-visible buffer; the encoder's thread waits for the copy, so nothing here waits.
void Renderer::stream_frame() {
    if (!host::netplay::wants_video() || !scan_valid_[0]) return;
    constexpr uint32_t w = host::netplay::kStreamWidth, h = host::netplay::kStreamHeight;
    const VkFormat scan = scan_[0].format;
    const bool bgra = scan == VK_FORMAT_B8G8R8A8_UNORM || scan == VK_FORMAT_B8G8R8A8_SRGB;
    // The scan buffer's own 8-bit format (its bytes are what the screen shows), else RGBA.
    const VkFormat format = bgra || scan == VK_FORMAT_R8G8B8A8_SRGB ? scan : VK_FORMAT_R8G8B8A8_UNORM;
    if (stream_image_.image == VK_NULL_HANDLE || stream_image_.format != format) {
        if (stream_image_.image != VK_NULL_HANDLE) {
            submit(true);
            destroy_image(stream_image_);
        }
        stream_image_ = create_image(format, VK_IMAGE_ASPECT_COLOR_BIT, w, h, 1, 1,
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D);
    }
    if (stream_slots_[0].buffer.buffer == VK_NULL_HANDLE) {
        // Cached memory where there is some: the encoder reads every byte.
        VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < ctx_.memory.memoryTypeCount; ++i) {
            const auto cached = flags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            if ((ctx_.memory.memoryTypes[i].propertyFlags & cached) == cached) {
                flags = cached;
                break;
            }
        }
        for (StreamSlot& slot : stream_slots_) {
            slot.buffer = ctx_.create_buffer(VkDeviceSize{w} * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, flags);
        }
    }
    StreamSlot* slot = nullptr;
    for (StreamSlot& s : stream_slots_) {
        if (!s.busy.load()) {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr) return; // the encoder is behind: this frame is not sent
    end_rendering();
    barrier();
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(scan_[0].width), static_cast<int32_t>(scan_[0].height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    vkCmdBlitImage(cmd(), scan_[0].image, VK_IMAGE_LAYOUT_GENERAL, stream_image_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                   VK_FILTER_LINEAR);
    barrier();
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd(), stream_image_.image, VK_IMAGE_LAYOUT_GENERAL, slot->buffer.buffer, 1, &region);
    VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0,
                         nullptr);
    slot->busy = true;
    const uint64_t value = submit(false);
    host::netplay::video_frame(
        slot->buffer.mapped, bgra,
        [this, value] {
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &timeline_;
            wi.pValues = &value;
            vkWaitSemaphores(ctx_.device, &wi, 1'000'000'000ull);
        },
        [slot] { slot->busy = false; });
}

} // namespace cafe::gpu::vk
