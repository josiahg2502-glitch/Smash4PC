#pragma once

// The Vulkan backend: renders the command processor's draws, clears and
// copies, and presents the TV scan buffer in the window.
//
// Synchronous with the command processor: work is recorded into one
// command buffer and submitted, and waited for, before the command
// processor retires a submission or completes a frame. Everything the
// guest can observe therefore happens after the GPU work it depends on,
// and guest memory the GPU reads in place (vertex and uniform buffers)
// cannot change underneath it.
//
// Surfaces: render targets and depth buffers live on the GPU, keyed by
// address, format and size; textures are images created from guest memory
// (detiled) or copied from a render target at the same address when the
// GPU wrote it last.
//
// Upscaling (display.ini's resolution or TTT2_SCALE, else enough to cover
// the display): targets the size of the screen and its halvings get images
// `scale_` times larger, 1, 1.5, 2, 3 or 4; everything else (shadow maps,
// the GPU's texture-compression targets, the GamePad screen) keeps the
// title's size. Passes into upscaled targets scale the viewport and
// scissor, shaders see the title's pixel coordinates, and textures copied
// from upscaled targets stay upscaled (shaders scale their texel
// coordinates and sizes). At 1.5 a title pixel covers one or two image
// pixels: sizes round up (scaled()), and edges take the image pixels whose
// centres fall inside them.

#include "gpu/gpu.h"
#include "gpu/backend.h"
#include "gpu/vulkan/context.h"
#include "gpu/vulkan/formats.h"
#include "latte/program.h"
#include "latte/shader_abi.h"
#include "latte/translate.h"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <functional>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct shaderc_compiler;

namespace cafe::gpu::vk {

// A size in the title's pixels in image pixels at `scale`, rounded up (at
// 1.5 an odd size gives half a pixel: a texture copied from a target of a
// dynamic width can have one), and an image's size in the title's pixels,
// rounded down. Both are exact at the whole scales.
inline uint32_t scaled(uint32_t n, float scale) {
    return static_cast<uint32_t>(std::ceil(static_cast<double>(n) * scale));
}
inline uint32_t unscaled(uint32_t pixels, float scale) {
    return static_cast<uint32_t>(std::floor(static_cast<double>(pixels) / scale));
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; // all levels and layers, identity swizzle
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, layers = 1, levels = 1;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
};

// A color or depth buffer the GPU renders into.
struct Target {
    uint32_t address = 0;   // of the level/slice rendered to, swizzle removed
    uint32_t format = 0;    // GX2 surface format
    uint32_t pitch = 0;     // elements
    uint32_t height = 0;    // aligned rows
    uint32_t tile_mode = 0;
    uint32_t base_reg = 0;  // its address register (address >> 8, the bank/pipe swizzle included)
    bool depth = false;
    float scale = 1;        // image size over the title's size
    bool write_back = false; // copied to guest memory after the GPU writes it (see Renderer::retired)
    bool write_back_queued = false;
    Image image;
    uint64_t written = 0;   // event stamp of the last GPU write
    uint64_t overwritten = 0; // event stamp of the last CPU write to its memory
    uint32_t end = 0;       // first byte past its memory
    // Interpolation: targets read in a frame before it writes them carry
    // content between frames; replays start from their content at the
    // start of the frame, and the next frame gets the real frame's.
    uint64_t written_frame = 0; // frame_number_ of the last write by a real frame
    bool carried = false;       // read before written in this frame
    bool start_copied = false;  // start_copy holds this frame's start
    bool rewrites = false;      // was rewritten after being carried before: copy its start
    Image start_copy, end_copy;
};

struct Texture {
    std::array<uint32_t, 7> words{}; // the resource, key
    Image image;
    VkImageView view = VK_NULL_HANDLE; // with the resource's component selects
    uint32_t memory[2][2] = {}; // guest ranges [begin, end): base levels, mip levels
    uint64_t loaded = 0;     // event stamp the contents correspond to
    uint64_t hash = 0;       // of the guest data last loaded
    bool dirty = true;       // the CPU wrote its memory since it was loaded
    const Target* source = nullptr; // render target the contents came from
    float scale = 1;        // image size over the resource's size (copied from upscaled targets)
    // A replacement (gpu/replacements.h) matched the contents: its index,
    // the generation of replacement contents loaded, whether the image is
    // RGBA8 for them.
    int replacement = -1;
    uint64_t replacement_generation = 0;
    bool rgba = false;
};

struct ShaderModule {
    VkShaderModule module = VK_NULL_HANDLE;
    bool stage_vertex = false;
    uint64_t glsl_hash = 0; // the translation it was compiled from (modules are shared by it)
    latte::TranslatedShader info;
    bool failed = false;
};

// Everything a pipeline bakes in; the rest is dynamic state. The shaders are
// named by the hash of their GLSL, so keys are the same from run to run and
// can be saved for precompiling.
struct PipelineKey {
    uint64_t vs_glsl;
    uint64_t ps_glsl;
    VkFormat colors[8];
    VkFormat depth;
    uint32_t color_count;
    uint32_t blend[8]; // CB_BLENDn_CONTROL, with bit 31 set when blending is enabled
    uint32_t write_mask;
    uint32_t polygon_mode;
    uint32_t topology_class;
    uint32_t depth_clamp;
};

// A shader module or pipeline compiled on a worker thread.
enum class Built : int { kPending, kReady, kFailed };

struct PendingModule {
    std::atomic<Built> state{Built::kPending};
    VkShaderModule module = VK_NULL_HANDLE;
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const ShaderModule* vs = nullptr;
    const ShaderModule* ps = nullptr;
    std::atomic<Built> state{Built::kReady};
    bool listed = false; // in pipelines.list
    PipelineKey key{};
};

class Renderer final : public Backend {
public:
    Renderer();
    const char* name() const override { return "Vulkan"; }
    void draw(const Registers& regs, const Draw& draw) override;
    void clear_color(const Registers& regs, const gx2::ColorBuffer& buffer, const float rgba[4]) override;
    void clear_depth_stencil(const Registers& regs, const gx2::DepthBuffer& buffer, uint32_t flags, float depth,
                             uint32_t stencil) override;
    void copy_surface(const gx2::Surface& src, uint32_t src_level, uint32_t src_slice, const gx2::Surface& dst,
                      uint32_t dst_level, uint32_t dst_slice) override;
    void resolve_color(const gx2::ColorBuffer& src, const gx2::Surface& dst, uint32_t dst_level,
                       uint32_t dst_slice) override;
    void expand_depth(const gx2::DepthBuffer&) override {}
    void convert_depth(const gx2::DepthBuffer& src, const gx2::Surface& dst, uint32_t dst_level,
                       uint32_t dst_slice) override;
    void copy_to_scan_buffer(const gx2::ColorBuffer& buffer, uint32_t scan_target) override;
    void swap() override;
    void invalidate(uint32_t address, uint32_t size, uint32_t coherency_flags) override;
    void cpu_wrote(uint32_t address, uint32_t size) override;
    void sync() override;
    uint64_t flush() override;
    void wait_for(uint64_t value) override;
    void idle() override;
    uint32_t frames_per_frame() const override { return frames_per_frame_; }
    bool want_replays() override;
    bool want_another_replay() override;
    void begin_replay(uint32_t n) override;
    void end_replay() override;
    void frame_shown() override;
    void retired(uint64_t value) override;

private:
    // ----------------------------------------------------- commands (renderer.cpp)
    VkCommandBuffer cmd();
    // Ends the command buffer being recorded and submits it; returns the
    // timeline value that signals its completion. With `wait`, waits for
    // every submission so far.
    uint64_t submit(bool wait);
    // why: 0 a command slot, 1 an upload chunk, 2 an explicit sync, 3 other (for the statistics).
    void wait_value(uint64_t value, int why = 3);
    uint64_t waits_by_[4] = {}, submits_counted_ = 0;
    bool flush_pending_ = false; // flush() held back a submission (see Renderer::flush)
    double last_submit_ = 0;
    void barrier();
    uint8_t* upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset);
    // `shared`: used by the presentation queue too (concurrent sharing).
    Image create_image(VkFormat format, VkImageAspectFlags aspect, uint32_t width, uint32_t height, uint32_t layers,
                       uint32_t levels, VkImageUsageFlags usage, VkImageViewType view_type, bool shared = false);
    void destroy_image(Image& image);
    void end_rendering();
    uint64_t stamp() { return ++events_; }

    // ------------------------------------------------- targets (targets.cpp)
    Target* color_target(uint32_t base_reg, uint32_t size_reg, uint32_t info_reg);
    Target* color_target(const gx2::ColorBuffer& buffer);
    Target* depth_target(uint32_t base_reg, uint32_t size_reg, uint32_t info_reg);
    Target* depth_target(const gx2::DepthBuffer& buffer);
    Target* find_target(uint32_t address, uint32_t format, bool depth);
    // Copies the top-left width x height (in the title's pixels) of a target
    // into `dst` (an image `dst_scale` times the title's size); filtered
    // when the scales differ.
    void copy_target_region(const Target& src, const Image& dst, float dst_scale, uint32_t dst_layer,
                            uint32_t width, uint32_t height);
    bool upscaled_size(uint32_t width, uint32_t height) const;
    // The scale of upscaled targets: scale_ is relative to a 720-row image,
    // the title's TV image may be 1080 rows.
    float target_scale() const { return scale_ * 720.0f / static_cast<float>(tv_height()); }
    // Write-back: on the console the GPU renders into guest memory, and the
    // title reads some targets with the CPU (the exposure of some stages:
    // the scene's average colour, rendered to one element of a 64x64
    // target and read two frames later). Small targets at the title's size
    // are copied to staging memory at the end of each submission that
    // wrote them and tiled into guest memory once it finished (retired).
    static constexpr uint64_t kWriteBackBytes = 64u << 10;
    void queue_write_back(Target* t);
    void copy_write_backs();
    struct WriteBack {
        uint64_t value;     // submission that copies it to staging
        uint32_t staging;   // index in write_back_staging_
        uint32_t address, base_reg, tile_mode, bpp, pitch, height;
    };
    std::vector<Target*> write_back_targets_; // written since the last submission
    std::mutex write_back_mutex_;             // the members below: the retirement thread uses them too
    std::deque<WriteBack> write_backs_;
    std::vector<Buffer> write_back_staging_;
    std::vector<uint32_t> write_back_free_;

    // ----------------------------------------------- textures (textures.cpp)
    Texture* texture(const uint32_t words[7]);
    void create_texture_image(Texture& t, float scale);
    void load_texture(Texture& t);
    void mark_textures_dirty(uint32_t address, uint64_t end);
    bool load_texture_from_targets(Texture& t);
    void copy_depth_to_texture(const Target& src, Texture& t, uint32_t width, uint32_t height);
    VkSampler sampler(const uint32_t words[3], const float border[4], bool compare);
    Buffer depth_staging_; // video memory for depth -> texture copies
    // A 1x1 2D image of zeros, for texture slots with nothing bound.
    const Image& null_texture();
    Image null_texture_;

    // ---------------------------------------------------- draws (draw.cpp)
    const ShaderModule* shader(latte::Stage stage, uint32_t address, uint32_t size, uint32_t fetch_address,
                               uint32_t fetch_size, const latte::ShaderEnvironment& env);
    const latte::Program* program(uint32_t address, uint32_t size, uint64_t& hash);
    VkPipelineLayout pipeline_layout(uint32_t vs_textures, uint32_t ps_textures, bool vs_registers,
                                     bool ps_registers);
    const Pipeline* pipeline(const Registers& regs, const ShaderModule* vs, const ShaderModule* ps,
                             const VkFormat* colors, uint32_t color_count, VkFormat depth, uint32_t topology_class);
    latte::abi::Buffer buffer_descriptor(uint32_t address, uint32_t size, uint32_t stride) const;

    // ---------------------------------------- interpolation (interpolate.cpp)
public: // record types, shared with interpolate.cpp's helpers
    struct ConstantSource {
        uint8_t stage; // 0 vertex, 1 pixel
        uint8_t kind;  // 0 uniform block, 1 buffer resource
        uint8_t index;
        uint32_t address, size, stride;
    };
    struct ConstantSources {
        ConstantSource slots[64];
        uint32_t count = 0;
        void add(const ConstantSource& s) {
            if (count < 64 && s.size != 0) slots[count++] = s;
        }
        const ConstantSource* begin() const { return slots; }
        const ConstantSource* end() const { return slots + count; }
    };
    struct SlotRecord {
        uint8_t stage, kind, index;
        uint32_t offset, size, stride;
        bool big_endian;
        uint64_t hash; // of the contents
    };
    struct DrawRecord {
        uint64_t key;
        uint32_t first_slot, slot_count;
        uint64_t hash; // of all its constants
        bool blend;    // a depth-tested (3D) draw: its constants blend in replays
    };
    // A frame's draws and the constant data they read, copied into
    // GPU-visible memory: the frame's own draws read it there too, so a
    // replay sees exactly what the frame saw.
    struct FrameRecord {
        std::vector<DrawRecord> draws;
        std::vector<SlotRecord> slots;
        Buffer data;
        size_t used = 0;
        struct Snapshot {
            uint32_t offset;
            bool big_endian;
            uint64_t hash;
        };
        std::map<uint64_t, Snapshot> snapshots; // (address << 32 | size) -> copy in data
        bool overflow = false;
        void clear();
    };
    struct Difference {
        uint32_t changed = 0, large = 0, compared = 0, odd = 0;
        double distance = 0;
        // Nearer: fewer words that are not floats changed, then the floats
        // moved less.
        bool operator<(const Difference& o) const { return odd != o.odd ? odd < o.odd : distance < o.distance; }
    };

private:
    Difference difference(const FrameRecord& a_frame, const DrawRecord& a, const FrameRecord& b_frame,
                          const DrawRecord& b);
    std::unordered_map<uint64_t, Difference> block_differences_; // (previous, current offset) -> difference
    void init_interpolation();
    uint32_t begin_draw_record(const Registers& regs, const Draw& d);
    static constexpr uint32_t kMaxSnapshot = 256u << 10;
    bool snapshot(uint32_t address, uint32_t size, uint32_t& offset, bool& swapped, uint64_t& hash);
    void record_constants(uint32_t draw_index, const ConstantSources& sources, latte::abi::DrawConstants& dc);
    void replace_constants(uint32_t draw_index, latte::abi::DrawConstants& dc);
    uint32_t frames_per_frame_ = 1; // frames shown per frame the title renders
    bool uncapped_ = false;         // as many of those as the GPU keeps up with, presented without pacing or vsync
    bool replaying_ = false;
    bool replayed_ = false; // this frame's extra frames were rendered
    float replay_t_ = 1.0f;          // blend position: 0 the previous frame, 1 this one
    bool replay_blend_ = false;
    uint32_t replay_draw_ = 0;
    FrameRecord frame_records_[2];
    uint64_t record_value_[2] = {}; // last submission reading each record
    int current_record_ = 0;
    std::vector<int32_t> match_;     // this frame's draw -> previous frame's, -1 none
    struct Blend {
        VkDeviceAddress address; // in the ring
        uint32_t chunk;          // its ring chunk, marked used by each command buffer reading it
    };
    std::unordered_map<uint64_t, Blend> blended_; // (previous, current offset) -> blend
    std::unordered_map<uint32_t, uint32_t> shared_blocks_; // current offset -> previous, of the draws that pair
    void forget_snapshots(uint32_t address, uint32_t size);
    void note_target_written(const Target* t);
    void note_target_read(const Target* t);
    void copy_image(const Image& src, const Image& dst);
    std::vector<Target*> carried_; // this frame's carried targets
    uint64_t replays_ = 0, blended_replays_ = 0, skipped_replays_ = 0;
    double match_seconds_ = 0, replay_seconds_ = 0;
    std::chrono::steady_clock::time_point replay_started_;

    // ----------------------------------------------- presentation (present.cpp)
    void start_presenter();
    uint32_t take_present_image();
    uint32_t copy_scan_to_present_image(uint64_t& ready);
    void queue_present(uint32_t image, double time, uint64_t ready);
    void show_frame();
    void show_extra_frame();
    void finish_frame_presentation();
    void presenter_main();
    void choose_presentation(double hz);
    void present_timed(float reported_hz);
    void present_every_refresh(float reported_hz);
    void note_presentation(double scheduled, double presented);
    void create_swapchain(VkFormat image_format);
    // Presents image `index` once its copy (timeline value `ready`) is done;
    // `present_id` > 0 tags the present for present waits. False if the
    // swapchain had to be recreated (nothing was presented).
    bool present_image(uint32_t index, uint64_t ready, uint64_t present_id = 0);
    std::mutex queue_mutex_; // vkQueueSubmit and vkQueuePresentKHR from either thread
    std::mutex present_mutex_;
    std::condition_variable present_cv_;
    struct QueuedFrame {
        uint32_t image;
        double time;    // steady clock seconds
        uint64_t ready; // timeline value at which the image is complete
        double reference; // when its slot was chosen (the frame's swap)
        // Presenting every refresh: the refresh it is due at, the title
        // vblank its frame started from, whether it is its frame's first
        // image (and how many refreshes after the frame's first slot it is
        // due: a frame without extra frames), and whether it was found
        // complete.
        int64_t due = 0, tick = 0;
        int32_t offset = 0;
        bool first = false, seen_ready = false, counted_late = false;
    };
    void queue_present(const QueuedFrame& f);
    std::deque<QueuedFrame> present_queue_;
    std::vector<Image> present_images_;
    std::vector<uint8_t> present_free_;
    uint32_t pending_real_ = UINT32_MAX; // interpolation: this frame's image, shown after its extra frames
    uint64_t pending_ready_ = 0;
    uint64_t last_image_ready_ = 0;     // timeline value of the newest presentation image
    uint64_t previous_frame_ready_ = 0; // ... of the previous title frame's last image
    uint32_t extra_frames_ = 0;
    double frame_base_time_ = 0;
    int64_t frame_base_due_ = 0, frame_base_tick_ = 0; // presenting every refresh
    void queue_frame_image(uint32_t image, uint64_t ready, uint32_t n, bool first);
    double frame_slot();
    double last_slot_ = 0;                    // display time of the last frame's slot (GPU thread)
    double slot_reference_ = 0;               // when the last slot was chosen
    std::atomic<double> present_lead_{0.004}; // swap to slot, adapted by the presentation thread
    // Presenting every refresh (FIFO, present waits) of a display that
    // refreshes refreshes_per_frame_ times per title frame: the
    // presentation thread drives the title's vblanks, frames are due at
    // refresh counts.
    std::atomic<bool> every_refresh_{false};
    bool present_waits_ = false;           // the surface tells when presents reach the screen
    bool swapchain_every_refresh_ = false; // the mode the swapchain was made for (presentation thread)
    uint64_t swapchain_generation_ = 0;    // counts swapchain creations (presentation thread)
    uint64_t present_id_ = 0;              // the last present id used (presentation thread)
    // Presenting every refresh (presentation thread): the refresh the last
    // present seen on screen showed at, and the last title vblank's; kept
    // across mode changes so frames' due refreshes stay comparable.
    int64_t refresh_count_ = 0, last_tick_ = -1;
    double measured_hz_ = 0;               // presentation thread: the window's display measured unlike reported
    // Under present_mutex_: the GPU thread is replacing the presentation
    // images and waits for the presentation thread to let go of them.
    bool present_release_ = false;
    std::atomic<uint32_t> stolen_images_{0}; // taken back from the queue unshown (statistics)
    std::atomic<uint32_t> refreshes_per_frame_{1};
    double refresh_period_ = 1.0 / 60.0;          // seconds
    std::atomic<int64_t> tick_refresh_{0};        // refresh of the last title vblank
    std::atomic<double> tick_time_{0};            // and when it was seen
    std::vector<float> swap_delays_;              // GPU thread: swap after its vblank, ms (statistics)
    std::atomic<int32_t> refresh_lead_{0};        // title vblank to the frame's first image, in refreshes
    int64_t frame_due();
    bool compute_present_ = false;        // presenting from a compute queue with a scaling shader
    uint32_t present_family_ = 0;
    VkQueue present_queue_handle_ = VK_NULL_HANDLE;
    VkPipelineLayout scale_layout_ = VK_NULL_HANDLE;
    VkPipeline scale_pipeline_ = VK_NULL_HANDLE;
    VkSampler scale_sampler_ = VK_NULL_HANDLE;
    // gpu::set_overlay's layers, as the scaling shader reads them (presentation thread).
    Buffer overlay_buffer_;
    uint64_t overlay_version_ = 0;
    struct OverlayPush {
        float dim = 0, unused = 0;
        int32_t layers[2][4]{}; // x, y, width, height
        uint32_t base[2]{};     // the first pixel of each in the buffer
        float opacity[2]{};
    } overlay_push_;
    void update_overlay();
    void note_live_frame(); // swap: gpu::live_stats' title frames
    std::vector<VkImageView> swapchain_views_;
    VkCommandPool present_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer present_cmd_ = VK_NULL_HANDLE;
    VkFence present_fence_ = VK_NULL_HANDLE;

    // ------------------------------------------------------ misc (renderer.cpp)
    void load_pipeline_cache();
    void save_pipeline_cache();
    // Shaders and pipelines of earlier runs, compiled before the title starts.
    void precompile();
    VkShaderModule cached_module(uint64_t glsl_hash, const std::vector<uint32_t>* spirv);
    VkPipeline build_pipeline(const PipelineKey& k, VkShaderModule vs, VkShaderModule ps, VkPipelineLayout layout);
    void capture();
    void save_image(const Image& image, const std::string& path, bool keep_alpha = false);
    int64_t captured_frame_ = -1; // TTT2_CAPTURE: frame number and index of the last capture
    uint32_t captured_index_ = 0;
    uint32_t sequence_left_ = 0, sequence_frame_ = 0; // TTT2_CAPTURE_SEQUENCE: frames still to save
    uint32_t sequence_index_ = 0;                     // and the capture it follows
    bool sequence_saving_ = false;
    void save_sequence_image(uint32_t n);

    float scale_ = 1; // upscaling factor of screen-sized targets: 1, 1.5, 2, 3 or 4
    Context ctx_;
    GuestMemory guest_;
    UploadRing ring_;
    // Command buffers in turn, each reused once the GPU finished it; a
    // timeline semaphore counts finished submissions.
    // 64: Smash's fights (with big mods) submit 120 times a frame; with 8 the
    // CPU waited for the GPU some 60 times a frame.
    static constexpr uint32_t kSlots = 64;
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t value = 0; // timeline value of its last submission
    };
    VkCommandPool pool_ = VK_NULL_HANDLE;
    Slot slots_[kSlots];
    uint32_t slot_ = 0;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE; // being recorded
    bool recording_ = false;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t submitted_ = 0;               // value of the last submission
    uint64_t chunk_value_[UploadRing::kChunks] = {}; // last submission using each upload chunk
    uint32_t chunks_touched_ = 0;          // by the command buffer being recorded
    uint64_t events_ = 1;

    // Current dynamic rendering pass.
    bool rendering_ = false;
    // A draw wrote stream-out data (guest memory, from the vertex shader)
    // since the last barrier: draws reading it need one, outside a pass.
    bool stream_out_pending_ = false;
    std::array<const Target*, 8> pass_colors_{};
    const Target* pass_depth_ = nullptr;
    uint32_t pass_width_ = 0, pass_height_ = 0;
    const Pipeline* bound_pipeline_ = nullptr;

    std::vector<std::unique_ptr<Target>> targets_;
    std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures_;
    // Textures and shader programs by the 64 KiB pages of guest memory they
    // use, for the CPU-write and cache-invalidation notices (hundreds a frame).
    std::unordered_map<uint32_t, std::vector<Texture*>> texture_pages_;
    std::unordered_map<uint32_t, std::vector<uint64_t>> program_pages_;
    template <typename F>
    void for_pages(uint32_t address, uint64_t end, F f) {
        for (uint64_t page = address >> 16; page <= (end - 1) >> 16; ++page) f(static_cast<uint32_t>(page));
    }
    std::unordered_map<uint64_t, VkSampler> samplers_;

    shaderc_compiler* compiler_ = nullptr;
    struct ProgramEntry {
        uint64_t hash;
        latte::Program program;
        bool ok;
    };
    std::unordered_map<uint64_t, ProgramEntry> programs_;  // by address/size
    std::unordered_map<uint64_t, std::unique_ptr<ShaderModule>> shaders_;
    std::unordered_map<uint64_t, VkShaderModule> modules_; // by GLSL hash
    // New shaders and pipelines compile on worker threads; draws that need
    // one still compiling are skipped meanwhile (TTT2_SYNC_SHADERS=1: wait).
    bool async_compile_ = true;
    std::unordered_map<uint64_t, std::shared_ptr<PendingModule>> pending_modules_; // by GLSL hash
    void compile_async(std::function<void()> job);
    void resolve_module(ShaderModule& entry);
    std::mutex jobs_mutex_;
    std::condition_variable jobs_cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    uint64_t deferred_draws_ = 0;
    std::unordered_map<uint64_t, VkPipelineLayout> layouts_;
    std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines_;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    std::atomic<uint32_t> new_pipelines_{0};

    // Scan-out: the last frame copied to each scan buffer (TV, DRC).
    Image scan_[2];
    bool scan_valid_[2] = {};

    // Window presentation.
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered_;

    // TTT2_TRACE_FRAME=n: log every command of frame n (counted from 0).
    bool tracing() const { return frame_number_ == trace_frame_; }
    uint64_t frame_number_ = 0;
    uint64_t trace_frame_ = UINT64_MAX;

    // TTT2_PROFILE_AT=seconds: GPU time of every operation of one frame.
    void profile_mark(const char* what, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
    void profile_report();
    VkQueryPool profile_pool_ = VK_NULL_HANDLE;
    bool profiling_ = false;
    uint64_t profile_frame_ = UINT64_MAX;
    struct ProfileMark {
        const char* what;
        uint32_t a, b, c;
    };
    std::vector<ProfileMark> profile_marks_;

    // Statistics, printed every ten seconds.
    uint64_t frames_ = 0, draws_ = 0, skipped_draws_ = 0, waits_ = 0;
    // Where frame time went on the draw thread in the stats window: shader
    // translation, SPIR-V compiles, pipeline builds, texture loads (seconds, count).
    struct Spent {
        double seconds = 0, longest = 0;
        uint32_t count = 0;
    };
    Spent spent_translate_, spent_compile_, spent_pipeline_, spent_texture_, spent_upload_;
    // Adds the time since `since` to `s` when it goes out of scope.
    struct Timed {
        Spent& s;
        std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        ~Timed() {
            const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
            s.seconds += d;
            s.longest = d > s.longest ? d : s.longest;
            ++s.count;
        }
    };
    double wait_seconds_ = 0, busy_seconds_ = 0;
    VkQueryPool busy_pool_ = VK_NULL_HANDLE; // two timestamps per slot
    double window_start_ = 0;
};

} // namespace cafe::gpu::vk
