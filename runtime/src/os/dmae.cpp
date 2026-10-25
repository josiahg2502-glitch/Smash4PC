// dmae: the DMA engine. Transfers complete at once here, so every returned
// timestamp has already retired.
//
// The engine is little-endian: a copy without swapping moves bytes as they
// are; swap modes reverse each 16- or 32-bit unit. A fill stores the value
// as the CPU would, so the title reads back what it passed.

#include "kernel.h"

#include "gpu/gpu.h"

#include "cafe/export.h"

#include <atomic>
#include <cstring>

extern "C" uint64_t cafe_ppc_timebase(void);

namespace cafe::os {
namespace {

enum Endian : uint32_t { kNone = 0, kSwap16 = 1, kSwap32 = 2, kSwap64 = 3 };

std::atomic<uint64_t> g_retired{0};

uint64_t complete() {
    const uint64_t timestamp = cafe_ppc_timebase();
    g_retired.store(timestamp);
    return timestamp;
}

// DMAECopyMem(dst, src, size in 32-bit words, endian swap)
uint64_t DMAECopyMem(GuestAddress dst, GuestAddress src, uint32_t words, uint32_t endian) {
    uint8_t* out = dst.as<uint8_t>();
    const uint8_t* in = src.as<uint8_t>();
    const size_t bytes = size_t{words} * 4;
    switch (endian) {
    case kSwap16:
        for (size_t i = 0; i < bytes; i += 2) {
            const uint8_t a = in[i], b = in[i + 1];
            out[i] = b;
            out[i + 1] = a;
        }
        break;
    case kSwap32:
        for (size_t i = 0; i < bytes; i += 4) {
            uint32_t v;
            std::memcpy(&v, in + i, 4);
            v = __builtin_bswap32(v);
            std::memcpy(out + i, &v, 4);
        }
        break;
    case kSwap64:
        for (size_t i = 0; i + 8 <= bytes; i += 8) {
            uint64_t v;
            std::memcpy(&v, in + i, 8);
            v = __builtin_bswap64(v);
            std::memcpy(out + i, &v, 8);
        }
        break;
    default:
        std::memmove(out, in, bytes);
        break;
    }
    gpu::cpu_wrote(dst.value, static_cast<uint32_t>(bytes));
    return complete();
}

// DMAEFillMem(dst, 32-bit value, size in 32-bit words)
uint64_t DMAEFillMem(GuestAddress dst, uint32_t value, uint32_t words) {
    be<uint32_t>* out = dst.as<be<uint32_t>>();
    for (uint32_t i = 0; i < words; ++i) out[i] = value;
    gpu::cpu_wrote(dst.value, words * 4);
    return complete();
}

bool DMAEWaitDone(uint64_t) { return true; }

uint64_t DMAEGetRetiredTimeStamp() { return g_retired.load(); }
uint64_t DMAEGetLastSubmittedTimeStamp() { return g_retired.load(); }

} // namespace

CAFE_EXPORT(dmae, DMAECopyMem, DMAECopyMem);
CAFE_EXPORT(dmae, DMAEFillMem, DMAEFillMem);
CAFE_EXPORT(dmae, DMAEWaitDone, DMAEWaitDone);
CAFE_EXPORT(dmae, DMAEGetRetiredTimeStamp, DMAEGetRetiredTimeStamp);
CAFE_EXPORT(dmae, DMAEGetLastSubmittedTimeStamp, DMAEGetLastSubmittedTimeStamp);

} // namespace cafe::os
