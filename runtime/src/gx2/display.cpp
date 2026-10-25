// GX2 display: TV and GamePad (DRC) scan buffers, swapping, vertical blank
// and flip waits, and GPU timer samples.
//
// The title renders into its own color buffers and copies the finished
// frame into a scan buffer; GX2SwapScanBuffers then queues it for display.
// The copy is a command the backend performs; the display clock (gpu/)
// decides when the frame flips.

#include "gx2/internal.h"

#include "gpu/gpu.h"

#include "cafe/export.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"

#include <cstdio>
#include <cstring>

namespace cafe::gx2 {
namespace {

using namespace latte;

// Swaps the CPU may queue ahead of the display before GX2SwapScanBuffers
// waits for a flip.
constexpr uint32_t kMaxPendingSwaps = 5;

struct ScanOut {
    uint32_t buffer = 0;
    uint32_t mode = 0; // GX2TVRenderMode / GX2DrcRenderMode, 0 = off
    uint32_t format = 0;
    uint32_t buffering = 0;
    bool enabled = false;
};
ScanOut g_tv, g_drc;
// Whether the title copied into each scan buffer (TV, GamePad) since the last
// swap; some render straight into the scan buffer instead.
bool g_copied[2];

struct Size {
    uint32_t width, height;
};
// GX2TVRenderMode: off, 480p 4:3, 480p 16:9, 720p, 720i, 1080p.
constexpr Size kTvSizes[] = {{0, 0}, {640, 480}, {854, 480}, {1280, 720}, {1280, 720}, {1920, 1080}};
constexpr Size kDrcSize{854, 480};

// A scan buffer's layout: a default-tiled 2D color/texture surface.
Surface scan_surface(uint32_t width, uint32_t height, uint32_t format) {
    Surface s{};
    s.use = surface_use::kTexture | surface_use::kColorBuffer;
    s.width = width;
    s.height = height;
    s.depth = 1;
    s.mip_levels = 1;
    s.dim = kDim2D;
    s.format = format;
    s.tile_mode = kTileDefault;
    calc_surface_size_and_alignment(s);
    return s;
}

// Every buffer of the chain, each aligned to the surface's alignment.
uint32_t chain_size(const Surface& s, uint32_t buffers, uint32_t factor) {
    const uint32_t pad = (s.alignment - s.image_size % s.alignment) % s.alignment;
    return ((s.image_size + pad) * buffers) * factor - pad;
}

void GX2CalcTVSize(uint32_t mode, uint32_t format, uint32_t buffering, be<uint32_t>* out_size,
                   be<uint32_t>* out_scale_needed) {
    if (mode >= std::size(kTvSizes)) fatal("GX2CalcTVSize: invalid TV render mode %u", mode);
    const Surface s = scan_surface(kTvSizes[mode].width, kTvSizes[mode].height, format);
    // 480-line modes are scaled by the display hardware from four buffers;
    // 720i holds both fields.
    const uint32_t buffers = mode < 3 ? 4 : buffering;
    const uint32_t factor = mode == 4 ? 2 : 1;
    if (out_size) *out_size = chain_size(s, buffers, factor);
    if (out_scale_needed) *out_scale_needed = 0u;
}

void GX2CalcDRCSize(uint32_t mode, uint32_t format, uint32_t buffering, be<uint32_t>* out_size,
                    be<uint32_t>* out_scale_needed) {
    const Surface s = mode != 0 ? scan_surface(kDrcSize.width, kDrcSize.height, format) : Surface{};
    if (out_size) *out_size = mode != 0 ? chain_size(s, buffering, 1) : 0u;
    if (out_scale_needed) *out_scale_needed = 0u;
}

void GX2SetTVBuffer(uint32_t buffer, uint32_t, uint32_t mode, uint32_t format, uint32_t buffering) {
    ApiLock lock;
    g_tv.buffer = buffer;
    g_tv.mode = mode;
    g_tv.format = format;
    g_tv.buffering = buffering;
    if (mode < std::size(kTvSizes)) gpu::set_tv_height(kTvSizes[mode].height);
    std::fprintf(stderr, "ttt2: gx2: TV scan buffer 0x%08X, mode %u, format 0x%03X, %u buffers\n", buffer, mode, format,
                 buffering);
}

void GX2SetDRCBuffer(uint32_t buffer, uint32_t, uint32_t mode, uint32_t format, uint32_t buffering) {
    ApiLock lock;
    g_drc.buffer = buffer;
    g_drc.mode = mode;
    g_drc.format = format;
    g_drc.buffering = buffering;
    std::fprintf(stderr, "ttt2: gx2: GamePad scan buffer 0x%08X, mode %u, format 0x%03X, %u buffers\n", buffer, mode,
                 format, buffering);
}

void GX2SetTVEnable(bool enable) { g_tv.enabled = enable; }
void GX2SetDRCEnable(bool enable) { g_drc.enabled = enable; }
void GX2SetTVScale(uint32_t, uint32_t) {}
void GX2SetDRCScale(uint32_t, uint32_t) {}

uint32_t GX2GetSystemTVScanMode() { return 7; }     // 1080p
uint32_t GX2GetSystemTVAspectRatio() { return 1; }  // 16:9
uint32_t GX2GetSystemDRCMode() { return 1; }        // single GamePad
bool GX2IsVideoOutReady() { return true; }

void GX2CopyColorBufferToScanBuffer(ColorBuffer* buffer, uint32_t target) {
    ApiLock lock;
    g_copied[target == 4 ? 1 : 0] = true;
    HlePacket(pm4::kHleCopyColorToScan).u32(target).guest_struct(*buffer).write();
}

// The scan buffer of a target (1 TV, 4 GamePad) as a color buffer.
// shortcut: always the chain's first buffer; follow the buffer being displayed if a title alternates between them.
ColorBuffer scan_color_buffer(uint32_t target) {
    const ScanOut& scan = target == 4 ? g_drc : g_tv;
    const Size size = target == 4 ? kDrcSize : kTvSizes[scan.mode < std::size(kTvSizes) ? scan.mode : 0];
    ColorBuffer buffer;
    std::memset(&buffer, 0, sizeof buffer);
    buffer.surface = scan_surface(size.width, size.height, scan.format);
    buffer.surface.image = scan.buffer;
    buffer.view_num_slices = 1u;
    init_color_buffer_regs(buffer);
    return buffer;
}

void GX2SwapScanBuffers() {
    {
        ApiLock lock;
        // A title that drew into the scan buffer itself shows that.
        for (const uint32_t target : {1u, 4u}) {
            const ScanOut& scan = target == 4 ? g_drc : g_tv;
            if (!g_copied[target == 4 ? 1 : 0] && scan.buffer != 0 && scan.mode != 0) {
                HlePacket(pm4::kHleCopyColorToScan).u32(target).guest_struct(scan_color_buffer(target)).write();
            }
        }
        g_copied[0] = g_copied[1] = false;
        gpu::note_swap_requested();
        write_packet(pm4::kHleSwapBuffers, {0});
        flush();
    }
    for (;;) {
        const gpu::SwapStatus status = gpu::swap_status();
        if (status.swaps - status.flips <= kMaxPendingSwaps) break;
        gpu::wait_for_flip();
    }
}

// Until the buffer the next frame will be copied into is no longer on
// screen: at most one swap waiting for its flip.
void GX2WaitForFreeScanBuffer(uint32_t) {
    for (;;) {
        const gpu::SwapStatus status = gpu::swap_status();
        if (status.swaps - status.flips <= 1) break;
        gpu::wait_for_flip();
    }
}

void GX2WaitForVsync() { gpu::wait_for_vsync(); }
void GX2WaitForFlip() { gpu::wait_for_flip(); }

void GX2GetSwapStatus(be<uint32_t>* swaps, be<uint32_t>* flips, be<uint64_t>* last_flip, be<uint64_t>* last_vsync) {
    const gpu::SwapStatus status = gpu::swap_status();
    if (swaps) *swaps = status.swaps;
    if (flips) *flips = status.flips;
    if (last_flip) *last_flip = status.last_flip;
    if (last_vsync) *last_vsync = status.last_vsync;
}

void GX2SetSwapInterval(uint32_t interval) { gpu::set_swap_interval(interval); }
uint32_t GX2GetSwapInterval() { return gpu::swap_interval(); }

// GPU timer samples: the GPU overwrites the -1 when the command executes.
void GX2SampleTopGPUCycle(be<uint64_t>* out) {
    ApiLock lock;
    *out = ~uint64_t{0};
    write_packet(pm4::kMemWrite, {guest_address(out) | endian::k8In64, pm4::kMemWriteClock, 0, 0});
}

void GX2SampleBottomGPUCycle(be<uint64_t>* out) {
    ApiLock lock;
    *out = ~uint64_t{0};
    write_packet(pm4::kEventWriteEop, {pm4::kEventBottomOfPipeTs | pm4::kEventIndexTs, guest_address(out) | endian::k8In64,
                                       pm4::kEopDataClock, 0, 0});
}

uint64_t GX2GPUTimeToCPUTime(uint64_t time) { return time; }

// GX2PipeEvent: top of pipe, bottom of pipe, bottom after cache flush.
void GX2SubmitUserTimeStamp(be<uint64_t>* out, uint64_t value, uint32_t event, bool) {
    ApiLock lock;
    const uint32_t address = guest_address(out) | endian::k8In64;
    const uint32_t lo = static_cast<uint32_t>(value), hi = static_cast<uint32_t>(value >> 32);
    if (event == 0) {
        write_packet(pm4::kMemWrite, {address, 0, lo, hi});
    } else {
        const uint32_t type = event == 2 ? pm4::kEventCacheFlushAndInvTs : pm4::kEventBottomOfPipeTs;
        write_packet(pm4::kEventWriteEop, {type | pm4::kEventIndexTs, address, pm4::kEopData64, lo, hi});
    }
}

// The scan buffer as a color buffer, for titles that read the displayed
// frame back (screenshots). GX2ScanTarget: 1 TV, 4 GamePad.
// shortcut: the backend does not write displayed frames back to guest memory; read them back if a title needs
// real screenshots.
void GX2GetCurrentScanBuffer(uint32_t target, ColorBuffer* out) {
    if (!out) return;
    ApiLock lock;
    *out = scan_color_buffer(target);
}
void GX2MarkScanBufferCopied(uint32_t) {}

// The GamePad never connects or disconnects: no callback to make. Returns
// the previous callback, none.
uint32_t GX2SetDRCConnectCallback(uint32_t, uint32_t) { return 0; }
// shortcut: event callbacks (vsync, flip, ...) are accepted and never called; call them if the title waits on them.
void GX2SetEventCallback(uint32_t, uint32_t, uint32_t) {}
void GX2DebugTagUserString(uint32_t, uint32_t) {}

} // namespace

void init_display() {
    g_tv = {};
    g_drc = {};
}

CAFE_EXPORT(gx2, GX2WaitForFreeScanBuffer, GX2WaitForFreeScanBuffer);
CAFE_EXPORT(gx2, GX2GetCurrentScanBuffer, GX2GetCurrentScanBuffer);
CAFE_EXPORT(gx2, GX2MarkScanBufferCopied, GX2MarkScanBufferCopied);
CAFE_EXPORT(gx2, GX2SetDRCConnectCallback, GX2SetDRCConnectCallback);
CAFE_EXPORT(gx2, GX2SetEventCallback, GX2SetEventCallback);
CAFE_EXPORT(gx2, GX2DebugTagUserString, GX2DebugTagUserString);
CAFE_EXPORT(gx2, GX2DebugTagUserStringVA, GX2DebugTagUserString);
CAFE_EXPORT(gx2, GX2CalcTVSize, GX2CalcTVSize);
CAFE_EXPORT(gx2, GX2CalcDRCSize, GX2CalcDRCSize);
CAFE_EXPORT(gx2, GX2SetTVBuffer, GX2SetTVBuffer);
CAFE_EXPORT(gx2, GX2SetDRCBuffer, GX2SetDRCBuffer);
CAFE_EXPORT(gx2, GX2SetTVEnable, GX2SetTVEnable);
CAFE_EXPORT(gx2, GX2SetDRCEnable, GX2SetDRCEnable);
CAFE_EXPORT(gx2, GX2SetTVScale, GX2SetTVScale);
CAFE_EXPORT(gx2, GX2SetDRCScale, GX2SetDRCScale);
CAFE_EXPORT(gx2, GX2GetSystemTVScanMode, GX2GetSystemTVScanMode);
CAFE_EXPORT(gx2, GX2GetSystemTVAspectRatio, GX2GetSystemTVAspectRatio);
CAFE_EXPORT(gx2, GX2GetSystemDRCMode, GX2GetSystemDRCMode);
CAFE_EXPORT(gx2, GX2IsVideoOutReady, GX2IsVideoOutReady);
CAFE_EXPORT(gx2, GX2CopyColorBufferToScanBuffer, GX2CopyColorBufferToScanBuffer);
CAFE_EXPORT(gx2, GX2SwapScanBuffers, GX2SwapScanBuffers);
CAFE_EXPORT(gx2, GX2WaitForVsync, GX2WaitForVsync);
CAFE_EXPORT(gx2, GX2WaitForFlip, GX2WaitForFlip);
CAFE_EXPORT(gx2, GX2GetSwapStatus, GX2GetSwapStatus);
CAFE_EXPORT(gx2, GX2SetSwapInterval, GX2SetSwapInterval);
CAFE_EXPORT(gx2, GX2GetSwapInterval, GX2GetSwapInterval);
CAFE_EXPORT(gx2, GX2SampleTopGPUCycle, GX2SampleTopGPUCycle);
CAFE_EXPORT(gx2, GX2SampleBottomGPUCycle, GX2SampleBottomGPUCycle);
CAFE_EXPORT(gx2, GX2GPUTimeToCPUTime, GX2GPUTimeToCPUTime);
CAFE_EXPORT(gx2, GX2SubmitUserTimeStamp, GX2SubmitUserTimeStamp);

} // namespace cafe::gx2
