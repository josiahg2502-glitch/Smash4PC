// Renderer setup, command recording and synchronisation, scan-out and
// presentation.

#include "gpu/vulkan/renderer.h"

#include "gpu/vulkan/png.h"
#include "host/display_settings.h"
#include "host/window.h"

#include "cafe/runtime.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace cafe::gpu {

namespace {
std::atomic<bool> g_trace_requested{false};
} // namespace

void request_frame_trace() { g_trace_requested = true; }

namespace {
std::atomic<uint32_t> g_tv_height{720};
} // namespace

void set_tv_height(uint32_t rows) {
    if (rows != 0 && g_tv_height.exchange(rows) != rows) {
        std::fprintf(stderr, "ttt2: GPU: TV image %u rows: render targets drawn at %ux%u\n", rows,
                     static_cast<uint32_t>(std::lround(rows * 16.0 / 9.0)), rows);
    }
}
uint32_t tv_height() { return g_tv_height; }

std::unique_ptr<Backend> make_vulkan_backend() { return std::make_unique<vk::Renderer>(); }

namespace vk {
namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr VkDeviceSize kRingSize = 256ull << 20;

} // namespace

Renderer::Renderer() {
    ctx_.init(host::vulkan_instance_extensions());
    init_formats(ctx_.physical);
    guest_.init(ctx_);
    ring_.init(ctx_, kRingSize);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(ctx_.device, &sci, nullptr, &acquired_));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = ctx_.queue_family;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(ctx_.device, &pci, nullptr, &pool_));
    for (Slot& slot : slots_) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &cai, &slot.cmd));
    }
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    tsci.pNext = &tci;
    VK_CHECK(vkCreateSemaphore(ctx_.device, &tsci, nullptr, &timeline_));
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 2 * kSlots;
    VK_CHECK(vkCreateQueryPool(ctx_.device, &qci, nullptr, &busy_pool_));

    if (const char* f = std::getenv("TTT2_TRACE_FRAME")) trace_frame_ = std::strtoull(f, nullptr, 10);
    // Render scale: the run's (display.ini's resolution or TTT2_SCALE, set
    // by the window), else enough to cover the display (2 on 1080p and
    // 1440p, 3 on 4K).
    if (const float chosen = host::display::active().scale; chosen > 0) {
        scale_ = chosen;
    } else {
        uint32_t dw = 0, dh = 0;
        host::display_size(dw, dh);
        scale_ = host::display::auto_scale(dh);
    }
    std::fprintf(stderr, "ttt2: GPU: rendering at %ux%u (scale %g)\n", scaled(1280, scale_), scaled(720, scale_),
                 static_cast<double>(scale_));
    compiler_ = shaderc_compiler_initialize();
    load_pipeline_cache();
    async_compile_ = std::getenv("TTT2_SYNC_SHADERS") == nullptr;
    precompile();
    surface_ = host::create_vulkan_surface(ctx_.instance);
    init_interpolation();
    start_presenter();
    window_start_ = now_seconds();
}

// ------------------------------------------------------------- commands

VkCommandBuffer Renderer::cmd() {
    if (!recording_) {
        Slot& slot = slots_[slot_];
        if (slot.value != 0) {
            wait_value(slot.value, 0);
            // Its GPU execution time, for the statistics.
            uint64_t ts[2];
            if (vkGetQueryPoolResults(ctx_.device, busy_pool_, 2 * slot_, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) ==
                    VK_SUCCESS &&
                ts[1] >= ts[0]) {
                busy_seconds_ += (ts[1] - ts[0]) * ctx_.properties.limits.timestampPeriod * 1e-9;
            }
        }
        cmd_ = slot.cmd;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
        recording_ = true;
        vkCmdResetQueryPool(cmd_, busy_pool_, 2 * slot_, 2);
        vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, busy_pool_, 2 * slot_);
    }
    return cmd_;
}

void Renderer::wait_value(uint64_t value, int why) {
    uint64_t done = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(ctx_.device, timeline_, &done));
    if (done >= value) return;
    const double begin = now_seconds();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(ctx_.device, &wi, 10'000'000'000ull);
    if (r != VK_SUCCESS) fatal("Vulkan: the GPU did not finish a submission (%d)", static_cast<int>(r));
    wait_seconds_ += now_seconds() - begin;
    ++waits_;
    ++waits_by_[why];
}

uint64_t Renderer::submit(bool wait) {
    if (recording_) {
        end_rendering();
        copy_write_backs();
        vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, busy_pool_, 2 * slot_ + 1);
        VK_CHECK(vkEndCommandBuffer(cmd_));
        const uint64_t value = ++submitted_;
        VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ti.signalSemaphoreValueCount = 1;
        ti.pSignalSemaphoreValues = &value;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.pNext = &ti;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &timeline_;
        {
            std::lock_guard lock(queue_mutex_);
            VK_CHECK(vkQueueSubmit(ctx_.queue, 1, &si, VK_NULL_HANDLE));
        }
        slots_[slot_].value = value;
        slot_ = (slot_ + 1) % kSlots;
        for (uint32_t c = 0; c < UploadRing::kChunks; ++c) {
            if (chunks_touched_ & (1u << c)) chunk_value_[c] = value;
        }
        chunks_touched_ = 0;
        recording_ = false;
        bound_pipeline_ = nullptr;
        last_submit_ = now_seconds();
    }
    flush_pending_ = false;
    if (wait) wait_value(submitted_, 2);
    return submitted_;
}

// Smash asks for a completion value up to ~250 times a frame (each GX2 submission, timestamps,
// end-of-pipe writes); a Vulkan submission each cost more than the frame's work. Within 1 ms of the
// last submission the request joins the batch being recorded: its value is the one that batch will
// get, and idle() or a later flush submits it.
uint64_t Renderer::flush() {
    if (recording_ && now_seconds() - last_submit_ < 0.001) {
        flush_pending_ = true;
        return submitted_ + 1;
    }
    return submit(false);
}

void Renderer::idle() {
    if (flush_pending_) submit(false);
}

// From the command processor's retirement thread.
void Renderer::wait_for(uint64_t value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(ctx_.device, &wi, 10'000'000'000ull);
    if (r != VK_SUCCESS) fatal("Vulkan: the GPU did not finish a submission (%d)", static_cast<int>(r));
}

void Renderer::sync() { submit(true); }

void Renderer::barrier() {
    stream_out_pending_ = false;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd(), &di);
}

uint8_t* Renderer::upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
    if (uint8_t* p = ring_.allocate(size, alignment, offset)) {
        chunks_touched_ |= 1u << ring_.chunk();
        return p;
    }
    // Bigger than a chunk (a capture or texture at a high render scale): a
    // run of consecutive chunks, once the GPU finished with all of them.
    if (size > ring_.chunk_size()) {
        const uint32_t count = static_cast<uint32_t>((size + ring_.chunk_size() - 1) / ring_.chunk_size());
        if (count > UploadRing::kChunks) {
            fatal("Vulkan: an upload of %llu bytes does not fit the upload ring", static_cast<unsigned long long>(size));
        }
        uint32_t first = ring_.next_chunk();
        if (first + count > UploadRing::kChunks) first = 0;
        bool touched = false;
        for (uint32_t c = first; c < first + count; ++c) touched |= (chunks_touched_ >> c) & 1;
        if (touched) submit(false);
        for (uint32_t c = first; c < first + count; ++c) {
            wait_value(chunk_value_[c], 1);
            chunks_touched_ |= 1u << c;
        }
        return ring_.take_span(first, count, size, offset);
    }
    // The next chunk, once the GPU finished with it (it rarely has not).
    const uint32_t next = ring_.next_chunk();
    if (chunks_touched_ & (1u << next)) submit(false); // the commands being recorded use it
    wait_value(chunk_value_[next], 1);
    ring_.advance();
    if (uint8_t* p = ring_.allocate(size, alignment, offset)) {
        chunks_touched_ |= 1u << ring_.chunk();
        return p;
    }
    fatal("Vulkan: an upload of %llu bytes does not fit an upload chunk", static_cast<unsigned long long>(size));
}

void Renderer::end_rendering() {
    if (!rendering_) return;
    vkCmdEndRendering(cmd_);
    rendering_ = false;
    bound_pipeline_ = nullptr;
    barrier();
}

Image Renderer::create_image(VkFormat format, VkImageAspectFlags aspect, uint32_t width, uint32_t height,
                             uint32_t layers, uint32_t levels, VkImageUsageFlags usage, VkImageViewType view_type,
                             bool shared) {
    Image img;
    img.format = format;
    img.aspect = aspect;
    img.width = std::max(width, 1u);
    img.height = std::max(height, 1u);
    img.layers = std::max(layers, 1u);
    img.levels = std::max(levels, 1u);
    img.view_type = view_type;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                    : (view_type == VK_IMAGE_VIEW_TYPE_1D || view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) ? VK_IMAGE_TYPE_1D
                                                                                                           : VK_IMAGE_TYPE_2D;
    if (view_type == VK_IMAGE_VIEW_TYPE_CUBE || view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    ici.format = format;
    ici.extent = {img.width, ici.imageType == VK_IMAGE_TYPE_1D ? 1 : img.height,
                  ici.imageType == VK_IMAGE_TYPE_3D ? img.layers : 1};
    ici.mipLevels = img.levels;
    ici.arrayLayers = ici.imageType == VK_IMAGE_TYPE_3D ? 1 : img.layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const uint32_t families[2] = {ctx_.queue_family, ctx_.present_family};
    if (shared && ctx_.present_family != UINT32_MAX && ctx_.present_family != ctx_.queue_family) {
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices = families;
    }
    VK_CHECK(vkCreateImage(ctx_.device, &ici, nullptr, &img.image));
    img.memory = ctx_.allocate_image_memory(img.image);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = view_type;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, img.levels, 0, ici.arrayLayers};
    VK_CHECK(vkCreateImageView(ctx_.device, &vci, nullptr, &img.view));

    // Every image lives in the general layout.
    end_rendering();
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img.image;
    ib.subresourceRange = vci.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd(), &di);
    return img;
}

void Renderer::destroy_image(Image& img) {
    // Only for images no recorded command uses.
    if (img.view) vkDestroyImageView(ctx_.device, img.view, nullptr);
    if (img.image) vkDestroyImage(ctx_.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(ctx_.device, img.memory, nullptr);
    img = Image{};
}

// ------------------------------------------------------------- scan-out

void Renderer::copy_to_scan_buffer(const gx2::ColorBuffer& buffer, uint32_t scan_target) {
    const int index = scan_target == gx2::kScanDrc ? 1 : 0;
    Target* t = color_target(buffer);
    if (t == nullptr) return;
    const uint32_t width = std::min(scaled(buffer.surface.width, t->scale), t->image.width);
    const uint32_t height = std::min(scaled(buffer.surface.height, t->scale), t->image.height);
    Image& scan = scan_[index];
    if (scan.image == VK_NULL_HANDLE || scan.width != width || scan.height != height || scan.format != t->image.format) {
        if (scan.image != VK_NULL_HANDLE) {
            submit(true);
            destroy_image(scan);
        }
        scan = create_image(t->image.format, VK_IMAGE_ASPECT_COLOR_BIT, width, height, 1, 1,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_VIEW_TYPE_2D);
    }
    end_rendering();
    barrier();
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    vkCmdCopyImage(cmd(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, scan.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier();
    scan_valid_[index] = true;
    profile_mark("copy to scan buffer", t->address, width, height);
    if (tracing()) {
        std::fprintf(stderr, "trace: copy to scan %u from 0x%08X fmt 0x%03X %ux%u\n", scan_target, t->address, t->format,
                     width, height);
    }
}

// --------------------------------------------------------- presentation

// Saves an 8-bit RGBA/BGRA image as a PNG (debugging); waits for the GPU.
void Renderer::save_image(const Image& img, const std::string& path, bool keep_alpha) {
    // Formats a trace can show: 8-bit, signed 8-bit, 10-bit, half float and
    // depth, decoded to RGBA in [0, 1] (signed and HDR values clamped; the
    // stats line has the real mean and peak).
    enum class Kind { kRgba, kBgra, kSnorm, kSnorm16, kRgb10, kHalf, kRg16, kDepth16, kDepth } kind;
    switch (img.format) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: kind = Kind::kRgba; break;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: kind = Kind::kBgra; break;
    case VK_FORMAT_R8G8B8A8_SNORM: kind = Kind::kSnorm; break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: kind = Kind::kRgb10; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: kind = Kind::kHalf; break;
    case VK_FORMAT_R16G16_UNORM: kind = Kind::kRg16; break;
    case VK_FORMAT_R16G16B16A16_SNORM: kind = Kind::kSnorm16; break;
    case VK_FORMAT_D16_UNORM: kind = Kind::kDepth16; break;
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: kind = Kind::kDepth; break;
    default: return;
    }
    const uint32_t texel = kind == Kind::kHalf || kind == Kind::kSnorm16 ? 8 : kind == Kind::kDepth16 ? 2 : 4;
    const VkDeviceSize bytes = VkDeviceSize{img.width} * img.height * texel;
    VkDeviceSize offset = 0;
    upload(bytes, 16, offset);
    const uint8_t* data = ring_.mapped() + offset;
    end_rendering();
    barrier();
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {kind == Kind::kDepth || kind == Kind::kDepth16 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {img.width, img.height, 1};
    vkCmdCopyImageToBuffer(cmd(), img.image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &region);
    submit(true);
    const auto half = [](uint16_t h) {
        const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        float v = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                  : exponent == 31 ? 65504.0f
                                   : std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
        return sign ? -v : v;
    };
    const size_t count = size_t{img.width} * img.height;
    std::vector<uint8_t> pixels(count * 4);
    double sum[4] = {}, peak = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* t = data + i * texel;
        float c[4];
        switch (kind) {
        case Kind::kRgba: for (int k = 0; k < 4; ++k) c[k] = t[k] / 255.0f; break;
        case Kind::kBgra: c[0] = t[2] / 255.0f; c[1] = t[1] / 255.0f; c[2] = t[0] / 255.0f; c[3] = t[3] / 255.0f; break;
        case Kind::kSnorm: for (int k = 0; k < 4; ++k) c[k] = std::max(static_cast<int8_t>(t[k]) / 127.0f, -1.0f); break;
        case Kind::kRgb10: {
            uint32_t w;
            std::memcpy(&w, t, 4);
            c[0] = (w & 0x3FF) / 1023.0f;
            c[1] = ((w >> 10) & 0x3FF) / 1023.0f;
            c[2] = ((w >> 20) & 0x3FF) / 1023.0f;
            c[3] = (w >> 30) / 3.0f;
            break;
        }
        case Kind::kHalf: {
            uint16_t h[4];
            std::memcpy(h, t, 8);
            for (int k = 0; k < 4; ++k) c[k] = half(h[k]);
            break;
        }
        case Kind::kSnorm16: {
            int16_t h[4];
            std::memcpy(h, t, 8);
            for (int k = 0; k < 4; ++k) c[k] = std::max(h[k] / 32767.0f, -1.0f);
            break;
        }
        case Kind::kRg16: {
            uint16_t h[2];
            std::memcpy(h, t, 4);
            c[0] = h[0] / 65535.0f;
            c[1] = h[1] / 65535.0f;
            c[2] = 0.0f;
            c[3] = 1.0f;
            break;
        }
        case Kind::kDepth16: {
            uint16_t h;
            std::memcpy(&h, t, 2);
            c[0] = c[1] = c[2] = h / 65535.0f;
            c[3] = 1.0f;
            break;
        }
        case Kind::kDepth: {
            float d;
            std::memcpy(&d, t, 4);
            c[0] = c[1] = c[2] = d;
            c[3] = 1.0f;
            break;
        }
        }
        for (int k = 0; k < 3; ++k) {
            sum[k] += c[k];
            peak = std::max(peak, static_cast<double>(c[k]));
            pixels[i * 4 + k] = static_cast<uint8_t>(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        sum[3] += c[3];
        pixels[i * 4 + 3] = keep_alpha ? static_cast<uint8_t>(std::clamp(c[3], 0.0f, 1.0f) * 255.0f + 0.5f) : 255;
    }
    std::fprintf(stderr, "trace: saved %s (vk format %d): mean %.3f %.3f %.3f alpha %.3f, peak %.3f\n", path.c_str(),
                 static_cast<int>(img.format), sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count, peak);
    write_png(path, pixels.data(), img.width, img.height);
}

// TTT2_CAPTURE=<directory>: every TTT2_CAPTURE_INTERVAL seconds (default
// 5) the TV and GamePad images are saved as PNG files, from
// TTT2_CAPTURE_START seconds on (default 0); TTT2_CAPTURE_TV_ONLY=1 saves
// the TV image alone (no GamePad image, no extra frames). The render
// targets of a traced frame (TTT2_TRACE_FRAME/AT) are saved there too.
void Renderer::capture() {
    static const char* dir = [] {
        const char* d = std::getenv("TTT2_CAPTURE");
        return (d != nullptr && *d != '\0') ? d : nullptr;
    }();
    static const double interval = [] {
        const char* v = std::getenv("TTT2_CAPTURE_INTERVAL");
        return v ? std::atof(v) : 5.0;
    }();
    static double last = now_seconds();
    static const double start = [] {
        const char* v = std::getenv("TTT2_CAPTURE_START");
        return now_seconds() + (v ? std::atof(v) : 0.0);
    }();
    static const bool tv_only = std::getenv("TTT2_CAPTURE_TV_ONLY") != nullptr;
    static uint32_t index = 0;
    // TTT2_WATCH_PIXEL=x,y (fractions of the TV image): logs each change of
    // that pixel's colour, read back a frame later without waiting.
    static const char* watch = std::getenv("TTT2_WATCH_PIXEL");
    static const uint8_t* watched = nullptr;
    static uint32_t last_rgba = 0;
    if (watch != nullptr && scan_valid_[0]) {
        if (watched != nullptr) {
            uint32_t rgba;
            std::memcpy(&rgba, watched, 4);
            if (rgba != last_rgba) {
                std::fprintf(stderr, "watch: frame %llu pixel %08X\n", static_cast<unsigned long long>(frame_number_ - 1), rgba);
                last_rgba = rgba;
            }
        }
        const float fx = std::strtof(watch, nullptr), fy = std::strtof(std::strchr(watch, ',') + 1, nullptr);
        VkDeviceSize offset = 0;
        upload(4, 16, offset);
        watched = ring_.mapped() + offset;
        end_rendering();
        barrier();
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {static_cast<int32_t>(fx * scan_[0].width), static_cast<int32_t>(fy * scan_[0].height), 0};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cmd(), scan_[0].image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &region);
    }
    if (dir != nullptr && tracing()) {
        for (const auto& t : targets_) {
            char name[96];
            std::snprintf(name, sizeof(name), "/frame%llu_%s_%08X_%03X_%ux%u.png",
                          static_cast<unsigned long long>(frame_number_), t->depth ? "depth" : "color", t->address,
                          t->format, t->pitch, t->height);
            save_image(t->image, std::string(dir) + name);
        }
    }
    if (dir == nullptr || now_seconds() < start || now_seconds() - last < interval) return;
    last = now_seconds();
    for (int s = 0; s < 2; ++s) {
        if (!scan_valid_[s] || (tv_only && s == 1)) continue;
        char name[64];
        std::snprintf(name, sizeof(name), "/%s_%04u.png", s == 0 ? "tv" : "drc", index);
        save_image(scan_[s], std::string(dir) + name);
    }
    captured_frame_ = tv_only ? -1 : static_cast<int64_t>(frame_number_);
    captured_index_ = index;
    ++index;
    // TTT2_CAPTURE_SEQUENCE=<frames>: also every image shown for the next
    // frames, in the order shown, as seq_<index>_<frame>_<n>.png (the real
    // image is the last n of its frame).
    static const uint32_t sequence = [] {
        const char* v = std::getenv("TTT2_CAPTURE_SEQUENCE");
        return v ? static_cast<uint32_t>(std::atoi(v)) : 0u;
    }();
    if (sequence_left_ == 0 && sequence > 0) {
        sequence_left_ = sequence;
        sequence_frame_ = 0;
        sequence_index_ = captured_index_;
    }
}

void Renderer::save_sequence_image(uint32_t n) {
    char name[64];
    std::snprintf(name, sizeof(name), "/seq_%04u_%03u_%u.png", sequence_index_, sequence_frame_, n);
    save_image(scan_[0], std::string(std::getenv("TTT2_CAPTURE")) + name);
}

// ------------------------------------------------------------ profiling

void Renderer::profile_mark(const char* what, uint32_t a, uint32_t b, uint32_t c) {
    if (!profiling_ || profile_marks_.size() >= 8191) return;
    vkCmdWriteTimestamp2(cmd(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, profile_pool_,
                         static_cast<uint32_t>(profile_marks_.size()));
    profile_marks_.push_back({what, a, b, c});
}

void Renderer::profile_report() {
    const uint32_t n = static_cast<uint32_t>(profile_marks_.size());
    std::vector<uint64_t> ts(n);
    if (n < 2 || vkGetQueryPoolResults(ctx_.device, profile_pool_, 0, n, n * 8, ts.data(), 8,
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
        return;
    }
    const double ns = ctx_.properties.limits.timestampPeriod;
    struct Item {
        double ms;
        uint32_t index;
    };
    std::vector<Item> items;
    std::map<std::string, std::pair<double, uint32_t>> by_kind;
    double total = 0;
    for (uint32_t i = 1; i < n; ++i) {
        // Marks in different submissions are not comparable when the GPU idled between them.
        const double ms = ts[i] >= ts[i - 1] ? (ts[i] - ts[i - 1]) * ns / 1e6 : 0.0;
        items.push_back({ms, i});
        auto& k = by_kind[profile_marks_[i].what];
        k.first += ms;
        k.second += 1;
        total += ms;
    }
    std::fprintf(stderr, "profile: frame %llu, %u operations, %.2f ms of GPU time\n",
                 static_cast<unsigned long long>(frame_number_), n - 1, total);
    for (const auto& [what, v] : by_kind) {
        std::fprintf(stderr, "profile:   %-28s %5u x  %7.3f ms\n", what.c_str(), v.second, v.first);
    }
    std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.ms > y.ms; });
    for (size_t i = 0; i < std::min<size_t>(items.size(), 40); ++i) {
        const ProfileMark& m = profile_marks_[items[i].index];
        std::fprintf(stderr, "profile:   #%-4u %7.3f ms  %s 0x%08X 0x%08X 0x%08X\n", items[i].index, items[i].ms, m.what,
                     m.a, m.b, m.c);
    }
}

void Renderer::swap() {
    copy_write_backs(); // the frame's, before its replays render into the same targets
    capture();
    show_frame();
    if (profiling_) {
        submit(true);
        profile_report();
        profiling_ = false;
    }
    static const double profile_at = [] {
        const char* v = std::getenv("TTT2_PROFILE_AT");
        return v ? std::atof(v) : -1.0;
    }();
    static const double profile_start = now_seconds();
    if (profile_at >= 0 && profile_frame_ == UINT64_MAX && now_seconds() - profile_start >= profile_at) {
        profile_frame_ = frame_number_ + 1;
        if (profile_pool_ == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 8192;
            VK_CHECK(vkCreateQueryPool(ctx_.device, &qci, nullptr, &profile_pool_));
        }
        vkCmdResetQueryPool(cmd(), profile_pool_, 0, 8192);
        profile_marks_.clear();
        profiling_ = true;
        profile_mark("start");
    }
    if (tracing()) std::fprintf(stderr, "trace: swap (end of frame %llu)\n", static_cast<unsigned long long>(frame_number_));
    // TTT2_TRACE_AT=seconds: trace the first frame after that time.
    static const double trace_at = [] {
        const char* v = std::getenv("TTT2_TRACE_AT");
        return v ? std::atof(v) : -1.0;
    }();
    static const double started = now_seconds();
    if (trace_at >= 0 && trace_frame_ == UINT64_MAX && now_seconds() - started >= trace_at) {
        trace_frame_ = frame_number_ + 1;
    }
    if (g_trace_requested.exchange(false)) {
        trace_frame_ = frame_number_ + 1;
        std::fprintf(stderr, "ttt2: gpu: tracing frame %llu\n", static_cast<unsigned long long>(trace_frame_));
    }
    ++frame_number_;
    ++frames_;
    note_live_frame();
    const double t = now_seconds();
    if (t - window_start_ >= 10.0) {
        save_pipeline_cache();
        std::fprintf(stderr, "ttt2: gpu: %.1f frames/s, %.0f draws per frame (%.0f skipped), %zu targets, %zu textures, "
                             "%zu shaders, %zu pipelines; %.1f GPU waits, %.1f ms waiting (%.1f ms GPU busy) per frame; %llu/%llu extra frames blended (%llu skipped: GPU behind), %.1f MB recorded; replay %.1f ms (matching %.1f)\n",
                     frames_ / (t - window_start_), double(draws_) / std::max<uint64_t>(frames_, 1),
                     double(skipped_draws_) / std::max<uint64_t>(frames_, 1), targets_.size(), textures_.size(),
                     shaders_.size(), pipelines_.size(), double(waits_) / std::max<uint64_t>(frames_, 1),
                     wait_seconds_ * 1000.0 / std::max<uint64_t>(frames_, 1), busy_seconds_ * 1000.0 / std::max<uint64_t>(frames_, 1),
                     static_cast<unsigned long long>(blended_replays_),
                     static_cast<unsigned long long>(replays_), static_cast<unsigned long long>(skipped_replays_),
                     frame_records_[current_record_ ^ 1].used / 1e6,
                     replay_seconds_ * 1000.0 / std::max<uint64_t>(frames_, 1), match_seconds_ * 1000.0 / std::max<uint64_t>(frames_, 1));
        const auto line = [](const char* what, const Spent& s) {
            std::fprintf(stderr, " %s %u: %.0f ms (longest %.1f);", what, s.count, s.seconds * 1000.0, s.longest * 1000.0);
        };
        std::fprintf(stderr, "ttt2: gpu: draw thread:");
        line("translate", spent_translate_);
        line("compile", spent_compile_);
        line("pipelines", spent_pipeline_);
        line("texture binds", spent_texture_);
        line("texture uploads", spent_upload_);
        std::fprintf(stderr, " %llu draws deferred while compiling;", static_cast<unsigned long long>(deferred_draws_));
        deferred_draws_ = 0;
        const double per_frame = 1.0 / std::max<uint64_t>(frames_, 1);
        std::fprintf(stderr, " per frame: %.1f submissions, waits for a slot %.1f, an upload chunk %.1f, a sync %.1f, other %.1f;",
                     (submitted_ - submits_counted_) * per_frame, waits_by_[0] * per_frame, waits_by_[1] * per_frame,
                     waits_by_[2] * per_frame, waits_by_[3] * per_frame);
        submits_counted_ = submitted_;
        for (uint64_t& w : waits_by_) w = 0;
        std::fprintf(stderr, "\n");
        spent_translate_ = spent_compile_ = spent_pipeline_ = spent_texture_ = spent_upload_ = {};
        if (!swap_delays_.empty()) {
            std::sort(swap_delays_.begin(), swap_delays_.end());
            const auto pct = [&](size_t p) { return swap_delays_[std::min(swap_delays_.size() - 1, swap_delays_.size() * p / 100)]; };
            static const bool trace = std::getenv("TTT2_TRACE_PACING") != nullptr;
            if (trace) {
                std::fprintf(stderr, "pacing: swap after its vblank 50%% %.1f, 90%% %.1f, 99%% %.1f, max %.1f ms\n", pct(50),
                             pct(90), pct(99), swap_delays_.back());
            }
            swap_delays_.clear();
        }
        replay_seconds_ = match_seconds_ = 0;
        replays_ = blended_replays_ = skipped_replays_ = 0;
        window_start_ = t;
        frames_ = draws_ = skipped_draws_ = waits_ = 0;
        wait_seconds_ = busy_seconds_ = 0;
    }
}

} // namespace vk
} // namespace cafe::gpu
