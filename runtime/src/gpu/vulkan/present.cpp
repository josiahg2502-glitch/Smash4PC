// Presentation: finished frames go to the window from a thread of their own.
//
// The command processor copies the TV scan buffer into one of a few
// presentation images and queues it with the time it should appear; the
// presentation thread waits until the GPU has finished it and the time has
// come, then scales it, letterboxed, into the swapchain and presents. So
// the command processor never waits for the display, and with frame
// interpolation the extra frames appear between the real ones at even
// intervals: the extra frames of a frame first, its real image last,
// 1/(59.94 * frames_per_frame_) s apart.
//
// When the display refreshes at a multiple of 60 Hz (and the driver tells
// when presents reach the screen: present waits), presentation follows the
// display instead: the thread presents at every refresh in FIFO order, the
// title's vblanks come from every 60th of a second of those refreshes, and
// each frame's images are due at fixed refreshes after the vblank its frame
// started from (present_every_refresh). So on a fixed-rate display every
// image stays on screen for the same number of refreshes, with nothing
// drifting against the display. Otherwise frames are shown at times
// (present_timed).
//
// Presentation runs on a queue of its own, a compute queue (AMD's
// asynchronous compute), with a small compute shader doing the scaling: on
// the rendering queue a present waits for everything submitted before it,
// which by then includes the next frame. Without such a queue it falls back
// to a blit on the rendering queue (queue_mutex_ serialises its use).

#include "gpu/vulkan/renderer.h"

#include "gpu/gpu.h"
#include "host/window.h"

#include "cafe/runtime.h"

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shaderc/shaderc.h>
#include <thread>
#include <vector>

namespace cafe::gpu::vk {
namespace {

// Enough for every image waiting for its refresh (up to a frame's lead
// ahead), the one showing and the one being copied.
constexpr uint32_t kPresentImages = 12;
// One frame of the title: the console's 59.94 Hz.
constexpr double kFramePeriod = 1001.0 / 60000.0;

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}


} // namespace

// The compute shader that scales a presentation image into the swapchain.
constexpr const char* kScaleShader = R"(#version 460
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) writeonly uniform image2D target;
layout(set = 0, binding = 2) readonly buffer Overlay { uint pixels[]; } overlay;
layout(push_constant) uniform Rect {
    ivec2 offset; ivec2 size; ivec2 extent;
    float dim; float unused; ivec4 layers[2]; uvec2 base; vec2 opacity;
} rect;
void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, rect.extent))) return;
    const ivec2 p = xy - rect.offset;
    vec4 c = vec4(0.0, 0.0, 0.0, 1.0);
    if (all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, rect.size))) {
        c = vec4(textureLod(source, (vec2(p) + 0.5) / vec2(rect.size), 0.0).rgb, 1.0);
    }
    c.rgb *= 1.0 - rect.dim;
    for (int i = 0; i < 2; ++i) { // premultiplied, over the picture
        const ivec2 q = xy - rect.layers[i].xy;
        if (all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, rect.layers[i].zw))) {
            const vec4 o = unpackUnorm4x8(overlay.pixels[rect.base[i] + uint(q.y * rect.layers[i].z + q.x)]) * rect.opacity[i];
            c.rgb = c.rgb * (1.0 - o.a) + o.rgb;
        }
    }
    imageStore(target, xy, c);
}
)";

// gpu::set_overlay's layers, until the presentation thread takes them.
namespace {
std::mutex g_overlay_mutex;
float g_overlay_dim = 0;
std::vector<gpu::OverlayLayer> g_overlay_layers;
uint64_t g_overlay_version = 0;
} // namespace

// gpu::live_stats, every half second.
std::atomic<float> g_shown_fps{0}, g_worst_ms{0}, g_game_fps{0}, g_gpu_ms{0}, g_draws{0};

void Renderer::note_live_frame() {
    static double start = now_seconds(), busy = 0;
    static uint64_t frames = 0, draws = 0;
    static double busy_seen = 0;
    static uint64_t draws_seen = 0;
    // (The ten-second statistics restart from 0.)
    busy += busy_seconds_ >= busy_seen ? busy_seconds_ - busy_seen : busy_seconds_;
    draws += draws_ >= draws_seen ? draws_ - draws_seen : draws_;
    busy_seen = busy_seconds_;
    draws_seen = draws_;
    ++frames;
    const double t = now_seconds();
    if (t - start < 0.5) return;
    g_game_fps = static_cast<float>(frames / (t - start));
    g_gpu_ms = static_cast<float>(busy * 1000.0 / frames);
    g_draws = static_cast<float>(double(draws) / frames);
    start = t;
    busy = 0;
    frames = draws = 0;
}

// The presentation thread, after the last present's fence: the buffer is free.
void Renderer::update_overlay() {
    std::lock_guard lock(g_overlay_mutex);
    if (overlay_version_ == g_overlay_version) return;
    overlay_version_ = g_overlay_version;
    overlay_push_ = {};
    overlay_push_.dim = g_overlay_dim;
    size_t total = 0;
    for (const gpu::OverlayLayer& l : g_overlay_layers) total += l.pixels.size();
    if (total * 4 > overlay_buffer_.size) {
        ctx_.destroy_buffer(overlay_buffer_);
        overlay_buffer_ = ctx_.create_buffer(total * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    uint32_t base = 0;
    for (size_t i = 0; i < g_overlay_layers.size() && i < 2; ++i) {
        const gpu::OverlayLayer& l = g_overlay_layers[i];
        if (l.pixels.size() < size_t{l.width} * l.height) continue;
        std::memcpy(overlay_buffer_.mapped + size_t{base} * 4, l.pixels.data(), l.pixels.size() * 4);
        overlay_push_.layers[i][0] = l.x;
        overlay_push_.layers[i][1] = l.y;
        overlay_push_.layers[i][2] = static_cast<int32_t>(l.width);
        overlay_push_.layers[i][3] = static_cast<int32_t>(l.height);
        overlay_push_.base[i] = base;
        overlay_push_.opacity[i] = l.opacity;
        base += static_cast<uint32_t>(l.pixels.size());
    }
}

void Renderer::start_presenter() {
    // The compute path needs a presenting compute queue and storage writes
    // to the swapchain.
    VkBool32 supported = VK_FALSE;
    if (ctx_.present_queue != VK_NULL_HANDLE) {
        vkGetPhysicalDeviceSurfaceSupportKHR(ctx_.physical, ctx_.present_family, surface_, &supported);
    }
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical, surface_, &caps));
    compute_present_ = supported && ctx_.storage_without_format && (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) &&
                       !std::getenv("TTT2_PRESENT_BLIT");
    // Whether the surface tells when presents reach the screen (needed to
    // present every refresh; the mode itself is chosen by the presentation
    // thread, for the display the window is on).
    if (ctx_.present_wait) {
        const auto get_caps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            vkGetInstanceProcAddr(ctx_.instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
        VkSurfaceCapabilitiesPresentWait2KHR wait2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR};
        VkSurfaceCapabilitiesPresentId2KHR id2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
        id2.pNext = &wait2;
        VkSurfaceCapabilities2KHR caps2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        caps2.pNext = &id2;
        VkPhysicalDeviceSurfaceInfo2KHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        info.surface = surface_;
        present_waits_ = get_caps2 != nullptr && get_caps2(ctx_.physical, &info, &caps2) == VK_SUCCESS &&
                         id2.presentId2Supported && wait2.presentWait2Supported;
    }
    present_family_ = compute_present_ ? ctx_.present_family : ctx_.queue_family;
    present_queue_handle_ = compute_present_ ? ctx_.present_queue : ctx_.queue;
    choose_presentation(host::window_refresh_rate());
    if (compute_present_) {
        shaderc_compile_options_t options = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler_, kScaleShader, std::strlen(kScaleShader),
                                                                       shaderc_compute_shader, "scale", "main", options);
        shaderc_compile_options_release(options);
        if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
            fatal("Vulkan: the presentation shader does not compile: %s", shaderc_result_get_error_message(result));
        }
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = shaderc_result_get_length(result);
        mci.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(result));
        VkShaderModule module;
        VK_CHECK(vkCreateShaderModule(ctx_.device, &mci, nullptr, &module));
        shaderc_result_release(result);
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        dli.bindingCount = 3;
        dli.pBindings = bindings;
        VkDescriptorSetLayout set_layout;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device, &dli, nullptr, &set_layout));
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24 + sizeof(OverlayPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(ctx_.device, &pli, nullptr, &scale_layout_));
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main",
                     nullptr};
        cpi.layout = scale_layout_;
        VK_CHECK(vkCreateComputePipelines(ctx_.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &scale_pipeline_));
        vkDestroyShaderModule(ctx_.device, module, nullptr);
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(ctx_.device, &sci, nullptr, &scale_sampler_));
        overlay_buffer_ = ctx_.create_buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = present_family_;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx_.device, &pci, nullptr, &present_pool_));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = present_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &cai, &present_cmd_));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(ctx_.device, &fci, nullptr, &present_fence_));
    std::thread([this] { presenter_main(); }).detach();
}

// A free presentation image the size and format of the TV scan buffer.
uint32_t Renderer::take_present_image() {
    const Image& scan = scan_[0];
    std::unique_lock lock(present_mutex_);
    // Display-ready values: kept as they are (sRGB data in a UNORM image).
    const VkFormat format = scan.format == VK_FORMAT_R8G8B8A8_SRGB   ? VK_FORMAT_R8G8B8A8_UNORM
                            : scan.format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM
                                                                     : scan.format;
    const bool fits = !present_images_.empty() && present_images_[0].width == scan.width &&
                      present_images_[0].height == scan.height && present_images_[0].format == format;
    if (!fits) {
        // Wait until the presentation thread has let go of the old images
        // (presenting every refresh, it keeps showing one until told).
        present_release_ = true;
        present_cv_.notify_all();
        present_cv_.wait(lock, [&] {
            return present_queue_.empty() && std::all_of(present_free_.begin(), present_free_.end(), [](uint8_t f) { return f; });
        });
        present_release_ = false;
        submit(true);
        for (Image& img : present_images_) destroy_image(img);
        present_images_.clear();
        for (uint32_t i = 0; i < kPresentImages; ++i) {
            present_images_.push_back(create_image(format, VK_IMAGE_ASPECT_COLOR_BIT, scan.width, scan.height, 1, 1,
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                       VK_IMAGE_USAGE_SAMPLED_BIT,
                                                   VK_IMAGE_VIEW_TYPE_2D, compute_present_));
        }
        present_free_.assign(kPresentImages, 1);
    }
    // None free (the display stalled: a hidden window): the oldest image
    // still waiting to be shown, which a newer one supersedes anyway.
    const auto free = [&] { return std::find(present_free_.begin(), present_free_.end(), 1) != present_free_.end(); };
    if (!present_cv_.wait_for(lock, std::chrono::milliseconds(20), free) && !present_queue_.empty()) {
        const uint32_t i = present_queue_.front().image;
        present_queue_.pop_front();
        stolen_images_.fetch_add(1, std::memory_order_relaxed);
        return i;
    }
    present_cv_.wait(lock, free);
    const uint32_t i = static_cast<uint32_t>(std::find(present_free_.begin(), present_free_.end(), 1) - present_free_.begin());
    present_free_[i] = 0;
    return i;
}

// Copies the TV scan buffer into a presentation image; `ready` is the
// timeline value at which the copy has finished.
uint32_t Renderer::copy_scan_to_present_image(uint64_t& ready) {
    const uint32_t i = take_present_image();
    end_rendering();
    barrier();
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {scan_[0].width, scan_[0].height, 1};
    vkCmdCopyImage(cmd(), scan_[0].image, VK_IMAGE_LAYOUT_GENERAL, present_images_[i].image, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &region);
    ready = submit(false);
    last_image_ready_ = ready;
    return i;
}

void Renderer::queue_present(const QueuedFrame& f) {
    std::lock_guard lock(present_mutex_);
    present_queue_.push_back(f);
    present_cv_.notify_all();
}

void Renderer::queue_present(uint32_t image, double time, uint64_t ready) {
    QueuedFrame f{};
    f.image = image;
    f.time = time;
    f.ready = ready;
    f.reference = slot_reference_;
    queue_present(f);
}

// Presenting every refresh: image `n` of the frame (its extra frames first,
// its real image last) is due n refresh steps after the frame's first
// image, which is due a lead after the title vblank the frame started from.
int64_t Renderer::frame_due() {
    frame_base_tick_ = tick_refresh_.load();
    if (const double tick_time = tick_time_.load(); tick_time != 0) {
        swap_delays_.push_back(static_cast<float>((now_seconds() - tick_time) * 1000.0));
    }
    return frame_base_tick_ + refresh_lead_.load();
}

void Renderer::queue_frame_image(uint32_t image, uint64_t ready, uint32_t n, bool first) {
    QueuedFrame f{};
    f.image = image;
    f.ready = ready;
    f.offset = static_cast<int32_t>(n * (refreshes_per_frame_.load() / frames_per_frame_));
    f.due = frame_base_due_ + f.offset;
    f.tick = frame_base_tick_;
    f.first = first;
    queue_present(f);
}

// Frame pacing. Every frame the title renders gets a display slot: one
// title frame (1/59.94 s) after the previous slot, or, if the title fell
// behind, a lead time after its swap. Its extra frames show in the slot at
// even steps and its real image last, so the screen gets a frame every
// 1/(59.94 K) s however unevenly the frames finish. The lead time is the
// time a frame needs to finish on the GPU: it grows when the presentation
// thread finds a frame not ready at its time and shrinks slowly while every
// frame is early.
double Renderer::frame_slot() {
    const double now = now_seconds();
    slot_reference_ = now;
    const double lead = present_lead_.load();
    double slot = last_slot_ + kFramePeriod;
    if (slot < now + lead - kFramePeriod / 2 || slot > now + lead + kFramePeriod) slot = now + lead; // behind, or a stall
    slot = std::max(slot, now + lead * 0.5);
    last_slot_ = slot;
    return slot;
}

// The title finished a frame (swap): shown in its slot, or, with
// interpolation, after the extra frames the replays render.
void Renderer::show_frame() {
    previous_frame_ready_ = last_image_ready_;
    if (!scan_valid_[0]) return;
    sequence_saving_ = sequence_left_ > 0;
    if (sequence_saving_) {
        --sequence_left_;
        ++sequence_frame_;
        save_sequence_image(frames_per_frame_);
    }
    uint64_t ready = 0;
    const uint32_t image = copy_scan_to_present_image(ready);
    if (frames_per_frame_ == 1) {
        if (every_refresh_) {
            frame_base_due_ = frame_due();
            queue_frame_image(image, ready, 0, true);
        } else {
            queue_present(image, frame_slot(), ready);
        }
        return;
    }
    if (pending_real_ != UINT32_MAX) { // a frame without replays
        if (every_refresh_) {
            frame_base_due_ = frame_due();
            queue_frame_image(pending_real_, pending_ready_, frames_per_frame_ - 1, true);
        } else {
            queue_present(pending_real_, frame_slot() + (frames_per_frame_ - 1) * kFramePeriod / frames_per_frame_,
                          pending_ready_);
        }
    }
    pending_real_ = image;
    pending_ready_ = ready;
    extra_frames_ = 0;
}

// A replay finished: the extra frames go out first in the frame's slot,
// 1/K of a frame apart.
void Renderer::show_extra_frame() {
    if (!scan_valid_[0]) return;
    // TTT2_CAPTURE: the extra frames of a captured frame as tvx_<index>_<n>.png.
    if (captured_frame_ >= 0 && static_cast<uint64_t>(captured_frame_) + 1 == frame_number_) {
        if (const char* dir = std::getenv("TTT2_CAPTURE")) {
            char name[64];
            std::snprintf(name, sizeof(name), "/tvx_%04u_%u.png", captured_index_, extra_frames_ + 1);
            save_image(scan_[0], std::string(dir) + name);
        }
    }
    if (sequence_saving_) save_sequence_image(extra_frames_ + 1);
    uint64_t ready = 0;
    const uint32_t image = copy_scan_to_present_image(ready);
    if (every_refresh_) {
        if (extra_frames_ == 0) frame_base_due_ = frame_due();
        queue_frame_image(image, ready, extra_frames_, extra_frames_ == 0);
    } else {
        if (extra_frames_ == 0) frame_base_time_ = frame_slot();
        // (Uncapped: as soon as it is ready.)
        queue_present(image, uncapped_ ? 0.0 : frame_base_time_ + extra_frames_ * kFramePeriod / frames_per_frame_, ready);
    }
    ++extra_frames_;
}

void Renderer::finish_frame_presentation() {
    if (pending_real_ == UINT32_MAX) return;
    if (every_refresh_) {
        if (extra_frames_ == 0) frame_base_due_ = frame_due();
        queue_frame_image(pending_real_, pending_ready_, frames_per_frame_ - 1, extra_frames_ == 0);
    } else {
        // (Without extra frames the real image still comes last in its slot.)
        const double start = extra_frames_ ? frame_base_time_ : frame_slot();
        const double time = start + (frames_per_frame_ - 1) * kFramePeriod / frames_per_frame_;
        queue_present(pending_real_, uncapped_ ? 0.0 : time, pending_ready_);
    }
    pending_real_ = UINT32_MAX;
}

// ---------------------------------------------------------- presentation thread

// Pacing statistics, every ten seconds: the spacing of presents and how late
// they are against their scheduled time (presentation thread only).
void Renderer::note_presentation(double scheduled, double presented) {
    static std::vector<double> intervals;
    static double last = 0, window = presented, late_total = 0;
    static uint32_t late = 0;
    if (last != 0) intervals.push_back((presented - last) * 1000.0);
    // TTT2_TRACE_STALLS=1: each gap over 100 ms between presents, on the
    // steady clock (seconds), to line up with TTT2_TRACE_FS.
    static const bool trace_stalls = std::getenv("TTT2_TRACE_STALLS") != nullptr;
    if (trace_stalls && last != 0 && presented - last > 0.1) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(stderr, "stall: %.3f %.0f ms\n", now, (presented - last) * 1000.0);
    }
    {
        static double live_start = presented, worst = 0;
        static uint32_t shown = 0;
        if (last != 0) worst = std::max(worst, presented - last);
        ++shown;
        if (presented - live_start >= 0.5) {
            g_shown_fps = static_cast<float>(shown / (presented - live_start));
            g_worst_ms = static_cast<float>(worst * 1000.0);
            live_start = presented;
            worst = 0;
            shown = 0;
        }
    }
    last = presented;
    const double lateness = (presented - scheduled) * 1000.0;
    late_total += std::max(lateness, 0.0);
    if (lateness > 2.0) ++late;
    if (presented - window < 10.0 || intervals.empty()) return;
    std::vector<double> sorted = intervals;
    std::sort(sorted.begin(), sorted.end());
    double mean = 0;
    for (double v : sorted) mean += v;
    mean /= sorted.size();
    const auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))]; };
    std::fprintf(stderr,
                 "ttt2: present: %.1f/s, interval mean %.2f ms, 5%% %.2f, 50%% %.2f, 95%% %.2f, max %.2f; "
                 "%u more than 2 ms late, %.2f ms late on average; lead %.1f ms\n",
                 sorted.size() / (presented - window), mean, pct(0.05), pct(0.5), pct(0.95), sorted.back(), late,
                 late_total / (sorted.size() + 1), present_lead_.load() * 1000.0);
    intervals.clear();
    late = 0;
    late_total = 0;
    window = presented;
}

// How to present on a display refreshing `hz` times a second: at every
// refresh when the surface tells when presents reach the screen, the
// display refreshes a whole number of times (two or more) per title frame
// and the frame's images divide them evenly; else at times. (At 60 Hz
// presenting every refresh would only add its queue's latency.)
void Renderer::choose_presentation(double hz) {
    const uint32_t per_frame = static_cast<uint32_t>(std::lround(hz / 60.0));
    const bool every = !uncapped_ && present_waits_ && compute_present_ && per_frame >= 2 &&
                       std::fabs(hz / 60.0 - per_frame) < 0.01 * per_frame && per_frame % frames_per_frame_ == 0 &&
                       !std::getenv("TTT2_PRESENT_TIMED");
    if (every) {
        refreshes_per_frame_.store(per_frame);
        refresh_period_ = 1.0 / hz;
    }
    every_refresh_.store(every);
    static double printed_hz = 0;
    static bool printed_every = false;
    if (hz == printed_hz && every == printed_every) return;
    printed_hz = hz;
    printed_every = every;
    std::fprintf(stderr, "ttt2: GPU: presenting from %s", compute_present_ ? "a compute queue" : "the rendering queue");
    if (every) {
        std::fprintf(stderr, ", every refresh of a %.2f Hz display (%u per title frame)\n", hz, per_frame);
    } else {
        std::fprintf(stderr, " at times (a %.2f Hz display%s)\n", hz, present_waits_ ? "" : ", no present waits");
    }
}

void Renderer::presenter_main() {
    pthread_setname_np(pthread_self(), "present");
    // The mode follows the window's display: chosen again when the window
    // moves to another display or its mode changes, or when the display
    // turns out to refresh at another rate than reported.
    for (;;) {
        const float reported = host::window_refresh_rate();
        choose_presentation(measured_hz_ > 0 ? measured_hz_ : reported);
        if (every_refresh_) {
            present_every_refresh(reported);
        } else {
            gpu::host_vsync_stopped(); // the title's timer takes over at once
            present_timed(reported);
        }
        if (host::window_refresh_rate() != reported) measured_hz_ = 0;
    }
}

void Renderer::present_timed(float reported_hz) {
    while (host::window_refresh_rate() == reported_hz) {
        QueuedFrame f;
        {
            std::unique_lock lock(present_mutex_);
            present_cv_.wait(lock, [&] { return !present_queue_.empty(); });
            f = present_queue_.front();
            present_queue_.pop_front();
        }
        // Finished on the GPU first, then shown at its time.
        uint64_t done = 0;
        VK_CHECK(vkGetSemaphoreCounterValue(ctx_.device, timeline_, &done));
        const bool waited = done < f.ready;
        if (waited) {
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &timeline_;
            wi.pValues = &f.ready;
            VK_CHECK(vkWaitSemaphores(ctx_.device, &wi, 10'000'000'000ull));
        }
        const double ready_at = now_seconds();
        // The lead is how long frames take from their swap until the GPU has
        // finished them: the 98th percentile of the last 240, plus a
        // millisecond (stalls over 30 ms, such as loading, are not measures).
        static std::vector<double> needs;
        static size_t next_need = 0;
        // (A frame already finished when its turn came needed no more lead.)
        const double need = waited ? ready_at - f.reference : 0.0;
        if (need >= 0 && need < 0.030) {
            if (needs.size() < 240) needs.push_back(need);
            else needs[next_need++ % needs.size()] = need;
            if (needs.size() >= 30 && next_need % 30 == 0) {
                std::vector<double> sorted = needs;
                std::sort(sorted.begin(), sorted.end());
                const double p98 = sorted[sorted.size() * 98 / 100];
                present_lead_.store(std::clamp(p98 + 0.001, 0.001, 0.0167));
                static const bool trace = std::getenv("TTT2_TRACE_PACING") != nullptr;
                if (trace && next_need % 240 == 0) {
                    const auto at = [&](int p) { return sorted[std::min(sorted.size() - 1, sorted.size() * p / 100)] * 1000; };
                    size_t zero = 0;
                    for (double v : sorted) zero += v == 0;
                    std::fprintf(stderr, "pacing: need 50%% %.2f 90%% %.2f 98%% %.2f 99%% %.2f max %.2f ms; %zu/%zu ready in time\n",
                                 at(50), at(90), at(98), at(99), sorted.back() * 1000, zero, sorted.size());
                }
            }
        }
        if (f.time == 0) f.time = now_seconds(); // queued while presenting every refresh: now
        const double wait = f.time - now_seconds();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait, 0.1)));
        present_image(f.image, f.ready);
        note_presentation(f.time, now_seconds());
        {
            std::lock_guard lock(present_mutex_);
            present_free_[f.image] = 1;
            present_cv_.notify_all();
        }
    }
}

// Presenting every refresh. FIFO presentation shows one image per refresh
// and drops none; the thread keeps a few presents ahead of the screen and,
// each time the oldest reaches it (a present wait), queues the next: the
// newest complete image due at the refresh it will show at, else the one
// already showing again. Every refreshes_per_frame_-th refresh is a title
// vblank; a frame's images are due a lead after the vblank its frame
// started from, the lead being what frames have needed (all but the
// slowest 1% over the last ten seconds, more at once when frames keep
// coming late). Refreshes are counted one per present, and by time from
// the last present the thread waited for (a refresh the screen showed
// twice because a present came late); the time between such presents also
// checks the display's rate. It returns when the mode should be chosen
// again.
void Renderer::present_every_refresh(float reported_hz) {
    const int64_t per_frame = refreshes_per_frame_.load();
    const double period = refresh_period_;
    // Presents kept ahead: enough for about 8 ms, what KWin needs between a
    // commit and the refresh that shows it plus how late it tells (3 at
    // 240 Hz, 2 at 120 Hz).
    static const char* queue_override = std::getenv("TTT2_PRESENT_QUEUE");
    const size_t queued = queue_override ? static_cast<size_t>(std::clamp(std::atoi(queue_override), 1, 4))
                                         : static_cast<size_t>(std::clamp(1 + std::ceil(0.008 / period), 2.0, 4.0));
    const uint64_t stall_ns = static_cast<uint64_t>(std::max(0.025, (queued + 1) * period) * 1e9);
    refresh_lead_.store(static_cast<int32_t>(per_frame + queued - 1));
    {
        // Images queued in the other mode: due at once.
        std::lock_guard lock(present_mutex_);
        for (QueuedFrame& f : present_queue_) {
            f.due = INT64_MIN;
            f.first = false;
        }
    }
    std::deque<uint64_t> in_flight; // presents not yet seen on screen
    uint64_t generation = swapchain_generation_;
    int64_t& refresh = refresh_count_;
    int64_t& last_tick = last_tick_;
    int64_t anchor_refresh = 0;
    double anchor_time = 0; // the last present waited for: its refresh and when it was seen (0: none)
    double last_waited = 0;  // when the last present waited for was seen
    uint32_t since_waited = 0;
    bool behind = false;     // the last present waited for came a refresh late: so far unconfirmed
    bool unanchored = true;  // no count by time since the last title vblank
    std::vector<double> periods; // one refresh, between presents waited for one after the other
    uint32_t faster_windows = 0; // windows in a row measuring a faster display than reported
    uint32_t showing = UINT32_MAX; // the image presented last
    uint64_t showing_ready = 0;
    int64_t showing_from = 0;
    // Statistics, every ten seconds.
    uint32_t refreshes = 0, images = 0, late = 0, missed = 0, dropped = 0, stalls = 0;
    uint32_t lengths[4] = {};
    double window = now_seconds();
    std::vector<int32_t> needs;      // refreshes from a frame's vblank until its first image was complete
    std::deque<int64_t> late_frames; // when frames needed more than the lead
    static const bool trace = std::getenv("TTT2_TRACE_PACING") != nullptr;
    // Shows the newest complete image at once (presents failing or not
    // reaching the screen), so the queue keeps draining. Under present_mutex_.
    const auto drain = [&] {
        uint64_t done = 0;
        VK_CHECK(vkGetSemaphoreCounterValue(ctx_.device, timeline_, &done));
        bool first = true;
        while (!present_queue_.empty() && done >= present_queue_.front().ready) {
            if (showing != UINT32_MAX) {
                present_free_[showing] = 1;
                if (first) {
                    ++lengths[std::clamp<int64_t>(refresh + 1 - showing_from, 1, 4) - 1];
                    ++images;
                } else {
                    ++dropped;
                }
            }
            first = false;
            showing = present_queue_.front().image;
            showing_ready = present_queue_.front().ready;
            showing_from = refresh + 1;
            present_queue_.pop_front();
        }
        present_cv_.notify_all();
    };
    while (host::window_refresh_rate() == reported_hz) {
        {
            std::unique_lock lock(present_mutex_);
            // The GPU thread replaces the images: let go of all of them.
            const auto release = [&] {
                if (showing != UINT32_MAX) present_free_[showing] = 1;
                for (const QueuedFrame& f : present_queue_) present_free_[f.image] = 1;
                present_queue_.clear();
                showing = UINT32_MAX;
                present_cv_.notify_all();
            };
            if (present_release_) release();
            // The first image (it sets the swapchain's format and size).
            if (showing == UINT32_MAX) {
                for (;;) {
                    present_cv_.wait(lock, [&] { return !present_queue_.empty(); });
                    if (!present_release_) break;
                    release();
                }
                const QueuedFrame f = present_queue_.front();
                present_queue_.pop_front();
                showing = f.image;
                showing_ready = f.ready;
                showing_from = refresh + 1;
            }
        }
        if (generation != swapchain_generation_) { // presents to an old swapchain are gone
            generation = swapchain_generation_;
            in_flight.clear();
            anchor_time = 0;
            unanchored = true;
        }
        if (in_flight.size() >= queued) {
            VkPresentWait2InfoKHR wi{VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR};
            wi.presentId = in_flight.front();
            wi.timeout = 0;
            VkResult r = ctx_.wait_for_present(ctx_.device, swapchain_, &wi);
            const bool waited = r == VK_TIMEOUT; // else it was on screen already
            if (waited) {
                wi.timeout = stall_ns;
                r = ctx_.wait_for_present(ctx_.device, swapchain_, &wi);
            }
            if (r == VK_TIMEOUT) {
                // Not reaching the screen (a hidden window): the title's own
                // timer takes over its vblanks; show the newest image.
                // (The count by time goes on from the last present seen:
                // the blanks the timer gives meanwhile count against it.)
                ++stalls;
                std::lock_guard lock(present_mutex_);
                drain();
                continue;
            }
            in_flight.pop_front();
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
                in_flight.clear();
                anchor_time = 0;
                unanchored = true;
            } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                check_failed("vkWaitForPresent2KHR", r, __FILE__, __LINE__);
            } else {
                const double t = now_seconds();
                int64_t shown = refresh + 1; // FIFO: one refresh per present
                ++since_waited;
                bool reanchor = waited;
                if (anchor_time != 0) {
                    const double elapsed = (t - anchor_time) / period;
                    if (waited) {
                        // Refreshes the screen showed an image again (a present
                        // too late for its refresh) are in the time; a single
                        // late wake-up is too, so it counts once the next
                        // present shows it as well (measured from the same
                        // present: a repeat moves every later present).
                        const int64_t by_time = anchor_refresh + static_cast<int64_t>(std::floor(elapsed + 0.3));
                        if (by_time > shown && !behind) {
                            behind = true;
                            reanchor = false;
                        } else if (by_time > shown) {
                            missed += static_cast<uint32_t>(by_time - shown);
                            shown = by_time;
                            behind = false;
                        } else {
                            behind = false;
                            // One refresh apart, waited for one after the other
                            // with presents queued behind: the display's period.
                            if (since_waited == 1 && last_waited != 0 && t - last_waited < 1.5 * period) {
                                periods.push_back(t - last_waited);
                            }
                        }
                    } else {
                        // Already on screen: the thread fell behind and the
                        // screen may have shown an image again meanwhile; at
                        // least the refreshes since the anchor, less those the
                        // presents still queued may take.
                        const int64_t by_time = anchor_refresh + static_cast<int64_t>(std::floor(elapsed)) -
                                                static_cast<int64_t>(in_flight.size());
                        if (by_time > shown) {
                            missed += static_cast<uint32_t>(by_time - shown);
                            shown = by_time;
                        }
                    }
                }
                if (anchor_time == 0) behind = false; // nothing to confirm it against
                if (reanchor) {
                    // A part of a refresh left over (a display slower than
                    // reported by other than a whole factor) carries over;
                    // small ones (drift, jitter) do not.
                    const double carried = anchor_time + static_cast<double>(shown - anchor_refresh) * period;
                    anchor_time = anchor_time != 0 && t - carried > 0.15 * period ? carried : t;
                    anchor_refresh = shown;
                }
                if (waited) {
                    last_waited = t;
                    since_waited = 0;
                }
                refreshes += static_cast<uint32_t>(shown - refresh);
                refresh = shown;
            }
        }
        // The title's vblanks, at every per_frame-th refresh (each one
        // passed, up to a few after a stall).
        if (const int64_t tick = refresh / per_frame * per_frame; tick > last_tick) {
            // (Rounded up: an old tick from another rate need not be a multiple.)
            const int64_t passed =
                last_tick < 0 ? 1 : std::clamp<int64_t>((tick - last_tick + per_frame - 1) / per_frame, 1, 4);
            last_tick = tick;
            if (anchor_time != 0) tick_time_.store(now_seconds());
            tick_refresh_.store(tick);
            // Counted without time since the last vblank (after a stall, a
            // new swapchain, a mode change): blanks the title's timer gave
            // meanwhile are not in `passed`.
            if (unanchored) gpu::forget_timer_vsyncs();
            unanchored = anchor_time == 0;
            gpu::host_vsync(static_cast<uint32_t>(passed));
        }
        // The refresh the next present shows at, and its image.
        const int64_t at = refresh + static_cast<int64_t>(in_flight.size()) + 1;
        {
            uint64_t done = 0;
            VK_CHECK(vkGetSemaphoreCounterValue(ctx_.device, timeline_, &done));
            std::lock_guard lock(present_mutex_);
            int32_t chosen = -1;
            for (size_t i = 0; i < present_queue_.size(); ++i) {
                QueuedFrame& f = present_queue_[i];
                if (!f.seen_ready && done >= f.ready) {
                    f.seen_ready = true;
                    if (f.first) {
                        const int32_t need = static_cast<int32_t>(std::max<int64_t>(0, at - f.tick - f.offset));
                        // Stalls (loading) over two frames are not measures.
                        if (need <= 3 * per_frame + static_cast<int32_t>(queued)) {
                            needs.push_back(need);
                            // Late more than once in the last second: more lead
                            // now (a single late frame, such as one compiling a
                            // shader, is not worth the latency).
                            if (need > refresh_lead_.load()) {
                                late_frames.push_back(at);
                                while (!late_frames.empty() && late_frames.front() < at - 60 * per_frame) {
                                    late_frames.pop_front();
                                }
                                if (late_frames.size() >= 2) {
                                    refresh_lead_.store(need);
                                    late_frames.clear();
                                }
                            }
                        }
                    }
                }
                if (f.due <= at && !f.seen_ready && !f.counted_late) {
                    f.counted_late = true;
                    ++late;
                }
                if (f.due <= at && f.seen_ready) chosen = static_cast<int32_t>(i);
            }
            if (chosen >= 0) {
                for (int32_t i = 0; i < chosen; ++i) {
                    present_free_[present_queue_[i].image] = 1;
                    ++dropped;
                }
                if (showing != UINT32_MAX) {
                    present_free_[showing] = 1;
                    ++lengths[std::clamp<int64_t>(at - showing_from, 1, 4) - 1];
                    ++images;
                }
                showing = present_queue_[chosen].image;
                showing_ready = present_queue_[chosen].ready;
                showing_from = at;
                present_queue_.erase(present_queue_.begin(), present_queue_.begin() + chosen + 1);
                present_cv_.notify_all();
            }
        }
        if (present_image(showing, showing_ready, present_id_ + 1)) {
            in_flight.push_back(++present_id_);
        } else {
            // Not presented (a new swapchain, a minimised window): keep the
            // queue moving.
            in_flight.clear();
            anchor_time = 0;
            unanchored = true;
            {
                std::lock_guard lock(present_mutex_);
                drain();
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(period));
        }

        // The display's own rate, from presents waited for one after the
        // other: faster than reported (a plausible rate, two windows in a
        // row) chooses the mode again, else the title's vblanks would come
        // too fast. (A slower display, or a compositor skipping refreshes,
        // is counted by time.)
        if (periods.size() >= 240) {
            std::nth_element(periods.begin(), periods.begin() + periods.size() / 2, periods.end());
            const double measured = periods[periods.size() / 2];
            periods.clear();
            faster_windows = measured > 1.0 / 500 && measured < 0.98 * period ? faster_windows + 1 : 0;
            if (faster_windows >= 2) {
                measured_hz_ = 1.0 / measured;
                std::fprintf(stderr, "ttt2: GPU: the display refreshes at %.2f Hz, not %.2f\n", measured_hz_, 1.0 / period);
                break;
            }
        }

        const double now = now_seconds();
        if (now - window >= 10.0) {
            // The lead: what the last ten seconds' frames needed, all but
            // the slowest 1%.
            std::sort(needs.begin(), needs.end());
            if (needs.size() >= 100) refresh_lead_.store(std::max(needs[needs.size() * 99 / 100], 1));
            const uint32_t stolen = stolen_images_.exchange(0, std::memory_order_relaxed);
            const uint32_t shown = std::max(1u, lengths[0] + lengths[1] + lengths[2] + lengths[3]);
            std::fprintf(stderr,
                         "ttt2: present: %.1f refreshes/s, %.1f images/s shown for 1/2/3/4+ refreshes: "
                         "%.1f/%.1f/%.1f/%.1f%%; %u late, %u missed refreshes, %u dropped%s; lead %d refreshes (%.1f ms)\n",
                         refreshes / (now - window), images / (now - window), 100.0 * lengths[0] / shown,
                         100.0 * lengths[1] / shown, 100.0 * lengths[2] / shown, 100.0 * lengths[3] / shown, late, missed,
                         dropped + stolen, stalls ? ", stalled" : "", refresh_lead_.load(),
                         refresh_lead_.load() * period * 1000.0);
            if (trace && !needs.empty()) {
                const auto at_pct = [&](size_t p) { return needs[std::min(needs.size() - 1, needs.size() * p / 100)]; };
                std::fprintf(stderr, "pacing: need 50%% %d, 90%% %d, 99%% %d, max %d refreshes\n", at_pct(50), at_pct(90),
                             at_pct(99), needs.back());
            }
            needs.clear();
            refreshes = images = late = missed = dropped = stalls = 0;
            std::fill(std::begin(lengths), std::end(lengths), 0u);
            window = now;
        }
    }
    // Leaving the mode: the image showing goes back; queued ones stay.
    std::lock_guard lock(present_mutex_);
    if (showing != UINT32_MAX) present_free_[showing] = 1;
    present_cv_.notify_all();
}

void Renderer::create_swapchain(VkFormat image_format) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical, surface_, &caps));
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, surface_, &count, formats.data());
    // The TV image holds display-ready values: present them unconverted
    // (a UNORM swapchain written by the scaling shader, or for the blit one
    // of the image's own kind).
    const bool srgb = !compute_present_ && (image_format == VK_FORMAT_R8G8B8A8_SRGB || image_format == VK_FORMAT_B8G8R8A8_SRGB);
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        const bool f_srgb = f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB;
        const bool f_unorm = f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM;
        const bool storage = !compute_present_ || (ctx_.format_features(f.format) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        if (((srgb && f_srgb) || (!srgb && f_unorm)) && storage) {
            chosen = f;
            break;
        }
    }
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physical, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physical, surface_, &mode_count, modes.data());
    // Presenting every refresh: FIFO, one image per refresh. Else frames are
    // presented at chosen times: show each at the next refresh.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_every_refresh_ = every_refresh_.load();
    if (!swapchain_every_refresh_) {
        // (Uncapped: immediate first, every frame reaching the screen, torn, rather than the latest per refresh.)
        const VkPresentModeKHR order[2] = {uncapped_ ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR,
                                           uncapped_ ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR};
        for (VkPresentModeKHR wanted : order) {
            if (mode == VK_PRESENT_MODE_FIFO_KHR && std::find(modes.begin(), modes.end(), wanted) != modes.end()) mode = wanted;
        }
    }
    uint32_t width, height;
    host::drawable_size(width, height);
    if (caps.currentExtent.width != UINT32_MAX) {
        width = caps.currentExtent.width;
        height = caps.currentExtent.height;
    }
    width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
    height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    if (swapchain_every_refresh_) {
        sci.flags = VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    }
    sci.surface = surface_;
    // Every refresh: two queued, one on screen, one being written.
    sci.minImageCount = std::max(caps.minImageCount, swapchain_every_refresh_ ? 5u : 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = {width, height};
    sci.imageArrayLayers = 1;
    sci.imageUsage = compute_present_ ? VK_IMAGE_USAGE_STORAGE_BIT
                                      : VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR swapchain;
    VK_CHECK(vkCreateSwapchainKHR(ctx_.device, &sci, nullptr, &swapchain));
    if (swapchain_) {
        if (compute_present_) {
            vkQueueWaitIdle(present_queue_handle_);
        } else {
            std::lock_guard lock(queue_mutex_);
            vkQueueWaitIdle(present_queue_handle_);
        }
        for (VkImageView v : swapchain_views_) vkDestroyImageView(ctx_.device, v, nullptr);
        swapchain_views_.clear();
        vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
    }
    swapchain_ = swapchain;
    ++swapchain_generation_;
    swapchain_format_ = chosen.format;
    swapchain_extent_ = sci.imageExtent;
    vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &count, swapchain_images_.data());
    if (compute_present_) {
        for (VkImage image : swapchain_images_) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = chosen.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            VK_CHECK(vkCreateImageView(ctx_.device, &vci, nullptr, &view));
            swapchain_views_.push_back(view);
        }
    }
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (acquired_ == VK_NULL_HANDLE) VK_CHECK(vkCreateSemaphore(ctx_.device, &semaphore, nullptr, &acquired_));
    while (rendered_.size() < count) {
        VkSemaphore s;
        VK_CHECK(vkCreateSemaphore(ctx_.device, &semaphore, nullptr, &s));
        rendered_.push_back(s);
    }
}

bool Renderer::present_image(uint32_t i, uint64_t ready, uint64_t present_id) {
    const Image& src = present_images_[i];
    uint32_t width, height;
    host::drawable_size(width, height);
    if (width == 0 || height == 0) return false; // minimised
    const bool srgb_image = src.format == VK_FORMAT_R8G8B8A8_SRGB || src.format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool srgb_chain = swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_R8G8B8A8_SRGB;
    if (swapchain_ == VK_NULL_HANDLE || width != swapchain_extent_.width || height != swapchain_extent_.height ||
        (!compute_present_ && srgb_image != srgb_chain) || swapchain_every_refresh_ != every_refresh_.load()) {
        create_swapchain(src.format);
    }
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(ctx_.device, swapchain_, 100'000'000, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        create_swapchain(src.format);
        return false;
    }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) return false;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) check_failed("vkAcquireNextImageKHR", r, __FILE__, __LINE__);
    VkImage image = swapchain_images_[index];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(present_cmd_, &bi));
    const auto transition = [&](VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        ib.oldLayout = from;
        ib.newLayout = to;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(present_cmd_, &di);
    };
    // Letterboxed to the TV image's aspect ratio. A window within 2 pixels of
    // the image shows it 1:1, centred, those pixels cut off or left black: at
    // a fractional desktop scale a window of the chosen render size can come
    // out a pixel smaller, and scaling by 0.999 would blur the whole picture.
    const int32_t extent_w = static_cast<int32_t>(swapchain_extent_.width);
    const int32_t extent_h = static_cast<int32_t>(swapchain_extent_.height);
    const bool one_to_one = std::abs(extent_w - static_cast<int32_t>(src.width)) <= 2 &&
                            std::abs(extent_h - static_cast<int32_t>(src.height)) <= 2;
    const double scale = one_to_one ? 1.0 : std::min(double(extent_w) / src.width, double(extent_h) / src.height);
    const int32_t w = static_cast<int32_t>(src.width * scale), h = static_cast<int32_t>(src.height * scale);
    const int32_t x = (extent_w - w) / 2, y = (extent_h - h) / 2; // -1 to 1 when 1:1
    if (compute_present_) {
        transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(present_cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, scale_pipeline_);
        VkDescriptorImageInfo source{scale_sampler_, src.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo target{VK_NULL_HANDLE, swapchain_views_[index], VK_IMAGE_LAYOUT_GENERAL};
        update_overlay();
        VkDescriptorBufferInfo overlay{overlay_buffer_.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = writes[1].sType = writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &source;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &target;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &overlay;
        ctx_.cmd_push_descriptor_set(present_cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, scale_layout_, 0, 3, writes);
        // Texel centres: 1:1 is an exact copy, whatever the offset.
        const int32_t rect[6] = {x, y, w, h, extent_w, extent_h};
        vkCmdPushConstants(present_cmd_, scale_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rect, rect);
        vkCmdPushConstants(present_cmd_, scale_layout_, VK_SHADER_STAGE_COMPUTE_BIT, sizeof rect, sizeof overlay_push_,
                           &overlay_push_);
        vkCmdDispatch(present_cmd_, (swapchain_extent_.width + 15) / 16, (swapchain_extent_.height + 15) / 16, 1);
        transition(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        // shortcut: no overlay (gpu::set_overlay) on this path; draw it here if a GPU without compute present needs it.
        transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(present_cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(src.width), static_cast<int32_t>(src.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        if (one_to_one) {
            // A blit's offsets must lie inside both images: the part of the
            // image the window shows, to the same size (nothing filtered).
            const int32_t copy_w = std::min(w, extent_w), copy_h = std::min(h, extent_h);
            blit.srcOffsets[0] = {std::max(0, -x), std::max(0, -y), 0};
            blit.srcOffsets[1] = {blit.srcOffsets[0].x + copy_w, blit.srcOffsets[0].y + copy_h, 1};
            blit.dstOffsets[0] = {std::max(0, x), std::max(0, y), 0};
            blit.dstOffsets[1] = {blit.dstOffsets[0].x + copy_w, blit.dstOffsets[0].y + copy_h, 1};
        }
        vkCmdBlitImage(present_cmd_, src.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, one_to_one ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    VK_CHECK(vkEndCommandBuffer(present_cmd_));

    // After the swapchain image is free and the frame's copy has finished.
    const VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    const VkSemaphore waits[2] = {acquired_, timeline_};
    const uint64_t wait_values[2] = {0, ready};
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = 2;
    ti.pWaitSemaphoreValues = wait_values;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = wait_stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &present_cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkPresentId2KHR id{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};
    if (present_id != 0) {
        id.swapchainCount = 1;
        id.pPresentIds = &present_id;
        pi.pNext = &id;
    }
    if (compute_present_) {
        VK_CHECK(vkQueueSubmit(present_queue_handle_, 1, &si, present_fence_));
        r = vkQueuePresentKHR(present_queue_handle_, &pi);
    } else {
        std::lock_guard lock(queue_mutex_);
        VK_CHECK(vkQueueSubmit(present_queue_handle_, 1, &si, present_fence_));
        r = vkQueuePresentKHR(present_queue_handle_, &pi);
    }
    VK_CHECK(vkWaitForFences(ctx_.device, 1, &present_fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(ctx_.device, 1, &present_fence_));
    VK_CHECK(vkResetCommandPool(ctx_.device, present_pool_, 0));
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        create_swapchain(src.format);
        return r == VK_SUBOPTIMAL_KHR; // (presented; an id on the old swapchain is gone with it)
    }
    if (r != VK_SUCCESS) check_failed("vkQueuePresentKHR", r, __FILE__, __LINE__);
    return true;
}

} // namespace cafe::gpu::vk

namespace cafe::gpu {

void set_overlay(float dim, std::vector<OverlayLayer> layers) {
    std::lock_guard lock(vk::g_overlay_mutex);
    vk::g_overlay_dim = dim;
    vk::g_overlay_layers = std::move(layers);
    ++vk::g_overlay_version;
}

LiveStats live_stats() {
    return {vk::g_shown_fps.load(), vk::g_game_fps.load(), vk::g_worst_ms.load(), vk::g_gpu_ms.load(), vk::g_draws.load()};
}

} // namespace cafe::gpu
