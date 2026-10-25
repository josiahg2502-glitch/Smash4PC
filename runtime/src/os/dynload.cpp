// coreinit OSDynLoad: run-time lookup of library functions and data by name,
// answered from the runtime's export registry.

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace cafe::os {
namespace {

constexpr uint32_t kError = 0xFFFFFFFF;

std::map<uint32_t, std::string> g_modules;           // handle -> library name
std::map<std::string, uint32_t> g_function_thunks;    // "module:name" -> guest address
std::map<std::string, uint32_t> g_data_objects;
uint32_t g_next_handle = 0x7D000000; // handles are opaque to the title

// Unimplemented library functions still get an address, so titles that
// look up more than they use keep running; calling one stops with its name.
// Titles call these pointers with bctrl, so CTR holds the address called.
void unimplemented_dynamic_export(PPCContext& ctx, uint8_t*) {
    const char* name = host_function_name(ctx.ctr);
    fatal("the game called %s (found with OSDynLoad), which the runtime does not implement yet",
          name ? name : "a library function");
}

uint32_t OSDynLoad_Acquire(uint32_t name, be<uint32_t>* handle) {
    KernelLock lock(kernel_mutex());
    std::string module(guest_string(name));
    if (module.size() > 4 && module.compare(module.size() - 4, 4, ".rpl") == 0) module.resize(module.size() - 4);
    for (const auto& [h, m] : g_modules) {
        if (m == module) {
            if (handle) *handle = h;
            return 0;
        }
    }
    const uint32_t h = g_next_handle;
    g_next_handle += 0x100;
    g_modules[h] = module;
    if (handle) *handle = h;
    return 0;
}

uint32_t OSDynLoad_FindExport(uint32_t handle, uint32_t is_data, uint32_t name, be<uint32_t>* address) {
    KernelLock lock(kernel_mutex());
    const auto module = g_modules.find(handle);
    if (module == g_modules.end()) return kError;
    const std::string symbol(guest_string(name));
    const std::string key = module->second + ":" + symbol;
    if (is_data) {
        auto it = g_data_objects.find(key);
        if (it == g_data_objects.end()) {
            const DataExport* data = find_data_export(module->second, symbol);
            if (data == nullptr) {
                std::fprintf(stderr, "ttt2: OSDynLoad: no data export %s\n", key.c_str());
                return kError;
            }
            const uint32_t storage = system_alloc(0x1000, 32);
            data->initialize(storage);
            it = g_data_objects.emplace(key, storage).first;
        }
        if (address) *address = it->second;
        return 0;
    }
    auto it = g_function_thunks.find(key);
    if (it == g_function_thunks.end()) {
        const Export* e = find_export(module->second, symbol);
        uint32_t thunk;
        if (e != nullptr) {
            thunk = register_host_function(e->function, e->name);
        } else {
            std::fprintf(stderr, "ttt2: OSDynLoad: %s is not implemented (stops if called)\n", key.c_str());
            thunk = register_host_function(unimplemented_dynamic_export, strdup(key.c_str()));
        }
        it = g_function_thunks.emplace(key, thunk).first;
    }
    if (address) *address = it->second;
    return 0;
}

void OSDynLoad_Release(uint32_t) {}

// Modules are not loaded into guest memory, so the allocator is never used.
uint32_t OSDynLoad_SetAllocator(uint32_t, uint32_t) { return 0; }

} // namespace

CAFE_EXPORT(coreinit, OSDynLoad_Acquire, OSDynLoad_Acquire);
CAFE_EXPORT(coreinit, OSDynLoad_FindExport, OSDynLoad_FindExport);
CAFE_EXPORT(coreinit, OSDynLoad_Release, OSDynLoad_Release);
CAFE_EXPORT(coreinit, OSDynLoad_SetAllocator, OSDynLoad_SetAllocator);

} // namespace cafe::os
