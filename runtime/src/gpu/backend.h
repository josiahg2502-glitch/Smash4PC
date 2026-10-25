#pragma once

// What the command processor asks a renderer to do. The register file holds
// the full Latte state at the time of each call; GX2 operations that the
// console performs with internal shaders arrive as the GX2 structures the
// title passed.
//
// The null backend renders nothing but keeps every observable effect of
// the command stream (timestamps, swaps, GPU-written memory) intact.

#include "gx2/types.h"

#include <cstdint>
#include <memory>

namespace cafe::gpu {

struct Registers {
    uint32_t value[0x10000]; // indexed by byte address / 4
    uint32_t operator[](uint32_t address) const { return value[address >> 2]; }
};

struct Draw {
    enum Source { kAuto, kIndexBuffer, kImmediate } source;
    uint32_t count;
    uint32_t index_address; // guest address of the indices (index buffer or the packet)
    uint32_t num_instances;
    bool use_opaque; // stream-out draw: the count comes from the stream-out buffer
    // Indices copied out of the command buffer (replayed immediate draws);
    // used instead of index_address when set.
    const uint8_t* host_indices = nullptr;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;
    virtual void draw(const Registers& regs, const Draw& draw) = 0;
    virtual void clear_color(const Registers& regs, const gx2::ColorBuffer& buffer, const float rgba[4]) = 0;
    virtual void clear_depth_stencil(const Registers& regs, const gx2::DepthBuffer& buffer, uint32_t flags,
                                     float depth, uint32_t stencil) = 0;
    virtual void copy_surface(const gx2::Surface& src, uint32_t src_level, uint32_t src_slice,
                              const gx2::Surface& dst, uint32_t dst_level, uint32_t dst_slice) = 0;
    virtual void resolve_color(const gx2::ColorBuffer& src, const gx2::Surface& dst, uint32_t dst_level,
                               uint32_t dst_slice) = 0;
    virtual void expand_depth(const gx2::DepthBuffer& buffer) = 0;
    virtual void convert_depth(const gx2::DepthBuffer& src, const gx2::Surface& dst, uint32_t dst_level,
                               uint32_t dst_slice) = 0;
    virtual void copy_to_scan_buffer(const gx2::ColorBuffer& buffer, uint32_t scan_target) = 0;
    // The frame is complete; the display shows it at the next flip.
    virtual void swap() = 0;
    // SURFACE_SYNC: GPU caches over [address, address + size) are flushed or
    // invalidated (CP_COHER_CNTL bits in `coherency_flags`).
    virtual void invalidate(uint32_t address, uint32_t size, uint32_t coherency_flags) = 0;
    // The CPU (or the DMA engine) wrote guest memory in [address, address + size).
    virtual void cpu_wrote(uint32_t, uint32_t) {}
    // Everything requested so far has finished on the GPU: called before the
    // command processor makes the CPU see that it has (retired timestamps,
    // end-of-pipe writes).
    virtual void sync() {}
    // Submits what has been requested so far without waiting; returns a
    // value wait_for() accepts once that work has finished on the GPU.
    virtual uint64_t flush() {
        sync();
        return 0;
    }
    virtual void wait_for(uint64_t) {}
    // The command processor has nothing queued or is about to wait for guest memory: work flush() held
    // back (to submit fewer, bigger batches) goes to the GPU now, so that a wait_for() on it ends.
    virtual void idle() {}
    // On the retirement thread, after wait_for(value) and before the CPU is
    // let see that the work up to `value` finished: memory the GPU writes on
    // the console that a backend keeps elsewhere goes to guest memory.
    virtual void retired(uint64_t) {}

    // Frame interpolation. A backend that shows more than one frame per
    // frame the title renders gets each finished frame's commands again
    // (register writes and operations, nothing the CPU can observe) once per
    // extra frame, between begin_replay(n) and end_replay(), n = 1 ..
    // frames_per_frame() - 1, after swap(). Then frame_shown() follows.
    virtual uint32_t frames_per_frame() const { return 1; }
    // Whether to render the extra frames of the frame just swapped: not
    // while the GPU has yet to finish the previous frame, as they would
    // make the title wait for its flips (its speed comes first).
    virtual bool want_replays() { return true; }
    // Before each extra frame after the first: false stops this frame's replays there (uncapped frame rate).
    virtual bool want_another_replay() { return true; }
    virtual void begin_replay(uint32_t) {}
    virtual void end_replay() {}
    virtual void frame_shown() {}
};

std::unique_ptr<Backend> make_null_backend();
// The Vulkan renderer, presenting in the window (host/window.h).
std::unique_ptr<Backend> make_vulkan_backend();

// TTT2_DUMP_SHADERS support (shader_dump.cpp): records the shaders of a draw.
void dump_shaders(const Registers& regs);

} // namespace cafe::gpu
