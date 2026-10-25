#pragma once

// The GPU side of the port. A command processor thread executes the PM4
// command buffers GX2 submits, in order, against a Latte register file and
// hands draws, clears, copies and scan-outs to a rendering backend. A
// display clock flips finished frames at the TV refresh rate.
//
// Addresses in command buffers are guest (effective) addresses: the title
// never translates them itself, so the runtime has no physical address map.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace cafe::gpu {

// Where compiled shaders and pipelines are kept between runs (before
// start()). Default: $XDG_CACHE_HOME/ttt2, ~/.cache/ttt2, or on Windows
// %LOCALAPPDATA%/ttt2.
void set_cache_directory(const std::filesystem::path& directory);

// The TV image's height in the title's pixels (720 or 1080: GX2SetTVBuffer).
// Upscaling targets the display: render targets the size of this image and
// its halvings are drawn larger. Any thread.
void set_tv_height(uint32_t rows);
uint32_t tv_height();

// Traces the next frame the GPU finishes: its draws, and with TTT2_CAPTURE
// set, its render targets as PNG files (F12 in the window). Any thread.
void request_frame_trace();

// Starts the command processor and the display clock. Idempotent.
void start();

// ---------------------------------------------------------------- overlay
// Images drawn over the picture in the window's pixels (an in-game menu, a
// frame counter): premultiplied RGBA rows, top first, at x, y; at most two.
// The picture under them is darkened by `dim` (0 to 1). They stay until
// replaced; no layers removes them. Any thread.
struct OverlayLayer {
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> pixels;
    float opacity = 1;
};
void set_overlay(float dim, std::vector<OverlayLayer> layers);

// The last half second: frames shown and finished by the title per second,
// the longest gap between shown frames, the GPU's time and the draws per
// title frame. Any thread.
struct LiveStats {
    float shown_fps = 0, game_fps = 0, worst_ms = 0, gpu_ms = 0, draws = 0;
};
LiveStats live_stats();

// ------------------------------------------------------------- submission
// Queues `words` PM4 dwords at guest address `buffer` (big-endian, as GX2
// writes them). Returns the timestamp that retires once the GPU has
// executed them. Timestamps start at 1 and increase by one per submission.
uint64_t submit(uint32_t buffer, uint32_t words);
uint64_t last_submitted_timestamp();
uint64_t retired_timestamp();
// Blocks the calling guest thread until `timestamp` has retired. Returns
// false if `timeout_ns` passed first.
bool wait_timestamp(uint64_t timestamp, uint64_t timeout_ns);

// The CPU wrote guest memory the GPU may hold a copy of (textures, shaders):
// GX2Invalidate with the CPU flag and DMA engine transfers. Takes effect
// before the next submission executes. Any thread.
void cpu_wrote(uint32_t address, uint32_t size);

// The clock the command processor samples for GPU timestamps, in CPU timer
// ticks (so GX2GPUTimeToCPUTime is the identity).
uint64_t clock();

// ---------------------------------------------------------------- display
struct SwapStatus {
    uint32_t swaps;      // swaps the CPU requested
    uint32_t flips;      // frames the display has shown
    uint64_t last_flip;  // system time of the last flip
    uint64_t last_vsync; // system time of the last vertical blank
};
SwapStatus swap_status();
void note_swap_requested();
// Called by the command processor when a swap command executes: the frame
// is complete and waits for the next vertical blank (or flips now with a
// swap interval of 0).
void frame_ready();
void wait_for_vsync();
void wait_for_flip();
// Vertical blanks from the host display (the presentation thread, when the
// display refreshes at a multiple of 60 Hz): the title's frames then keep
// step with the display's refreshes. `count` blanks passed since the last
// call (more than one after a stall). The display's own 59.94 Hz timer
// stands in whenever these stop for two frames; blanks it gave meanwhile
// count against the following calls' (up to four).
void host_vsync(uint32_t count = 1);
// The host's blanks restart without reporting the time they were missing
// (counted anew): the timer's blanks meanwhile stand.
void forget_timer_vsyncs();
// The host's blanks stop (presentation no longer follows the display): the
// timer stands in from its next period.
void host_vsync_stopped();
void set_swap_interval(uint32_t interval);
uint32_t swap_interval();

} // namespace cafe::gpu
