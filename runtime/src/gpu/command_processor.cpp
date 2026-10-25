// The command processor: executes PM4 command buffers in submission order
// on its own host thread, as the GPU's CP does on the console.
//
// Register writes go to the Latte register file. While CONTEXT_CONTROL
// enables shadowing for a register type, writes are also stored to the
// shadow memory the last LOAD_* packet of that type named, which is how
// GX2ContextState records state; LOAD_* packets read registers back.
// Draws and the runtime's HLE packets go to the backend. Everything the
// CPU can observe (timestamps, memory the GPU writes, swap counts) is
// produced here regardless of backend.

#include "gpu/backend.h"
#include "gpu/gpu.h"
#include "gpu/latte.h"
#include "host/window.h"
#include "os/kernel.h"

#include "cafe/guest.h"
#include "cafe/runtime.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern "C" uint64_t cafe_ppc_timebase(void);

namespace cafe::gpu {
namespace {

using namespace latte;

struct Submission {
    uint32_t buffer;
    uint32_t words;
    uint64_t timestamp;
};

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<Submission> g_queue;
uint64_t g_submitted = 0; // under g_queue_mutex
uint64_t g_retired = 0;   // under the kernel lock

std::mutex g_written_mutex;
std::vector<std::pair<uint32_t, uint32_t>> g_written; // CPU writes not yet seen by the backend

std::once_flag g_started;
std::unique_ptr<Backend> g_backend;
Registers* g_regs = nullptr;

// One register type as CONTEXT_CONTROL and the LOAD_*/SET_* packets see it.
struct Space {
    uint32_t base;
    uint32_t end;
    uint32_t enable_bit;
    uint32_t shadow; // guest address of the shadow copy, 0 if none
};
enum SpaceId { kConfig, kContext, kAlu, kBool, kLoop, kResource, kSampler, kCtl, kSpaceCount };
Space g_spaces[kSpaceCount] = {
    {kConfigBase, kConfigEnd, pm4::context::kConfig, 0},
    {kContextBase, kContextEnd, pm4::context::kContext, 0},
    {kAluConstBase, kAluConstEnd, pm4::context::kAluConst, 0},
    {kBoolConstBase, kBoolConstEnd, pm4::context::kBoolConst, 0},
    {kLoopConstBase, kLoopConstEnd, pm4::context::kLoopConst, 0},
    {kResourceBase, kResourceEnd, pm4::context::kResource, 0},
    {kSamplerBase, kSamplerEnd, pm4::context::kSampler, 0},
    {kCtlConstBase, kCtlConstEnd, pm4::context::kCtlConst, 0},
};
uint32_t g_load_control = 0;
uint32_t g_shadow_enable = 0;

// ---------------------------------------------------------------- frame log
// With frame interpolation (Backend::frames_per_frame() > 1) the backend
// renders each finished frame again from a log of what it did, from one
// swap to the next: the register file at its start, then register writes
// and operations in order, with their arguments copied. Memory writes,
// timestamps and waits are not logged, so a replay changes nothing the
// title can observe.
struct LogEvent {
    enum Kind : uint8_t {
        kRegister, kDraw, kClearColor, kClearDepth, kCopySurface, kResolve, kExpandDepth, kConvertDepth, kCopyToScan
    };
    Kind kind;
    uint32_t a; // register: index; others: offset of the arguments in FrameLog::data
    uint32_t b; // register: value; draws: offset of copied indices (0: none)
};
struct ClearColorArgs { float rgba[4]; gx2::ColorBuffer buffer; };
struct ClearDepthArgs { uint32_t flags; float depth; uint32_t stencil; gx2::DepthBuffer buffer; };
struct CopySurfaceArgs { gx2::Surface src; uint32_t src_level, src_slice; gx2::Surface dst; uint32_t dst_level, dst_slice; };
struct ResolveArgs { gx2::ColorBuffer src; gx2::Surface dst; uint32_t level, slice; };
struct ConvertDepthArgs { gx2::DepthBuffer src; gx2::Surface dst; uint32_t level, slice; };
struct CopyToScanArgs { gx2::ColorBuffer buffer; uint32_t target; };

struct FrameLog {
    bool enabled = false;  // the backend interpolates
    bool complete = false; // started at a swap
    std::unique_ptr<Registers> start;
    std::vector<LogEvent> events;
    std::vector<uint8_t> data;

    uint32_t store(const void* bytes, size_t size) {
        const uint32_t offset = static_cast<uint32_t>(data.size());
        data.resize((data.size() + size + 7) & ~size_t{7});
        std::memcpy(data.data() + offset, bytes, size);
        return offset;
    }
    template <typename T>
    void op(LogEvent::Kind kind, const T& args, uint32_t extra = 0) {
        if (enabled) events.push_back({kind, store(&args, sizeof(T)), extra});
    }
};
FrameLog g_log;

void write_register(uint32_t index, uint32_t value) {
    g_regs->value[index] = value;
    if (g_log.enabled) g_log.events.push_back({LogEvent::kRegister, index, value});
}

uint32_t rd32(uint32_t address) { return byteswap_value(*guest<uint32_t>(address)); }
void wr32(uint32_t address, uint32_t value) { *guest<uint32_t>(address) = byteswap_value(value); }
void wr64(uint32_t address, uint64_t value) { *guest<uint64_t>(address) = byteswap_value(value); }

void set_register(uint32_t address, uint32_t value) {
    if (address >= kRegisterSpaceSize) fatal("GPU: register write outside the register space: 0x%05X", address);
    write_register(address >> 2, value);
}

// SET_*: [dword offset within the space, values...]
void set_registers(SpaceId id, uint32_t payload, uint32_t count) {
    Space& space = g_spaces[id];
    const uint32_t offset = rd32(payload) & 0xFFFF;
    const bool shadowed = (g_shadow_enable & space.enable_bit) && space.shadow != 0;
    for (uint32_t i = 1; i < count; ++i) {
        const uint32_t index = offset + i - 1;
        const uint32_t address = space.base + index * 4;
        if (address >= space.end) fatal("GPU: SET packet past the end of its register space (0x%05X)", address);
        const uint32_t value = rd32(payload + i * 4);
        write_register(address >> 2, value);
        if (shadowed) wr32(space.shadow + index * 4, value);
    }
}

// LOAD_*: [address lo, address hi, (dword offset, count)...]. Names the
// shadow copy and loads the listed ranges from it.
void load_registers(SpaceId id, uint32_t payload, uint32_t count) {
    Space& space = g_spaces[id];
    space.shadow = rd32(payload) & ~3u;
    if (!(g_load_control & space.enable_bit)) return;
    for (uint32_t i = 2; i + 1 < count; i += 2) {
        const uint32_t offset = rd32(payload + i * 4) & 0xFFFF;
        const uint32_t num = rd32(payload + (i + 1) * 4);
        for (uint32_t j = 0; j < num; ++j) {
            const uint32_t address = space.base + (offset + j) * 4;
            if (address >= space.end) fatal("GPU: LOAD packet past the end of its register space (0x%05X)", address);
            write_register(address >> 2, rd32(space.shadow + (offset + j) * 4));
        }
    }
}

bool compare(uint32_t function, uint32_t value, uint32_t reference) {
    switch (function) {
    case 0: return true;
    case 1: return value < reference;
    case 2: return value <= reference;
    case 3: return value == reference;
    case 4: return value != reference;
    case 5: return value >= reference;
    case 6: return value > reference;
    default: return true;
    }
}

void execute(uint32_t address, uint32_t words, int depth);

// ---------------------------------------------------------------- retirement
// What the CPU can observe about GPU work (retired timestamps, end-of-pipe
// memory writes) happens once that work has finished on
// the GPU, in command order, on a thread of its own; meanwhile the command
// processor goes on with the next commands, as the console's does.
struct Retirement {
    enum Kind : uint8_t { kTimestamp, kWrite32, kWrite64, kWriteClock } kind;
    uint64_t done;     // the backend's completion value (Backend::flush)
    uint32_t address;
    uint64_t data;
};
std::mutex g_retire_mutex;
std::condition_variable g_retire_cv;
std::deque<Retirement> g_retire_queue;

void retire_later(const Retirement& r) {
    std::lock_guard lock(g_retire_mutex);
    g_retire_queue.push_back(r);
    g_retire_cv.notify_one();
}

void retire(uint64_t timestamp);
void retirement_main() {
    pthread_setname_np(pthread_self(), "gpu retire");
    for (;;) {
        Retirement r;
        {
            std::unique_lock lock(g_retire_mutex);
            g_retire_cv.wait(lock, [] { return !g_retire_queue.empty(); });
            r = g_retire_queue.front();
            g_retire_queue.pop_front();
        }
        g_backend->wait_for(r.done);
        g_backend->retired(r.done);
        switch (r.kind) {
        case Retirement::kTimestamp: retire(r.data); break;
        case Retirement::kWrite32: wr32(r.address, static_cast<uint32_t>(r.data)); break;
        case Retirement::kWrite64: wr64(r.address, r.data); break;
        case Retirement::kWriteClock: wr64(r.address, clock()); break;
        }
    }
}

void start_frame_log() {
    if (!g_log.start) g_log.start = std::make_unique<Registers>();
    std::memcpy(g_log.start.get(), g_regs, sizeof(Registers));
    g_log.events.clear();
    g_log.data.assign(8, 0); // offset 0 means "no copied indices"
    g_log.complete = true;
}

// Renders the logged frame again, as extra frame `n`.
void replay_frame(uint32_t n) {
    static Registers* regs = new Registers{};
    std::memcpy(regs, g_log.start.get(), sizeof(Registers));
    g_backend->begin_replay(n);
    const uint8_t* data = g_log.data.data();
    const auto args = [&](const LogEvent& e) { return data + e.a; };
    for (const LogEvent& e : g_log.events) {
        switch (e.kind) {
        case LogEvent::kRegister: regs->value[e.a] = e.b; break;
        case LogEvent::kDraw: {
            Draw draw;
            std::memcpy(&draw, args(e), sizeof(Draw));
            if (e.b != 0) draw.host_indices = data + e.b;
            g_backend->draw(*regs, draw);
            break;
        }
        case LogEvent::kClearColor: {
            const auto* c = reinterpret_cast<const ClearColorArgs*>(args(e));
            g_backend->clear_color(*regs, c->buffer, c->rgba);
            break;
        }
        case LogEvent::kClearDepth: {
            const auto* c = reinterpret_cast<const ClearDepthArgs*>(args(e));
            g_backend->clear_depth_stencil(*regs, c->buffer, c->flags, c->depth, c->stencil);
            break;
        }
        case LogEvent::kCopySurface: {
            const auto* c = reinterpret_cast<const CopySurfaceArgs*>(args(e));
            g_backend->copy_surface(c->src, c->src_level, c->src_slice, c->dst, c->dst_level, c->dst_slice);
            break;
        }
        case LogEvent::kResolve: {
            const auto* c = reinterpret_cast<const ResolveArgs*>(args(e));
            g_backend->resolve_color(c->src, c->dst, c->level, c->slice);
            break;
        }
        case LogEvent::kExpandDepth:
            g_backend->expand_depth(*reinterpret_cast<const gx2::DepthBuffer*>(args(e)));
            break;
        case LogEvent::kConvertDepth: {
            const auto* c = reinterpret_cast<const ConvertDepthArgs*>(args(e));
            g_backend->convert_depth(c->src, c->dst, c->level, c->slice);
            break;
        }
        case LogEvent::kCopyToScan: {
            const auto* c = reinterpret_cast<const CopyToScanArgs*>(args(e));
            g_backend->copy_to_scan_buffer(c->buffer, c->target);
            break;
        }
        }
    }
    g_backend->end_replay();
}

// Stream-out: a draw appends a vertex per index to each enabled buffer.
void advance_stream_out(uint32_t vertices) {
    if (!((*g_regs)[reg::VGT_STRMOUT_EN] & 1)) return;
    const uint32_t enabled = (*g_regs)[reg::VGT_STRMOUT_BUFFER_EN];
    for (uint32_t i = 0; i < 4; ++i) {
        if (!(enabled & (1u << i))) continue;
        const uint32_t stride = (*g_regs)[reg::VGT_STRMOUT_VTX_STRIDE_0 + i * 16];
        const uint32_t offset_register = reg::VGT_STRMOUT_BUFFER_OFFSET_0 + i * 16;
        set_register(offset_register, (*g_regs)[offset_register] + vertices * stride);
    }
}

void execute_packet(uint32_t opcode, uint32_t payload, uint32_t count, uint32_t header_address, int depth) {
    const auto arg = [&](uint32_t i) { return rd32(payload + i * 4); };
    switch (opcode) {
    case pm4::kNop:
        break;
    case pm4::kContextControl:
        g_load_control = arg(0);
        g_shadow_enable = arg(1);
        break;
    case pm4::kSetConfigReg: set_registers(kConfig, payload, count); break;
    case pm4::kSetContextReg:
    case pm4::kSetAllContexts: set_registers(kContext, payload, count); break;
    case pm4::kSetAluConst: set_registers(kAlu, payload, count); break;
    case pm4::kSetBoolConst: set_registers(kBool, payload, count); break;
    case pm4::kSetLoopConst: set_registers(kLoop, payload, count); break;
    case pm4::kSetResource: set_registers(kResource, payload, count); break;
    case pm4::kSetSampler: set_registers(kSampler, payload, count); break;
    case pm4::kSetCtlConst: set_registers(kCtl, payload, count); break;
    case pm4::kLoadConfigReg: load_registers(kConfig, payload, count); break;
    case pm4::kLoadContextReg: load_registers(kContext, payload, count); break;
    case pm4::kLoadAluConst: load_registers(kAlu, payload, count); break;
    case pm4::kLoadBoolConst: load_registers(kBool, payload, count); break;
    case pm4::kLoadLoopConst: load_registers(kLoop, payload, count); break;
    case pm4::kLoadResource: load_registers(kResource, payload, count); break;
    case pm4::kLoadSampler: load_registers(kSampler, payload, count); break;
    case pm4::kLoadCtlConst: load_registers(kCtl, payload, count); break;

    case pm4::kIndirectBuffer:
    case pm4::kIndirectBufferPriv:
        // [address lo, address hi, size in dwords]
        if (depth >= 4) fatal("GPU: indirect buffers nested too deeply at 0x%08X", header_address);
        execute(arg(0) & ~3u, arg(2) & 0xFFFFF, depth + 1);
        break;

    case pm4::kIndexType:
        set_register(reg::VGT_DMA_INDEX_TYPE, arg(0));
        break;
    case pm4::kNumInstances:
        set_register(reg::VGT_DMA_NUM_INSTANCES, arg(0));
        break;
    case pm4::kDrawIndexAuto: {
        // [count, draw initiator]
        const Draw draw{Draw::kAuto, arg(0), 0, (*g_regs)[reg::VGT_DMA_NUM_INSTANCES], (arg(1) & (1u << 6)) != 0};
        g_log.op(LogEvent::kDraw, draw);
        dump_shaders(*g_regs);
        g_backend->draw(*g_regs, draw);
        advance_stream_out(draw.count * std::max<uint32_t>(draw.num_instances, 1));
        break;
    }
    case pm4::kDrawIndex2: {
        // [max indices, address lo, address hi, count, draw initiator]
        const Draw draw{Draw::kIndexBuffer, arg(3), arg(1), (*g_regs)[reg::VGT_DMA_NUM_INSTANCES], false};
        g_log.op(LogEvent::kDraw, draw);
        dump_shaders(*g_regs);
        g_backend->draw(*g_regs, draw);
        advance_stream_out(draw.count * std::max<uint32_t>(draw.num_instances, 1));
        break;
    }
    case pm4::kDrawIndexImmd: {
        // [count, draw initiator, indices...]
        const Draw draw{Draw::kImmediate, arg(0), payload + 8, (*g_regs)[reg::VGT_DMA_NUM_INSTANCES], false};
        if (g_log.enabled) {
            // The indices follow in the command buffer, which the title reuses.
            const uint32_t bytes = (count - 2) * 4;
            std::vector<uint8_t> indices(bytes);
            std::memcpy(indices.data(), guest<uint8_t>(payload + 8), bytes);
            const uint32_t at = g_log.store(indices.data(), bytes);
            g_log.op(LogEvent::kDraw, draw, at);
        }
        dump_shaders(*g_regs);
        g_backend->draw(*g_regs, draw);
        advance_stream_out(draw.count * std::max<uint32_t>(draw.num_instances, 1));
        break;
    }

    case pm4::kSurfaceSync:
        // [coherency control, size >> 8, base >> 8, poll interval]
        g_backend->invalidate(arg(2) << 8, arg(1) << 8, arg(0));
        break;
    case pm4::kEventWrite:
        // Cache flushes and pipeline events; the backend orders its own work.
        break;
    case pm4::kEventWriteEop: {
        // [event initiator, address lo, address hi (data/interrupt select), data lo, data hi]
        // Written once everything before it has finished.
        const uint32_t target = arg(1) & ~3u;
        switch (arg(2) >> 29) {
        case 1: retire_later({Retirement::kWrite32, g_backend->flush(), target, arg(3)}); break;
        case 2: retire_later({Retirement::kWrite64, g_backend->flush(), target, (uint64_t{arg(4)} << 32) | arg(3)}); break;
        case 3: retire_later({Retirement::kWriteClock, g_backend->flush(), target, 0}); break;
        default: break;
        }
        break;
    }
    case pm4::kMemWrite: {
        // [address lo, address hi (select), data lo, data hi]
        const uint32_t target = arg(0) & ~3u;
        const uint32_t select = arg(1);
        if (select & pm4::kMemWriteClock) {
            wr64(target, clock());
        } else if (select & pm4::kMemWriteData32) {
            wr32(target, arg(2));
        } else {
            wr64(target, (uint64_t{arg(3)} << 32) | arg(2));
        }
        break;
    }
    case pm4::kWaitRegMem: {
        // [function/space, address lo, address hi, reference, mask, poll interval]
        const uint32_t function = arg(0) & 7;
        if (!(arg(0) & (1u << 4))) break; // register space: nothing to wait for
        const uint32_t target = arg(1) & ~3u;
        if (!compare(function, rd32(target) & arg(4), arg(3))) g_backend->idle(); // what it waits for may need it
        while (!compare(function, rd32(target) & arg(4), arg(3))) {
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        break;
    }
    case pm4::kStrmoutBufferUpdate: {
        // [control, dst lo, dst hi, src lo, src hi]
        const uint32_t control = arg(0);
        const uint32_t buffer = (control >> 8) & 3;
        const uint32_t offset_register = reg::VGT_STRMOUT_BUFFER_OFFSET_0 + buffer * 16;
        if (control & 1) wr32(arg(1) & ~3u, (*g_regs)[offset_register]); // store filled size
        switch ((control >> 1) & 3) {
        case 0: set_register(offset_register, arg(3)); break;             // offset from packet
        case 2: set_register(offset_register, rd32(arg(3) & ~3u)); break; // offset from memory
        default: break;
        }
        break;
    }
    case pm4::kStrmoutBaseUpdate:
        break; // the base is also in VGT_STRMOUT_BUFFER_BASE_n
    case pm4::kCopyDw: {
        // [select, src lo, src hi, dst lo, dst hi]; memory -> register only
        const uint32_t select = arg(0);
        const uint32_t value = (select & 1) ? rd32(arg(1) & ~3u) : (*g_regs)[arg(1) << 2];
        if (select & 2) wr32(arg(3) & ~3u, value);
        else set_register(arg(3) << 2, value);
        break;
    }

    case pm4::kHleSwapBuffers:
        g_backend->swap();
        if (g_log.enabled) {
            // The replays read the frame's guest memory again: the title
            // learns the frame is done (and may reuse its buffers) after them.
            if (g_log.complete && g_backend->want_replays()) {
                for (uint32_t n = 1; n < g_backend->frames_per_frame(); ++n) {
                    if (n > 1 && !g_backend->want_another_replay()) break;
                    replay_frame(n);
                }
            }
            g_backend->frame_shown();
            start_frame_log();
        }
        {
            // The title paces on flips: waiting for this frame's GPU work too
            // missed blanks once it took most of a refresh (8 fighters at 4K).
            // One frame in flight: ready once the one before it has finished.
            static uint64_t previous = 0;
            const uint64_t done = g_backend->flush();
            if (previous != 0) {
                g_backend->idle(); // (a held-back submission would never finish)
                g_backend->wait_for(previous);
            }
            previous = done;
            frame_ready();
        }
        break;
    case pm4::kHleCopyColorToScan:
        g_log.op(LogEvent::kCopyToScan, CopyToScanArgs{*guest<gx2::ColorBuffer>(payload + 4), arg(0)});
        g_backend->copy_to_scan_buffer(*guest<gx2::ColorBuffer>(payload + 4), arg(0));
        break;
    case pm4::kHleClearColor: {
        float rgba[4];
        for (int i = 0; i < 4; ++i) rgba[i] = std::bit_cast<float>(arg(i));
        g_log.op(LogEvent::kClearColor, ClearColorArgs{{rgba[0], rgba[1], rgba[2], rgba[3]}, *guest<gx2::ColorBuffer>(payload + 16)});
        g_backend->clear_color(*g_regs, *guest<gx2::ColorBuffer>(payload + 16), rgba);
        break;
    }
    case pm4::kHleClearDepthStencil:
        g_log.op(LogEvent::kClearDepth, ClearDepthArgs{arg(0), std::bit_cast<float>(arg(1)), arg(2),
                                                       *guest<gx2::DepthBuffer>(payload + 12)});
        g_backend->clear_depth_stencil(*g_regs, *guest<gx2::DepthBuffer>(payload + 12), arg(0),
                                       std::bit_cast<float>(arg(1)), arg(2));
        break;
    case pm4::kHleCopySurface: {
        const uint32_t dst = payload + sizeof(gx2::Surface) + 8;
        g_log.op(LogEvent::kCopySurface,
                 CopySurfaceArgs{*guest<gx2::Surface>(payload), rd32(dst - 8), rd32(dst - 4), *guest<gx2::Surface>(dst),
                                 rd32(dst + sizeof(gx2::Surface)), rd32(dst + sizeof(gx2::Surface) + 4)});
        g_backend->copy_surface(*guest<gx2::Surface>(payload), rd32(dst - 8), rd32(dst - 4),
                                *guest<gx2::Surface>(dst), rd32(dst + sizeof(gx2::Surface)),
                                rd32(dst + sizeof(gx2::Surface) + 4));
        break;
    }
    case pm4::kHleResolveColor: {
        const uint32_t dst = payload + sizeof(gx2::ColorBuffer);
        g_log.op(LogEvent::kResolve, ResolveArgs{*guest<gx2::ColorBuffer>(payload), *guest<gx2::Surface>(dst),
                                                 rd32(dst + sizeof(gx2::Surface)), rd32(dst + sizeof(gx2::Surface) + 4)});
        g_backend->resolve_color(*guest<gx2::ColorBuffer>(payload), *guest<gx2::Surface>(dst),
                                 rd32(dst + sizeof(gx2::Surface)), rd32(dst + sizeof(gx2::Surface) + 4));
        break;
    }
    case pm4::kHleExpandDepth:
        g_log.op(LogEvent::kExpandDepth, *guest<gx2::DepthBuffer>(payload));
        g_backend->expand_depth(*guest<gx2::DepthBuffer>(payload));
        break;
    case pm4::kHleConvertDepth: {
        const uint32_t dst = payload + sizeof(gx2::DepthBuffer);
        g_log.op(LogEvent::kConvertDepth, ConvertDepthArgs{*guest<gx2::DepthBuffer>(payload), *guest<gx2::Surface>(dst),
                                                           rd32(dst + sizeof(gx2::Surface)),
                                                           rd32(dst + sizeof(gx2::Surface) + 4)});
        g_backend->convert_depth(*guest<gx2::DepthBuffer>(payload), *guest<gx2::Surface>(dst),
                                 rd32(dst + sizeof(gx2::Surface)), rd32(dst + sizeof(gx2::Surface) + 4));
        break;
    }

    default:
        fatal("GPU: unsupported PM4 packet, opcode 0x%02X with %u dwords at 0x%08X", opcode, count,
              header_address);
    }
}

void execute(uint32_t address, uint32_t words, int depth) {
    uint32_t pos = 0;
    while (pos < words) {
        const uint32_t header_address = address + pos * 4;
        const uint32_t header = rd32(header_address);
        switch (header >> 30) {
        case 0: {
            // Type 0: consecutive register writes from a register index.
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            const uint32_t base = (header & 0xFFFF) << 2;
            for (uint32_t i = 0; i < count; ++i) set_register(base + i * 4, rd32(header_address + 4 + i * 4));
            pos += 1 + count;
            break;
        }
        case 2:
            pos += 1;
            break;
        case 3: {
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            if (pos + 1 + count > words) {
                fatal("GPU: PM4 packet 0x%08X at 0x%08X runs past the end of its command buffer", header,
                      header_address);
            }
            execute_packet((header >> 8) & 0xFF, header_address + 4, count, header_address, depth);
            pos += 1 + count;
            break;
        }
        default:
            fatal("GPU: invalid PM4 header 0x%08X at 0x%08X", header, header_address);
        }
    }
}

void retire(uint64_t timestamp) {
    os::KernelLock lock(os::kernel_mutex());
    g_retired = timestamp;
    os::wake(os::kWaitGpu);
}

void command_processor_main() {
    pthread_setname_np(pthread_self(), "gpu");
    for (;;) {
        Submission s;
        {
            std::unique_lock lock(g_queue_mutex);
            if (g_queue.empty()) {
                lock.unlock();
                g_backend->idle();
                lock.lock();
            }
            g_queue_cv.wait(lock, [] { return !g_queue.empty(); });
            s = g_queue.front();
            g_queue.pop_front();
        }
        {
            std::vector<std::pair<uint32_t, uint32_t>> written;
            {
                std::lock_guard lock(g_written_mutex);
                written.swap(g_written);
            }
            for (const auto& [address, size] : written) g_backend->cpu_wrote(address, size);
        }
        execute(s.buffer, s.words, 0);
        retire_later({Retirement::kTimestamp, g_backend->flush(), 0, s.timestamp});
    }
}

} // namespace

void start_display();

void start() {
    std::call_once(g_started, [] {
        g_regs = new Registers{};
        // TTT2_GPU=null renders nothing (headless); otherwise Vulkan in the window.
        const char* choice = std::getenv("TTT2_GPU");
        if (host::window_open() && !(choice && std::strcmp(choice, "null") == 0)) g_backend = make_vulkan_backend();
        else g_backend = make_null_backend();
        std::fprintf(stderr, "ttt2: GPU backend: %s\n", g_backend->name());
        g_log.enabled = g_backend->frames_per_frame() > 1;
        std::thread(command_processor_main).detach();
        std::thread(retirement_main).detach();
        start_display();
    });
}

uint64_t submit(uint32_t buffer, uint32_t words) {
    std::lock_guard lock(g_queue_mutex);
    const uint64_t timestamp = ++g_submitted;
    g_queue.push_back({buffer, words, timestamp});
    g_queue_cv.notify_one();
    return timestamp;
}

uint64_t last_submitted_timestamp() {
    std::lock_guard lock(g_queue_mutex);
    return g_submitted;
}

uint64_t retired_timestamp() {
    os::KernelLock lock(os::kernel_mutex());
    return g_retired;
}

bool wait_timestamp(uint64_t timestamp, uint64_t timeout_ns) {
    os::KernelLock lock(os::kernel_mutex());
    return os::wait_until_for(lock, os::kWaitGpu, timeout_ns, [&] { return g_retired >= timestamp; });
}

uint64_t clock() { return cafe_ppc_timebase(); }

void cpu_wrote(uint32_t address, uint32_t size) {
    if (size == 0) return;
    std::lock_guard lock(g_written_mutex);
    g_written.emplace_back(address, size);
}

} // namespace cafe::gpu
