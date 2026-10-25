// coreinit: memory regions and block operations, caches, time, atomics, the
// GHS C runtime hooks, console output and process exit.

#include "guest_format.h"
#include "kernel.h"

#include "cafe/export.h"
#include "cafe/layout.h"
#include "cafe/ppc_ops.h"
#include "cafe/sysmem.h"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

#include "host/local_time.h"

extern "C" uint64_t cafe_ppc_timebase(void);

namespace cafe::os {
namespace {

constexpr uint64_t kTimerHz = 62156250; // bus clock / 4
constexpr uint32_t kBusClock = 248625000;
constexpr uint32_t kCoreClock = 1243125000;
constexpr int64_t kUnixTo2000 = 946684800;

// ---------------------------------------------------------------- memory
// MEM2 available to the title: from the end of the loaded image to the top
// of the data area (set up by the heap module).
uint32_t g_mem2_begin = layout::kMem2Base;
uint32_t g_mem2_end = layout::kMem2End;

void OSGetMemBound(int32_t type, be<uint32_t>* base, be<uint32_t>* size) {
    uint32_t b = 0, s = 0;
    if (type == 1) {
        b = layout::kMem1Base;
        s = layout::kMem1Size;
    } else if (type == 2) {
        b = g_mem2_begin;
        s = g_mem2_end - g_mem2_begin;
    }
    if (base) *base = b;
    if (size) *size = s;
}

bool OSGetForegroundBucket(be<uint32_t>* base, be<uint32_t>* size) {
    if (base) *base = layout::kForegroundBucketBase;
    if (size) *size = layout::kForegroundBucketSize;
    return true; // the title is in the foreground
}

GuestAddress OSBlockMove(GuestAddress dst, GuestAddress src, uint32_t size, bool) {
    std::memmove(dst.as<uint8_t>(), src.as<uint8_t>(), size);
    return dst;
}

GuestAddress OSBlockSet(GuestAddress dst, uint32_t value, uint32_t size) {
    std::memset(dst.as<uint8_t>(), static_cast<int>(value), size);
    return dst;
}

GuestAddress guest_memcpy(GuestAddress dst, GuestAddress src, uint32_t size) {
    std::memcpy(dst.as<uint8_t>(), src.as<uint8_t>(), size);
    return dst;
}
GuestAddress guest_memmove(GuestAddress dst, GuestAddress src, uint32_t size) {
    std::memmove(dst.as<uint8_t>(), src.as<uint8_t>(), size);
    return dst;
}
GuestAddress guest_memset(GuestAddress dst, int32_t value, uint32_t size) {
    std::memset(dst.as<uint8_t>(), value, size);
    return dst;
}

// The host's caches are coherent with everything the runtime does, so cache
// maintenance has no effect; zeroing still has to happen.
void DCFlushRange(uint32_t, uint32_t) {}
void DCInvalidateRange(uint32_t, uint32_t) {}
void DCZeroRange(GuestAddress address, uint32_t size) {
    const uint32_t begin = address.value & ~31u;
    const uint32_t end = (address.value + size + 31) & ~31u;
    std::memset(guest_pointer(begin), 0, end - begin);
}
void OSMemoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
void OSEnforceInorderIO() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// ------------------------------------------------------------ locked cache
// Each core's 16 KiB scratchpad, handed out in 512-byte units. A plain
// memory range here; DMA to and from it is a copy.
constexpr uint32_t kLcGranule = 0x200;
uint8_t g_lc_used[3][layout::kLockedCacheSize / kLcGranule];

uint32_t LCAlloc(uint32_t size) {
    if (size == 0 || size > layout::kLockedCacheSize) return 0;
    size = (size + kLcGranule - 1) & ~(kLcGranule - 1);
    const uint32_t units = size / kLcGranule;
    KernelLock lock(kernel_mutex());
    const int core = current_thread()->core;
    auto& used = g_lc_used[core];
    for (uint32_t start = 0; start + units <= std::size(used); ++start) {
        bool free = true;
        for (uint32_t i = 0; i < units && free; ++i) free = used[start + i] == 0;
        if (!free) continue;
        for (uint32_t i = 0; i < units; ++i) used[start + i] = static_cast<uint8_t>(i == 0 ? units : 0xFF);
        return layout::kLockedCacheBase[core] + start * kLcGranule;
    }
    return 0;
}

void LCDealloc(uint32_t address) {
    KernelLock lock(kernel_mutex());
    for (int core = 0; core < 3; ++core) {
        const uint32_t base = layout::kLockedCacheBase[core];
        if (address < base || address >= base + layout::kLockedCacheSize) continue;
        const uint32_t start = (address - base) / kLcGranule;
        const uint32_t units = g_lc_used[core][start];
        for (uint32_t i = 0; i < units; ++i) g_lc_used[core][start + i] = 0;
    }
}

uint32_t LCGetUnallocated() {
    KernelLock lock(kernel_mutex());
    uint32_t free = 0;
    for (const uint8_t unit : g_lc_used[current_thread()->core]) free += unit == 0 ? kLcGranule : 0;
    return free;
}

bool LCEnableDMA() { return true; }

// Moves `blocks` 32-byte blocks between memory and the locked cache.
void LCStoreDMABlocks(GuestAddress memory, GuestAddress cache, uint32_t blocks) {
    std::memmove(memory.as<uint8_t>(), cache.as<uint8_t>(), (blocks == 0 ? 128 : blocks) * 32);
}
void LCLoadDMABlocks(GuestAddress cache, GuestAddress memory, uint32_t blocks) {
    std::memmove(cache.as<uint8_t>(), memory.as<uint8_t>(), (blocks == 0 ? 128 : blocks) * 32);
}
// The copies above finish before they return: nothing is ever queued.
bool LCIsDMAEnabled() { return true; }
void LCDisableDMA() {}
void LCWaitDMAQueue(uint32_t) {}

// ------------------------------------------------------------------- time
// Calendar ticks count from 2000-01-01 00:00:00 local time; the system time
// counts from boot.
int64_t ticks_since_2000() {
    const auto now = std::chrono::system_clock::now();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    const time_t seconds = static_cast<time_t>(ns / 1000000000);
    const int64_t local_ns = ns + host::utc_offset_seconds(seconds) * 1000000000;
    const int64_t since_2000 = local_ns - kUnixTo2000 * 1000000000;
    return since_2000 / 1000000000 * static_cast<int64_t>(kTimerHz) +
           since_2000 % 1000000000 * static_cast<int64_t>(kTimerHz) / 1000000000;
}

int64_t OSGetTime() { return ticks_since_2000(); }
int64_t OSGetSystemTime() { return static_cast<int64_t>(cafe_ppc_timebase()); }
uint32_t OSGetTick() { return static_cast<uint32_t>(cafe_ppc_timebase()); }

// Days since 2000-01-01 <-> civil date (proleptic Gregorian).
void civil_from_days(int64_t days, int32_t& year, int32_t& month0, int32_t& day) {
    days += 10957 + 719468; // to days since 0000-03-01
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    day = static_cast<int32_t>(doy - (153 * mp + 2) / 5 + 1);
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    year = static_cast<int32_t>(yoe + era * 400 + (m <= 2));
    month0 = static_cast<int32_t>(m - 1);
}

int64_t days_from_civil(int32_t year, int32_t month0, int32_t day) {
    const int32_t m = month0 + 1;
    year -= m <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 - 10957;
}

struct CalendarTime {
    be<int32_t> second, minute, hour, day_of_month, month, year, day_of_week, day_of_year,
        millisecond, microsecond;
};
static_assert(sizeof(CalendarTime) == 0x28);

void OSTicksToCalendarTime(int64_t ticks, CalendarTime* out) {
    const int64_t seconds = ticks / static_cast<int64_t>(kTimerHz);
    const int64_t sub = ticks % static_cast<int64_t>(kTimerHz);
    const int64_t days = seconds / 86400;
    const int64_t second_of_day = seconds % 86400;
    int32_t year, month, day;
    civil_from_days(days, year, month, day);
    out->second = static_cast<int32_t>(second_of_day % 60);
    out->minute = static_cast<int32_t>(second_of_day / 60 % 60);
    out->hour = static_cast<int32_t>(second_of_day / 3600);
    out->day_of_month = day;
    out->month = month;
    out->year = year;
    out->day_of_week = static_cast<int32_t>((days + 6) % 7); // 2000-01-01 was a Saturday
    out->day_of_year = static_cast<int32_t>(days - days_from_civil(year, 0, 1));
    out->millisecond = static_cast<int32_t>(sub * 1000 / static_cast<int64_t>(kTimerHz) % 1000);
    out->microsecond = static_cast<int32_t>(sub * 1000000 / static_cast<int64_t>(kTimerHz) % 1000);
}

int64_t OSCalendarTimeToTicks(const CalendarTime* in) {
    const int64_t days = days_from_civil(in->year, in->month, in->day_of_month);
    const int64_t seconds = days * 86400 + int64_t{in->hour} * 3600 + int64_t{in->minute} * 60 + in->second;
    return seconds * static_cast<int64_t>(kTimerHz) +
           (int64_t{in->millisecond} * 1000 + in->microsecond) * static_cast<int64_t>(kTimerHz) / 1000000;
}

struct SystemInfo {
    be<uint32_t> bus_clock, core_clock;
    be<uint64_t> ticks_since_2000;
    be<uint32_t> l2_cache_size[3];
    be<uint32_t> core_to_bus_ratio;
};
static_assert(sizeof(SystemInfo) == 0x20);
uint32_t g_system_info = 0;

GuestAddress OSGetSystemInfo() {
    if (g_system_info == 0) {
        g_system_info = system_alloc(sizeof(SystemInfo), 8);
        SystemInfo* info = guest<SystemInfo>(g_system_info);
        info->bus_clock = kBusClock;
        info->core_clock = kCoreClock;
        info->ticks_since_2000 = static_cast<uint64_t>(ticks_since_2000() - OSGetSystemTime());
        info->l2_cache_size[0] = 512 * 1024;
        info->l2_cache_size[1] = 2048 * 1024;
        info->l2_cache_size[2] = 512 * 1024;
        info->core_to_bus_ratio = kCoreClock / kBusClock;
    }
    return GuestAddress{g_system_info};
}

// ---------------------------------------------------------------- atomics
// 64-bit big-endian values in guest memory, updated atomically on the host.
uint64_t* atomic64(GuestAddress address) { return address.as<uint64_t>(); }

uint64_t OSGetAtomic64(GuestAddress address) {
    return __builtin_bswap64(__atomic_load_n(atomic64(address), __ATOMIC_SEQ_CST));
}

uint64_t OSSetAtomic64(GuestAddress address, uint64_t value) {
    return __builtin_bswap64(__atomic_exchange_n(atomic64(address), __builtin_bswap64(value), __ATOMIC_SEQ_CST));
}

uint64_t OSAddAtomic64(GuestAddress address, uint64_t value) {
    uint64_t* p = atomic64(address);
    uint64_t expected = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    for (;;) {
        const uint64_t updated = __builtin_bswap64(__builtin_bswap64(expected) + value);
        if (__atomic_compare_exchange_n(p, &expected, updated, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return __builtin_bswap64(expected);
        }
    }
}

// ----------------------------------------------------------- GHS runtime
// C library state the game's statically linked GHS libc keeps in coreinit:
// errno per thread, a global lock, stdio's _iob table and per-stream locks.
constexpr uint32_t kFopenMax = 100;
constexpr uint32_t kIobSize = 16;
uint32_t g_iob = 0; // _iob[kFopenMax], followed by the stream lock indices

GuestAddress __gh_errno_ptr() { return GuestAddress{current_thread()->guest + osthread::kErrno}; }
void __gh_set_errno(int32_t value) { field<int32_t>(current_thread()->guest, osthread::kErrno) = value; }

// A recursive lock that blocks like an OS mutex.
struct GuestRecursiveLock {
    Thread* owner = nullptr;
    int depth = 0;
    uint32_t key() const { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this)); }
    void lock() {
        KernelLock guard(kernel_mutex());
        Thread* self = current_thread();
        wait_until(guard, key(), [&] { return owner == nullptr || owner == self; });
        owner = self;
        ++depth;
    }
    bool try_lock() {
        KernelLock guard(kernel_mutex());
        Thread* self = current_thread();
        if (owner != nullptr && owner != self) return false;
        owner = self;
        ++depth;
        return true;
    }
    void unlock() {
        KernelLock guard(kernel_mutex());
        if (owner == current_thread() && --depth == 0) {
            owner = nullptr;
            wake(key());
        }
    }
};
GuestRecursiveLock g_ghs_lock;
GuestRecursiveLock g_stream_locks[kFopenMax];
bool g_stream_lock_used[kFopenMax];

void __ghsLock() { g_ghs_lock.lock(); }
void __ghsUnlock() { g_ghs_lock.unlock(); }

// A stream's lock word holds the index of the lock it was given.
void __ghs_flock_create(be<uint32_t>* lock_word) {
    KernelLock guard(kernel_mutex());
    for (uint32_t i = 0; i < kFopenMax; ++i) {
        if (!g_stream_lock_used[i]) {
            g_stream_lock_used[i] = true;
            *lock_word = i;
            return;
        }
    }
    fatal("__ghs_flock_create: all %u stream locks are in use", kFopenMax);
}

void __ghs_flock_destroy(uint32_t index) {
    KernelLock guard(kernel_mutex());
    if (index < kFopenMax) g_stream_lock_used[index] = false;
}

GuestAddress __ghs_flock_ptr(GuestAddress stream) {
    const uint32_t index = (stream.value - g_iob) / kIobSize;
    return GuestAddress{g_iob + kFopenMax * kIobSize + index * 4};
}

void __ghs_flock_file(uint32_t index) {
    if (index < kFopenMax) g_stream_locks[index].lock();
}
bool __ghs_ftrylock_file(uint32_t index) {
    return index < kFopenMax && g_stream_locks[index].try_lock();
}
void __ghs_funlock_file(uint32_t index) {
    if (index < kFopenMax) g_stream_locks[index].unlock();
}

void init_iob(uint32_t address) {
    g_iob = address;
    constexpr uint32_t kIn = 0x1, kOut = 0x2;
    const auto flags = [&](uint32_t stream) -> be<uint32_t>& {
        return *guest<be<uint32_t>>(address + stream * kIobSize + 12);
    };
    flags(0) = kIn | (0u << 18);
    flags(1) = kOut | (1u << 18);
    flags(2) = kOut | (2u << 18);
    for (uint32_t stream = 0; stream < 3; ++stream) {
        __ghs_flock_create(guest<be<uint32_t>>(address + kFopenMax * kIobSize + stream * 4));
    }
}
void init_fopen_max(uint32_t address) { *guest<be<uint16_t>>(address) = static_cast<uint16_t>(kFopenMax); }
// Pointers the game's C runtime fills in itself; they start out null.
void init_zero(uint32_t) {}

// ----------------------------------------------------------------- console
void OSConsoleWrite(GuestAddress text, uint32_t size) {
    std::fwrite(text.as<char>(), 1, size, stderr);
}

uint32_t g_atexit_cleanup = 0;
uint32_t g_stdio_cleanup = 0;

[[noreturn]] void terminate(int32_t status) {
    std::fflush(stdout);
    std::fflush(stderr);
    std::fprintf(stderr, "ttt2: the game exited with status %d\n", status);
    std::_Exit(status);
}

void guest_exit(PPCContext& ctx, int32_t status) {
    if (const uint32_t f = ppc::ld32(guest_base(), g_atexit_cleanup)) call_guest(ctx, f, {static_cast<uint32_t>(status)});
    if (const uint32_t f = ppc::ld32(guest_base(), g_stdio_cleanup)) call_guest(ctx, f, {});
    terminate(status);
}
void guest_quick_exit(int32_t status) { terminate(status); }

bool OSIsDebuggerInitialized() { return false; }
uint32_t bspGetHardwareVersion() { return 0x21201022; } // a retail console (Cemu's value)
void OSSetScreenCapturePermission(bool) {}
bool OSSavesDone_ReadyToRelease() { return true; }

} // namespace

void ticks_to_calendar_time(int64_t ticks, uint32_t calendar) {
    OSTicksToCalendarTime(ticks, guest<CalendarTime>(calendar));
}

void set_mem2_bounds(uint32_t begin, uint32_t end) {
    g_mem2_begin = begin;
    g_mem2_end = end;
}

// Calls with nothing to do on the host: cache flushes, dimming, the HOME
// menu, the debugger.
void no_op() {}
uint32_t zero() { return 0; }
uint32_t im_open() { return 1; } // an IM handle the IM_ calls ignore

CAFE_EXPORT(coreinit, DCFlushRangeNoSync, no_op);
CAFE_EXPORT(coreinit, OSCoherencyBarrier, no_op);
CAFE_EXPORT(coreinit, OSEnableHomeButtonMenu, no_op);
CAFE_EXPORT(coreinit, OSIsDebuggerPresent, zero);
CAFE_EXPORT(coreinit, OSGetSecurityLevel, zero);
CAFE_EXPORT(coreinit, IMDisableDim, zero);
CAFE_EXPORT(coreinit, IMEnableDim, zero);
CAFE_EXPORT(coreinit, IM_Open, im_open);
CAFE_EXPORT(coreinit, IM_Close, zero);
CAFE_EXPORT(coreinit, IM_SetDeviceState, zero);
CAFE_EXPORT(coreinit, IM_GetParameter, zero);

// OSGetSharedData(type, flags, address*, size*): system font `type` (0
// CafeCn, 1 CafeKr, 2 CafeStd, 3 CafeTw) as a TrueType file. Fonts are not
// part of a game dump: <font folder>/<name>.ttf if present (the console's own,
// decrypted), else a host font with Latin glyphs.
std::filesystem::path g_font_folder;
std::mutex g_font_mutex;
struct SharedFont {
    uint32_t address = 0, size = 0;
};
SharedFont g_fonts[4];
uint32_t g_shared_next = layout::kSharedDataBase;

std::optional<std::vector<uint8_t>> read_host_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void set_font_folder(const std::filesystem::path& folder) { g_font_folder = folder; }

bool OSGetSharedData(uint32_t type, uint32_t, be<uint32_t>* address, be<uint32_t>* size) {
    static constexpr const char* kNames[] = {"CafeCn", "CafeKr", "CafeStd", "CafeTw"};
    if (address) *address = 0;
    if (size) *size = 0;
    if (type >= 4) return false;
    std::lock_guard lock(g_font_mutex);
    SharedFont& font = g_fonts[type];
    if (font.address == 0) {
        std::filesystem::path chosen = g_font_folder / (std::string(kNames[type]) + ".ttf");
        auto data = read_host_file(chosen);
#ifdef _WIN32
        if (!data) {
            const char* windows = std::getenv("WINDIR");
            chosen = std::filesystem::path(windows ? windows : "C:\\Windows") / "Fonts" / "segoeui.ttf";
            data = read_host_file(chosen);
        }
#endif
        const uint32_t aligned = data ? static_cast<uint32_t>((data->size() + 0xFFF) & ~size_t{0xFFF}) : 0;
        if (!data || g_shared_next + aligned > layout::kSharedDataBase + layout::kSharedDataSize) {
            std::fprintf(stderr, "ttt2: no font for system font %s (put %s.ttf in %s)\n", kNames[type], kNames[type],
                         g_font_folder.string().c_str());
            return false;
        }
        commit_guest_memory(g_shared_next, aligned);
        std::memcpy(guest_pointer(g_shared_next), data->data(), data->size());
        font = {g_shared_next, static_cast<uint32_t>(data->size())};
        g_shared_next += aligned;
        std::fprintf(stderr, "ttt2: system font %s from %s\n", kNames[type], chosen.string().c_str());
    }
    if (address) *address = font.address;
    if (size) *size = font.size;
    return true;
}
CAFE_EXPORT(coreinit, OSGetSharedData, OSGetSharedData);

// COSWarn(module, format, ...)
CAFE_EXPORT_RAW(coreinit, COSWarn) {
    std::string text = format_guest(ctx, ctx.r[4], 5);
    if (text.empty() || text.back() != '\n') text += '\n';
    std::fprintf(stderr, "[guest warning] %s", text.c_str());
}

// OSPanic(file, line, format, ...)
CAFE_EXPORT_RAW(coreinit, OSPanic) {
    const std::string text = format_guest(ctx, ctx.r[5], 6);
    fatal("guest panic at %s:%u: %s", guest<char>(ctx.r[3]), ctx.r[4], text.c_str());
}

CAFE_EXPORT_RAW(coreinit, OSReport) {
    std::string text = format_guest(ctx, ctx.r[3], 4);
    if (text.empty() || text.back() != '\n') text += '\n';
    std::fprintf(stderr, "[guest] %s", text.c_str());
}

CAFE_EXPORT(coreinit, OSGetMemBound, OSGetMemBound);
CAFE_EXPORT(coreinit, OSGetForegroundBucket, OSGetForegroundBucket);
CAFE_EXPORT(coreinit, OSBlockMove, OSBlockMove);
CAFE_EXPORT(coreinit, OSBlockSet, OSBlockSet);
CAFE_EXPORT(coreinit, memcpy, guest_memcpy);
CAFE_EXPORT(coreinit, memmove, guest_memmove);
CAFE_EXPORT(coreinit, memset, guest_memset);
CAFE_EXPORT(coreinit, DCFlushRange, DCFlushRange);
CAFE_EXPORT(coreinit, DCInvalidateRange, DCInvalidateRange);
CAFE_EXPORT(coreinit, DCZeroRange, DCZeroRange);
CAFE_EXPORT(coreinit, OSMemoryBarrier, OSMemoryBarrier);
CAFE_EXPORT(coreinit, OSEnforceInorderIO, OSEnforceInorderIO);
CAFE_EXPORT(coreinit, LCAlloc, LCAlloc);
CAFE_EXPORT(coreinit, LCDealloc, LCDealloc);
CAFE_EXPORT(coreinit, LCGetUnallocated, LCGetUnallocated);
CAFE_EXPORT(coreinit, LCEnableDMA, LCEnableDMA);
CAFE_EXPORT(coreinit, LCStoreDMABlocks, LCStoreDMABlocks);
CAFE_EXPORT(coreinit, LCLoadDMABlocks, LCLoadDMABlocks);
CAFE_EXPORT(coreinit, LCIsDMAEnabled, LCIsDMAEnabled);
CAFE_EXPORT(coreinit, LCDisableDMA, LCDisableDMA);
CAFE_EXPORT(coreinit, LCWaitDMAQueue, LCWaitDMAQueue);
CAFE_EXPORT(coreinit, OSGetTime, OSGetTime);
CAFE_EXPORT(coreinit, OSGetSystemTime, OSGetSystemTime);
CAFE_EXPORT(coreinit, OSGetTick, OSGetTick);
CAFE_EXPORT(coreinit, OSTicksToCalendarTime, OSTicksToCalendarTime);
CAFE_EXPORT(coreinit, OSCalendarTimeToTicks, OSCalendarTimeToTicks);
CAFE_EXPORT(coreinit, OSGetSystemInfo, OSGetSystemInfo);
CAFE_EXPORT(coreinit, OSGetAtomic64, OSGetAtomic64);
CAFE_EXPORT(coreinit, OSSetAtomic64, OSSetAtomic64);
CAFE_EXPORT(coreinit, OSAddAtomic64, OSAddAtomic64);
CAFE_EXPORT(coreinit, __gh_errno_ptr, __gh_errno_ptr);
CAFE_EXPORT(coreinit, __gh_set_errno, __gh_set_errno);
CAFE_EXPORT(coreinit, __ghsLock, __ghsLock);
CAFE_EXPORT(coreinit, __ghsUnlock, __ghsUnlock);
CAFE_EXPORT(coreinit, __ghs_flock_create, __ghs_flock_create);
CAFE_EXPORT(coreinit, __ghs_flock_destroy, __ghs_flock_destroy);
CAFE_EXPORT(coreinit, __ghs_flock_ptr, __ghs_flock_ptr);
CAFE_EXPORT(coreinit, __ghs_flock_file, __ghs_flock_file);
CAFE_EXPORT(coreinit, __ghs_ftrylock_file, __ghs_ftrylock_file);
CAFE_EXPORT(coreinit, __ghs_funlock_file, __ghs_funlock_file);
CAFE_EXPORT(coreinit, OSConsoleWrite, OSConsoleWrite);
CAFE_EXPORT(coreinit, exit, guest_exit);
CAFE_EXPORT(coreinit, _Exit, guest_quick_exit);
CAFE_EXPORT(coreinit, OSIsDebuggerInitialized, OSIsDebuggerInitialized);
CAFE_EXPORT(coreinit, bspGetHardwareVersion, bspGetHardwareVersion);
CAFE_EXPORT(coreinit, OSSetScreenCapturePermission, OSSetScreenCapturePermission);
CAFE_EXPORT(coreinit, OSSavesDone_ReadyToRelease, OSSavesDone_ReadyToRelease);

CAFE_DATA_EXPORT(coreinit, _iob, init_iob);
CAFE_DATA_EXPORT(coreinit, __gh_FOPEN_MAX, init_fopen_max);
CAFE_DATA_EXPORT(coreinit, environ, init_zero);
CAFE_DATA_EXPORT(coreinit, __cpp_exception_init_ptr, init_zero);
CAFE_DATA_EXPORT(coreinit, __cpp_exception_cleanup_ptr, init_zero);
static void init_atexit_cleanup(uint32_t address) { g_atexit_cleanup = address; }
static void init_stdio_cleanup(uint32_t address) { g_stdio_cleanup = address; }
CAFE_DATA_EXPORT(coreinit, __atexit_cleanup, init_atexit_cleanup);
CAFE_DATA_EXPORT(coreinit, __stdio_cleanup, init_stdio_cleanup);

} // namespace cafe::os
