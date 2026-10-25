// GX2 command buffers: the main ring, display lists and timestamps.
//
// The main command buffer is carved from the pool GX2Init was given (or
// allocates), used as a ring: each flush submits the written part to the
// GPU and the next buffer starts right after it, waiting for the GPU to
// retire older buffers when the ring is full. Buffers are padded to 8
// dwords with type-2 fillers, as the console's GX2 does.

#include "gx2/internal.h"

#include "gpu/gpu.h"
#include "os/kernel.h"

#include "cafe/export.h"
#include "cafe/runtime.h"

#include <algorithm>
#include <deque>

namespace cafe::gx2 {
namespace {

using namespace latte;

constexpr uint32_t kMinBufferWords = 0x100;
constexpr uint32_t kMaxBufferWords = 0x20000;
constexpr uint32_t kDefaultPoolSize = 0x400000;

std::recursive_mutex g_api_mutex;

struct Buffer {
    uint32_t base = 0; // guest address
    uint32_t pos = 0;  // dwords written
    uint32_t size = 0; // dwords available
};

struct InFlight {
    uint32_t begin, end;
    uint64_t timestamp;
};

bool g_initialized = false;
uint32_t g_pool_base = 0;
uint32_t g_pool_end = 0;
Buffer g_main;
std::deque<InFlight> g_in_flight; // main-ring buffers the GPU has not retired, oldest first
bool g_direct_pending = false;    // a direct-called display list waits in the main buffer
uint32_t g_timeout_ms = 10000;

// The display list this guest thread is recording.
thread_local bool t_recording = false;
thread_local Buffer t_display_list;

void pad(Buffer& buffer) {
    while (buffer.pos % 8 != 0 && buffer.pos < buffer.size) {
        *guest<be<uint32_t>>(buffer.base + buffer.pos * 4) = pm4::kFiller;
        ++buffer.pos;
    }
}

// Finds room for at least `words` dwords in the ring after the last
// submitted buffer, waiting for the GPU if the ring is full.
void allocate(uint32_t words) {
    const uint32_t need = (std::max(words, kMinBufferWords) + 7) & ~7u;
    const uint32_t bytes = need * 4;
    if (bytes > g_pool_end - g_pool_base) fatal("GX2: a %u-byte command does not fit the command buffer pool", bytes);
    for (;;) {
        const uint64_t retired = gpu::retired_timestamp();
        while (!g_in_flight.empty() && g_in_flight.front().timestamp <= retired) g_in_flight.pop_front();

        uint32_t write = g_main.base + g_main.pos * 4;
        uint32_t free = 0;
        if (g_in_flight.empty()) {
            write = g_pool_base;
            free = g_pool_end - g_pool_base;
        } else {
            const uint32_t read = g_in_flight.front().begin;
            if (write > read) {
                if (g_pool_end - write >= bytes) {
                    free = g_pool_end - write;
                } else {
                    write = g_pool_base;
                    free = read - g_pool_base;
                }
            } else {
                free = read - write; // write == read: the ring is full
            }
        }
        if (free >= bytes) {
            g_main = {write, 0, std::min(free / 4, kMaxBufferWords) & ~7u};
            return;
        }
        const uint64_t oldest = g_in_flight.front().timestamp;
        if (!gpu::wait_timestamp(oldest, uint64_t{g_timeout_ms} * 1000000)) {
            fatal("GX2: the GPU did not retire command buffer %llu within %u ms",
                  static_cast<unsigned long long>(oldest), g_timeout_ms);
        }
    }
}

void submit_main() {
    g_direct_pending = false;
    if (g_main.pos == 0) return;
    pad(g_main);
    const uint64_t timestamp = gpu::submit(g_main.base, g_main.pos);
    g_in_flight.push_back({g_main.base, g_main.base + g_main.pos * 4, timestamp});
    g_main.base += g_main.pos * 4;
    g_main.size -= g_main.pos;
    g_main.pos = 0;
}

uint32_t reserve(uint32_t words) {
    if (t_recording) {
        Buffer& list = t_display_list;
        if (list.pos + words > list.size) {
            fatal("GX2: display list at 0x%08X overflows its %u bytes", list.base, list.size * 4);
        }
        const uint32_t address = list.base + list.pos * 4;
        list.pos += words;
        return address;
    }
    if (!g_initialized) fatal("GX2: a command was written before GX2Init");
    if (g_main.pos + words > g_main.size) {
        submit_main();
        allocate(words);
    }
    const uint32_t address = g_main.base + g_main.pos * 4;
    g_main.pos += words;
    return address;
}

} // namespace

std::recursive_mutex& api_mutex() { return g_api_mutex; }

void init_command_buffers(PPCContext& ctx, uint32_t pool_base, uint32_t pool_size) {
    if (pool_size == 0) pool_size = kDefaultPoolSize;
    pool_size = std::max(pool_size, 0x2000u) & ~0xFFu;
    if (pool_base == 0) pool_base = os::default_heap_alloc(ctx, pool_size, 0x100);
    if (pool_base == 0) fatal("GX2Init: could not allocate a %u-byte command buffer pool", pool_size);
    g_pool_base = pool_base;
    g_pool_end = pool_base + pool_size;
    g_main = {pool_base, 0, 0};
    g_in_flight.clear();
    g_initialized = true;
    allocate(kMinBufferWords);
}

bool initialized() { return g_initialized; }

void write(std::span<const uint32_t> dwords) {
    const uint32_t address = reserve(static_cast<uint32_t>(dwords.size()));
    be<uint32_t>* out = guest<be<uint32_t>>(address);
    for (size_t i = 0; i < dwords.size(); ++i) out[i] = dwords[i];
}

void flush() {
    if (t_recording) return; // the console ignores (and reports) a flush inside a display list
    submit_main();
    allocate(kMinBufferWords);
}

void begin_display_list(uint32_t buffer, uint32_t bytes) {
    if (t_recording) fatal("GX2: a display list was begun while another one is being recorded");
    t_recording = true;
    t_display_list = {buffer, 0, bytes / 4};
}

uint32_t end_display_list() {
    if (!t_recording) fatal("GX2: GX2EndDisplayList without a display list being recorded");
    pad(t_display_list);
    t_recording = false;
    return t_display_list.pos * 4;
}

bool recording_display_list() { return t_recording; }

void call_display_list(uint32_t buffer, uint32_t bytes) {
    write_packet(pm4::kIndirectBufferPriv, {buffer, 0, bytes / 4});
}

void direct_call_display_list(uint32_t buffer, uint32_t bytes) {
    // The console submits the list at once. Here it is called from the main buffer: one GPU
    // submission per flush instead of two per call (Smash's fights make hundreds of calls a frame).
    // Its timestamp still counts as submitted: asking for the last submitted timestamp submits it.
    call_display_list(buffer, bytes);
    if (!t_recording) g_direct_pending = true;
}

uint64_t last_submitted_timestamp() { return gpu::last_submitted_timestamp(); }
uint32_t gpu_timeout_ms() { return g_timeout_ms; }

// ---------------------------------------------------------------- packets
void write_packet(uint32_t opcode, std::initializer_list<uint32_t> data) {
    const uint32_t count = static_cast<uint32_t>(data.size());
    const uint32_t address = reserve(count + 1);
    be<uint32_t>* out = guest<be<uint32_t>>(address);
    out[0] = pm4::type3(opcode, count);
    uint32_t i = 1;
    for (const uint32_t value : data) out[i++] = value;
}

namespace {
void set_registers(uint32_t opcode, uint32_t space_base, uint32_t address, std::span<const uint32_t> values) {
    const uint32_t count = static_cast<uint32_t>(values.size());
    const uint32_t out_address = reserve(count + 2);
    be<uint32_t>* out = guest<be<uint32_t>>(out_address);
    out[0] = pm4::type3(opcode, count + 1);
    out[1] = (address - space_base) / 4;
    for (uint32_t i = 0; i < count; ++i) out[2 + i] = values[i];
}
} // namespace

void set_config_reg(uint32_t address, uint32_t value) { set_config_regs(address, {&value, 1}); }
void set_config_regs(uint32_t address, std::span<const uint32_t> values) {
    set_registers(pm4::kSetConfigReg, kConfigBase, address, values);
}
void set_context_reg(uint32_t address, uint32_t value) { set_context_regs(address, {&value, 1}); }
void set_context_regs(uint32_t address, std::span<const uint32_t> values) {
    set_registers(pm4::kSetContextReg, kContextBase, address, values);
}
void set_all_contexts_reg(uint32_t address, uint32_t value) {
    set_registers(pm4::kSetAllContexts, kContextBase, address, {&value, 1});
}
void set_loop_const(uint32_t address, uint32_t value) {
    set_registers(pm4::kSetLoopConst, kLoopConstBase, address, {&value, 1});
}
void set_ctl_const(uint32_t address, uint32_t value) {
    set_registers(pm4::kSetCtlConst, kCtlConstBase, address, {&value, 1});
}
void set_resource(uint32_t slot, const uint32_t (&words)[resource::kWords]) {
    set_registers(pm4::kSetResource, kResourceBase, kResourceBase + slot * resource::kWords * 4,
                  {words, resource::kWords});
}
void set_sampler(uint32_t slot, uint32_t word0, uint32_t word1, uint32_t word2) {
    const uint32_t words[] = {word0, word1, word2};
    set_registers(pm4::kSetSampler, kSamplerBase, kSamplerBase + slot * sampler::kWords * 4, words);
}

HlePacket& HlePacket::f32(float value) { return u32(std::bit_cast<uint32_t>(value)); }

void HlePacket::write() {
    std::vector<uint32_t> packet;
    packet.reserve(payload_.size() + 1);
    packet.push_back(pm4::type3(opcode_, static_cast<uint32_t>(payload_.size())));
    packet.insert(packet.end(), payload_.begin(), payload_.end());
    gx2::write(packet);
}

// ------------------------------------------------------------------ exports
namespace {

void GX2Flush() {
    ApiLock lock;
    flush();
}

uint64_t GX2GetLastSubmittedTimeStamp() {
    ApiLock lock;
    if (g_direct_pending) flush(); // direct calls count as submitted (see direct_call_display_list)
    return gpu::last_submitted_timestamp();
}
uint64_t GX2GetRetiredTimeStamp() { return gpu::retired_timestamp(); }

bool GX2WaitTimeStamp(uint64_t timestamp) {
    return gpu::wait_timestamp(timestamp, uint64_t{g_timeout_ms} * 1000000);
}

bool GX2DrawDone() {
    {
        ApiLock lock;
        flush();
    }
    return GX2WaitTimeStamp(gpu::last_submitted_timestamp());
}

uint32_t GX2GetGPUTimeout() { return g_timeout_ms; }
void GX2SetGPUTimeout(uint32_t ms) { g_timeout_ms = ms; }

void GX2BeginDisplayListEx(uint32_t list, uint32_t bytes, bool) {
    ApiLock lock;
    begin_display_list(list, bytes);
}
void GX2BeginDisplayList(uint32_t list, uint32_t bytes) { GX2BeginDisplayListEx(list, bytes, true); }
uint32_t GX2EndDisplayList(uint32_t) {
    ApiLock lock;
    return end_display_list();
}
void GX2CallDisplayList(uint32_t list, uint32_t bytes) {
    ApiLock lock;
    call_display_list(list, bytes);
}
void GX2DirectCallDisplayList(uint32_t list, uint32_t bytes) {
    ApiLock lock;
    direct_call_display_list(list, bytes);
}
bool GX2GetDisplayListWriteStatus() { return t_recording; }

} // namespace

CAFE_EXPORT(gx2, GX2Flush, GX2Flush);
CAFE_EXPORT(gx2, GX2GetLastSubmittedTimeStamp, GX2GetLastSubmittedTimeStamp);
CAFE_EXPORT(gx2, GX2GetRetiredTimeStamp, GX2GetRetiredTimeStamp);
CAFE_EXPORT(gx2, GX2WaitTimeStamp, GX2WaitTimeStamp);
CAFE_EXPORT(gx2, GX2DrawDone, GX2DrawDone);
CAFE_EXPORT(gx2, GX2GetGPUTimeout, GX2GetGPUTimeout);
CAFE_EXPORT(gx2, GX2SetGPUTimeout, GX2SetGPUTimeout);
CAFE_EXPORT(gx2, GX2BeginDisplayListEx, GX2BeginDisplayListEx);
CAFE_EXPORT(gx2, GX2BeginDisplayList, GX2BeginDisplayList);
CAFE_EXPORT(gx2, GX2EndDisplayList, GX2EndDisplayList);
CAFE_EXPORT(gx2, GX2CallDisplayList, GX2CallDisplayList);
CAFE_EXPORT(gx2, GX2DirectCallDisplayList, GX2DirectCallDisplayList);
CAFE_EXPORT(gx2, GX2GetDisplayListWriteStatus, GX2GetDisplayListWriteStatus);

} // namespace cafe::gx2
