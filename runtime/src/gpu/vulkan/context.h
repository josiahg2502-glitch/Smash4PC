#pragma once

// The Vulkan device the renderer runs on, guest memory as the GPU sees it,
// and the small allocation helpers the renderer uses.
//
// Guest memory (MEM1, MEM2) is imported with VK_EXT_external_memory_host,
// so shaders and transfers read the title's buffers in place and stream-out
// writes land in guest memory: no copies, nothing to keep coherent.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace cafe::gpu::vk {

[[noreturn]] void check_failed(const char* expression, VkResult result, const char* file, int line);
#define VK_CHECK(expression)                                                                                  \
    do {                                                                                                      \
        const VkResult vk_check_result_ = (expression);                                                       \
        if (vk_check_result_ != VK_SUCCESS) ::cafe::gpu::vk::check_failed(#expression, vk_check_result_,       \
                                                                         __FILE__, __LINE__);                 \
    } while (0)

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
};

struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    // A queue of a compute-only family (AMD's asynchronous compute rings):
    // presentation runs there so it never waits behind rendering.
    // UINT32_MAX / null if the GPU has none.
    uint32_t present_family = UINT32_MAX;
    VkQueue present_queue = VK_NULL_HANDLE;
    bool storage_without_format = false;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    bool custom_border_color = false;
    bool validation = false;
    // VK_KHR_present_wait2: the presentation thread learns when each image
    // reaches the screen (surface support is checked separately).
    bool present_wait = false;
    bool surface_capabilities2 = false; // VK_KHR_get_surface_capabilities2
    PFN_vkWaitForPresent2KHR wait_for_present = nullptr;

    PFN_vkGetMemoryHostPointerPropertiesEXT get_memory_host_pointer_properties = nullptr;
    PFN_vkCmdPushDescriptorSetKHR cmd_push_descriptor_set = nullptr;

    // Creates the instance (with the window's surface extensions) and device.
    void init(const std::vector<const char*>& instance_extensions);

    uint32_t memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags);
    void destroy_buffer(Buffer& buffer);
    VkDeviceMemory allocate_image_memory(VkImage image);
    VkFormatFeatureFlags format_features(VkFormat format) const {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(physical, format, &p);
        return p.optimalTilingFeatures;
    }
};

// Guest memory regions imported as buffers.
class GuestMemory {
public:
    void init(Context& context);
    // The buffer and offset holding guest bytes [address, address + size), or
    // false if the range is not in an imported region.
    bool locate(uint32_t address, uint32_t size, VkBuffer& buffer, VkDeviceSize& offset) const;
    // Device address of a guest address (0 if not imported) and how many bytes
    // from there are in the same region.
    VkDeviceAddress device_address(uint32_t address, uint32_t& available) const;

private:
    struct Region {
        uint32_t base;
        uint32_t size;
        Buffer buffer;
    };
    std::vector<Region> regions_;
};

// Host-visible memory the renderer writes per draw and per transfer
// (constants, indices, staging), in chunks used in turn: the renderer moves
// to the next chunk once the GPU has finished every submission that used it.
class UploadRing {
public:
    static constexpr uint32_t kChunks = 8;
    void init(Context& context, VkDeviceSize size);
    // Null when the current chunk is full.
    uint8_t* allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset);
    uint32_t chunk() const { return chunk_; }
    // Makes `count` chunks from `first` the current position, as one
    // allocation of `size` bytes from the start of `first`.
    uint8_t* take_span(uint32_t first, uint32_t count, VkDeviceSize size, VkDeviceSize& offset) {
        chunk_ = first + count - 1;
        used_ = size - (count - 1) * chunk_size();
        offset = first * chunk_size();
        return buffer_.mapped + offset;
    }
    uint32_t next_chunk() const { return (chunk_ + 1) % kChunks; }
    void advance() {
        chunk_ = next_chunk();
        used_ = 0;
    }
    VkDeviceSize chunk_size() const { return buffer_.size / kChunks; }
    VkBuffer buffer() const { return buffer_.buffer; }
    VkDeviceAddress address() const { return buffer_.address; }
    uint8_t* mapped() const { return buffer_.mapped; }

private:
    Buffer buffer_;
    uint32_t chunk_ = 0;
    VkDeviceSize used_ = 0; // in the current chunk
};

} // namespace cafe::gpu::vk
