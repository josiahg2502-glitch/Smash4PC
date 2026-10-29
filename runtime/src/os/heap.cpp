// coreinit MEM: expanded heaps, frame heaps, base heaps and the default heap.
//
// The heap handle is the start of the heap's region, where a small header is
// written, as on hardware. Allocation bookkeeping is kept on the host; every
// block still reserves the 16 bytes in front of it that Cafe's block header
// occupies, so neighbouring allocations sit where the game expects them.

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/layout.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"

#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace cafe::os {

void set_mem2_bounds(uint32_t begin, uint32_t end);

namespace {

constexpr uint32_t kExpMagic = 0x45585048; // "EXPH"
constexpr uint32_t kFrmMagic = 0x46524D48; // "FRMH"
constexpr uint32_t kHeaderSize = 0x40;
constexpr uint32_t kBlockHeader = 0x10;

struct ExpHeap {
    uint32_t begin, end;
    std::map<uint32_t, uint32_t> free;                       // start -> size
    std::set<std::pair<uint32_t, uint32_t>> by_size;         // {size, start} of the same blocks
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> used;  // payload -> [start, end)
};

struct FrameHeap {
    uint32_t begin, end;
    uint32_t head, tail;
};

std::mutex g_heap_mutex;
std::unordered_map<uint32_t, std::unique_ptr<ExpHeap>> g_exp_heaps;
std::unordered_map<uint32_t, std::unique_ptr<FrameHeap>> g_frame_heaps;
uint32_t g_base_heaps[9]; // MEMGetBaseHeapHandle arenas: 0 MEM1, 1 MEM2, 8 FG

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1); }

// The free list, by address and by size: allocations take the smallest block
// that fits (best fit) in log time instead of walking the list.
void add_free(ExpHeap& heap, uint32_t start, uint32_t size) {
    heap.free[start] = size;
    heap.by_size.insert({size, start});
}
std::map<uint32_t, uint32_t>::iterator remove_free(ExpHeap& heap, std::map<uint32_t, uint32_t>::iterator it) {
    heap.by_size.erase({it->second, it->first});
    return heap.free.erase(it);
}

uint32_t create_exp_heap(uint32_t start, uint32_t size) {
    const uint32_t begin = align_up(start, 4);
    const uint32_t end = align_down(start + size, 4);
    if (end <= begin + kHeaderSize + kBlockHeader) return 0;
    *guest<be<uint32_t>>(begin) = kExpMagic;
    auto heap = std::make_unique<ExpHeap>();
    heap->begin = begin + kHeaderSize;
    heap->end = end;
    add_free(*heap, heap->begin, end - heap->begin);
    g_exp_heaps[begin] = std::move(heap);
    return begin;
}

ExpHeap& exp_heap(uint32_t handle) {
    const auto it = g_exp_heaps.find(handle);
    if (it == g_exp_heaps.end()) fatal("0x%08X is not an expanded heap", handle);
    return *it->second;
}

// Positive alignment allocates from the low end, negative from the high end.
uint32_t exp_alloc(ExpHeap& heap, uint32_t size, int32_t alignment) {
    const bool from_tail = alignment < 0;
    uint32_t a = static_cast<uint32_t>(from_tail ? -alignment : alignment);
    if (a < 4) a = 4;
    size = align_up(size == 0 ? 1 : size, 4);
    const auto take = [&](std::map<uint32_t, uint32_t>::iterator it, uint32_t start, uint32_t end,
                          uint32_t payload) {
        const uint32_t block = it->first, block_end = it->first + it->second;
        remove_free(heap, it);
        if (start > block) add_free(heap, block, start - block);
        if (end < block_end) add_free(heap, end, block_end - end);
        heap.used[payload] = {start, end};
        return payload;
    };
    if (!from_tail) {
        // Blocks at least big enough without alignment padding, smallest first.
        for (auto by = heap.by_size.lower_bound({size + kBlockHeader, 0}); by != heap.by_size.end(); ++by) {
            const auto [block_size, block] = *by;
            const uint32_t payload = align_up(block + kBlockHeader, a);
            if (uint64_t{payload} + size <= uint64_t{block} + block_size) {
                return take(heap.free.find(block), payload - kBlockHeader, payload + size, payload);
            }
        }
    } else {
        for (auto it = heap.free.rbegin(); it != heap.free.rend(); ++it) {
            const uint32_t block_end = it->first + it->second;
            if (it->second < size + kBlockHeader) continue;
            const uint32_t payload = align_down(block_end - size, a);
            if (payload >= it->first + kBlockHeader) {
                return take(std::next(it).base(), payload - kBlockHeader, payload + size, payload);
            }
        }
    }
    return 0;
}

void exp_free(ExpHeap& heap, uint32_t payload) {
    const auto it = heap.used.find(payload);
    if (it == heap.used.end()) fatal("free of 0x%08X, which this heap did not allocate", payload);
    uint32_t start = it->second.first;
    uint32_t end = it->second.second;
    heap.used.erase(it);
    auto next = heap.free.lower_bound(start);
    if (next != heap.free.end() && next->first == end) {
        end += next->second;
        next = remove_free(heap, next);
    }
    if (next != heap.free.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start) {
            start = previous->first;
            remove_free(heap, previous);
        }
    }
    add_free(heap, start, end - start);
}

uint32_t create_frame_heap(uint32_t start, uint32_t size) {
    const uint32_t begin = align_up(start, 4);
    const uint32_t end = align_down(start + size, 4);
    if (end <= begin + kHeaderSize) return 0;
    *guest<be<uint32_t>>(begin) = kFrmMagic;
    g_frame_heaps[begin] = std::make_unique<FrameHeap>(
        FrameHeap{begin + kHeaderSize, end, begin + kHeaderSize, end});
    return begin;
}

// ------------------------------------------------------------------ exports
uint32_t MEMCreateExpHeapEx(GuestAddress start, uint32_t size, uint32_t) {
    std::lock_guard lock(g_heap_mutex);
    return create_exp_heap(start.value, size);
}

GuestAddress MEMDestroyExpHeap(GuestAddress heap) {
    std::lock_guard lock(g_heap_mutex);
    g_exp_heaps.erase(heap.value);
    return heap;
}

uint32_t MEMAllocFromExpHeapEx(uint32_t heap, uint32_t size, int32_t alignment) {
    std::lock_guard lock(g_heap_mutex);
    return exp_alloc(exp_heap(heap), size, alignment);
}

void MEMFreeToExpHeap(uint32_t heap, uint32_t payload) {
    if (payload == 0) return;
    std::lock_guard lock(g_heap_mutex);
    exp_free(exp_heap(heap), payload);
}

uint32_t MEMGetAllocatableSizeForExpHeapEx(uint32_t heap, int32_t alignment) {
    std::lock_guard lock(g_heap_mutex);
    uint32_t a = static_cast<uint32_t>(alignment < 0 ? -alignment : alignment);
    if (a < 4) a = 4;
    uint32_t best = 0;
    for (const auto& [start, size] : exp_heap(heap).free) {
        const uint32_t payload = align_up(start + kBlockHeader, a);
        if (payload < start + size) best = std::max(best, align_down(start + size - payload, 4));
    }
    return best;
}

uint32_t MEMAllocFromFrmHeapEx(uint32_t handle, uint32_t size, int32_t alignment) {
    std::lock_guard lock(g_heap_mutex);
    const auto it = g_frame_heaps.find(handle);
    if (it == g_frame_heaps.end()) fatal("0x%08X is not a frame heap", handle);
    FrameHeap& heap = *it->second;
    uint32_t a = static_cast<uint32_t>(alignment < 0 ? -alignment : alignment);
    if (a < 4) a = 4;
    size = align_up(size, 4);
    if (alignment >= 0) {
        const uint32_t start = align_up(heap.head, a);
        if (uint64_t{start} + size > heap.tail) return 0;
        heap.head = start + size;
        return start;
    }
    if (heap.tail < size) return 0;
    const uint32_t start = align_down(heap.tail - size, a);
    if (start < heap.head) return 0;
    heap.tail = start;
    return start;
}

uint32_t MEMGetBaseHeapHandle(uint32_t arena) { return arena < 9 ? g_base_heaps[arena] : 0; }

uint32_t default_alloc(uint32_t size) { return MEMAllocFromExpHeapEx(g_base_heaps[1], size, 0x40); }
uint32_t default_alloc_ex(uint32_t size, int32_t alignment) {
    return MEMAllocFromExpHeapEx(g_base_heaps[1], size, alignment);
}
void default_free(uint32_t payload) { MEMFreeToExpHeap(g_base_heaps[1], payload); }

void thunk_default_alloc(PPCContext& ctx, uint8_t*) { abi::invoke<default_alloc>(ctx); }
void thunk_default_alloc_ex(PPCContext& ctx, uint8_t*) { abi::invoke<default_alloc_ex>(ctx); }
void thunk_default_free(PPCContext& ctx, uint8_t*) { abi::invoke<default_free>(ctx); }

// The default-heap entry points are function pointers the game reads from
// coreinit's data and may replace with its own allocator.
uint32_t g_alloc_ex_slot = 0;
uint32_t g_free_slot = 0;

void init_alloc_pointer(uint32_t slot) {
    *guest<be<uint32_t>>(slot) = register_host_function(thunk_default_alloc, "MEMAllocFromDefaultHeap");
}
void init_alloc_ex_pointer(uint32_t slot) {
    g_alloc_ex_slot = slot;
    *guest<be<uint32_t>>(slot) = register_host_function(thunk_default_alloc_ex, "MEMAllocFromDefaultHeapEx");
}
void init_free_pointer(uint32_t slot) {
    g_free_slot = slot;
    *guest<be<uint32_t>>(slot) = register_host_function(thunk_default_free, "MEMFreeToDefaultHeap");
}

} // namespace

// Allocation from the default heap as other OS libraries do it: through the
// pointers, so a title's replacement allocator is used.
uint32_t default_heap_alloc(PPCContext& ctx, uint32_t size, uint32_t alignment) {
    if (g_alloc_ex_slot == 0) return default_alloc_ex(size, static_cast<int32_t>(alignment));
    return call_guest(ctx, *guest<be<uint32_t>>(g_alloc_ex_slot), {size, alignment});
}

void default_heap_free(PPCContext& ctx, uint32_t block) {
    if (g_free_slot == 0) return default_free(block);
    call_guest(ctx, *guest<be<uint32_t>>(g_free_slot), {block});
}

// Base heaps as the OS creates them before the title starts: MEM1 and the
// foreground bucket as frame heaps, the rest of MEM2 as the default
// expanded heap.
void init_heaps(uint32_t mem2_begin) {
    std::lock_guard lock(g_heap_mutex);
    const uint32_t begin = align_up(mem2_begin, 0x1000);
    commit_guest_memory(begin, layout::kMem2End - begin);
    commit_guest_memory(layout::kMem1Base, layout::kMem1Size);
    commit_guest_memory(layout::kForegroundBucketBase, layout::kForegroundBucketSize);
    for (const uint32_t lc : layout::kLockedCacheBase) commit_guest_memory(lc, layout::kLockedCacheSize);
    // The top page reads as zeros: a lookup that finds nothing (-1) and indexes from a null table lands
    // just below 4 GiB, which the console survives (Smash's per-costume announcer call with the US4MP
    // mod pack's sound config, which has no such table).
    commit_guest_memory(0xFFFFF000u, 0x1000);
    set_mem2_bounds(begin, layout::kMem2End);
    g_base_heaps[0] = create_frame_heap(layout::kMem1Base, layout::kMem1Size);
    g_base_heaps[1] = create_exp_heap(begin, layout::kMem2End - begin);
    // The first 40 MiB of the bucket are free for the title.
    g_base_heaps[8] = create_frame_heap(layout::kForegroundBucketBase, 0x2800000);
}

void run_preinit_user(PPCContext& ctx, uint32_t function) {
    uint32_t handles[3];
    {
        std::lock_guard lock(g_heap_mutex);
        handles[0] = g_base_heaps[0];
        handles[1] = g_base_heaps[8];
        handles[2] = g_base_heaps[1];
    }
    const uint32_t slots = system_alloc(sizeof handles, 4);
    auto* guest = reinterpret_cast<uint32_t*>(guest_pointer(slots));
    for (int i = 0; i < 3; ++i) guest[i] = __builtin_bswap32(handles[i]);
    // Its registers are not the entry point's: keep argc/argv and the rest.
    const PPCContext saved = ctx;
    call_guest(ctx, function, {slots, slots + 4, slots + 8});
    ctx = saved;
    std::lock_guard lock(g_heap_mutex);
    g_base_heaps[0] = __builtin_bswap32(guest[0]);
    g_base_heaps[8] = __builtin_bswap32(guest[1]);
    g_base_heaps[1] = __builtin_bswap32(guest[2]);
}

CAFE_EXPORT(coreinit, MEMCreateExpHeapEx, MEMCreateExpHeapEx);
CAFE_EXPORT(coreinit, MEMDestroyExpHeap, MEMDestroyExpHeap);
CAFE_EXPORT(coreinit, MEMAllocFromExpHeapEx, MEMAllocFromExpHeapEx);
CAFE_EXPORT(coreinit, MEMFreeToExpHeap, MEMFreeToExpHeap);
CAFE_EXPORT(coreinit, MEMGetAllocatableSizeForExpHeapEx, MEMGetAllocatableSizeForExpHeapEx);
CAFE_EXPORT(coreinit, MEMAllocFromFrmHeapEx, MEMAllocFromFrmHeapEx);
uint32_t MEMGetTotalFreeSizeForExpHeap(uint32_t heap) {
    std::lock_guard lock(g_heap_mutex);
    uint32_t total = 0;
    for (const auto& [start, size] : exp_heap(heap).free) total += size > kBlockHeader ? size - kBlockHeader : 0;
    return total;
}

// shortcut: allocations always come first-fit; honour the mode if a title depends on placement.
uint32_t MEMSetAllocModeForExpHeap(uint32_t, uint32_t) { return 0; }

uint32_t MEMGetAllocatableSizeForFrmHeapEx(uint32_t handle, int32_t alignment) {
    std::lock_guard lock(g_heap_mutex);
    const auto it = g_frame_heaps.find(handle);
    if (it == g_frame_heaps.end()) return 0;
    uint32_t a = static_cast<uint32_t>(alignment < 0 ? -alignment : alignment);
    if (a < 4) a = 4;
    const uint32_t start = align_up(it->second->head, a);
    return start < it->second->tail ? align_down(it->second->tail - start, 4) : 0;
}

// MEMAllocator: {functions, heap, param1, param2}; functions: {alloc, free}.
uint32_t MEMAllocFromAllocator(PPCContext& ctx, GuestAddress allocator, uint32_t size) {
    const uint32_t functions = *guest<be<uint32_t>>(allocator.value);
    return call_guest(ctx, *guest<be<uint32_t>>(functions), {allocator.value, size});
}
void MEMFreeToAllocator(PPCContext& ctx, GuestAddress allocator, uint32_t block) {
    const uint32_t functions = *guest<be<uint32_t>>(allocator.value);
    call_guest(ctx, *guest<be<uint32_t>>(functions + 4), {allocator.value, block});
}

CAFE_EXPORT(coreinit, MEMGetBaseHeapHandle, MEMGetBaseHeapHandle);
CAFE_EXPORT(coreinit, MEMGetTotalFreeSizeForExpHeap, MEMGetTotalFreeSizeForExpHeap);
CAFE_EXPORT(coreinit, MEMSetAllocModeForExpHeap, MEMSetAllocModeForExpHeap);
CAFE_EXPORT(coreinit, MEMGetAllocatableSizeForFrmHeapEx, MEMGetAllocatableSizeForFrmHeapEx);
CAFE_EXPORT(coreinit, MEMAllocFromAllocator, MEMAllocFromAllocator);
CAFE_EXPORT(coreinit, MEMFreeToAllocator, MEMFreeToAllocator);
CAFE_DATA_EXPORT(coreinit, MEMAllocFromDefaultHeap, init_alloc_pointer);
CAFE_DATA_EXPORT(coreinit, MEMAllocFromDefaultHeapEx, init_alloc_ex_pointer);
CAFE_DATA_EXPORT(coreinit, MEMFreeToDefaultHeap, init_free_pointer);

} // namespace cafe::os
