#include "cafe/sysmem.h"

#include "cafe/layout.h"
#include "cafe/runtime.h"

#include <cstring>
#include <map>
#include <mutex>

namespace cafe {
namespace {

std::mutex g_mutex;
std::map<uint32_t, uint32_t> g_free;      // address -> size
std::map<uint32_t, uint32_t> g_allocated; // address -> size

} // namespace

void init_system_heap() {
    commit_guest_memory(layout::kSystemHeapBase, layout::kSystemHeapSize);
    g_free[layout::kSystemHeapBase] = layout::kSystemHeapSize;
}

uint32_t system_alloc(uint32_t size, uint32_t alignment) {
    std::lock_guard lock(g_mutex);
    size = (size + 15) & ~15u;
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        const uint32_t start = (it->first + alignment - 1) & ~(alignment - 1);
        const uint32_t end = it->first + it->second;
        if (start + size > end) continue;
        const uint32_t block = it->first;
        g_free.erase(it);
        if (start > block) g_free[block] = start - block;
        if (start + size < end) g_free[start + size] = end - (start + size);
        g_allocated[start] = size;
        std::memset(guest_pointer(start), 0, size);
        return start;
    }
    fatal("system heap exhausted (request of 0x%X bytes)", size);
}

void system_free(uint32_t address) {
    std::lock_guard lock(g_mutex);
    const auto it = g_allocated.find(address);
    if (it == g_allocated.end()) {
        fatal("system_free of 0x%08X, which was not allocated", address);
    }
    uint32_t start = address;
    uint32_t size = it->second;
    g_allocated.erase(it);
    auto next = g_free.lower_bound(start);
    if (next != g_free.end() && next->first == start + size) {
        size += next->second;
        next = g_free.erase(next);
    }
    if (next != g_free.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start) {
            start = previous->first;
            size += previous->second;
            g_free.erase(previous);
        }
    }
    g_free[start] = size;
}

} // namespace cafe
