#include "cafe/generated.h"
#include "cafe/layout.h"
#include "cafe/runtime.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace cafe {
namespace {

// Open-addressing map from guest code address to native function, sized for
// a load factor under one half. Indirect calls are hot (the game makes ~470k
// static virtual calls), so a lookup is one multiply and usually one probe.
struct Slot {
    uint32_t address;
    PPCFunc* function;
    bool compiled;
};

std::vector<Slot> g_slots;
uint64_t g_mask = 0;
std::mutex g_host_mutex;
uint32_t g_next_host_thunk = layout::kHostThunkBase;
std::vector<std::string> g_host_names;

size_t home(uint32_t address) {
    return static_cast<size_t>(((address >> 2) * 0x9E3779B97F4A7C15ull) >> 40) & g_mask;
}

void insert(uint32_t address, PPCFunc* function, bool compiled) {
    for (size_t i = home(address);; i = (i + 1) & g_mask) {
        Slot& slot = g_slots[i];
        if (slot.address == 0 || slot.address == address) {
            slot = {address, function, compiled};
            return;
        }
    }
}

} // namespace

void build_dispatch_table() {
    const size_t wanted = (cafe_guest_function_count + cafe_guest_import_count + 0x10000) * 2;
    size_t capacity = 1;
    while (capacity < wanted) {
        capacity <<= 1;
    }
    g_slots.assign(capacity, Slot{0, nullptr, false});
    g_mask = capacity - 1;
    for (size_t i = 0; i < cafe_guest_function_count; ++i) {
        const GuestFunction& f = cafe_guest_functions[i];
        insert(f.address, f.function, f.function != nullptr);
    }
    // Code may take the address of an OS function (lis/addi on its stub).
    for (size_t i = 0; i < cafe_guest_import_count; ++i) {
        const GuestImport& import = cafe_guest_imports[i];
        if (!import.is_data) {
            insert(import.stub_address, import.function, true);
        }
    }
}

uint32_t register_host_function(PPCFunc* function, const char* name) {
    std::lock_guard lock(g_host_mutex);
    if (g_next_host_thunk >= layout::kHostThunkLimit) {
        fatal("out of host function addresses");
    }
    const uint32_t address = g_next_host_thunk;
    g_next_host_thunk += 4;
    g_host_names.emplace_back(name);
    insert(address, function, true);
    return address;
}

const char* host_function_name(uint32_t address) {
    std::lock_guard lock(g_host_mutex);
    if (address < layout::kHostThunkBase || address >= g_next_host_thunk || (address & 3) != 0) return nullptr;
    return g_host_names[(address - layout::kHostThunkBase) / 4].c_str();
}

} // namespace cafe

extern "C" cafe::PPCFunc* cafe_ppc_lookup(uint32_t address) {
    using namespace cafe;
    for (size_t i = home(address);; i = (i + 1) & g_mask) {
        const Slot& slot = g_slots[i];
        if (slot.address == address) {
            if (!slot.compiled) {
                fatal("guest function 0x%08X was called indirectly but was not "
                      "compiled: static analysis judged it unreachable. Regenerate "
                      "with --all and report this address.",
                      address);
            }
            return slot.function;
        }
        if (slot.address == 0) {
            fatal("indirect call to 0x%08X, which is not the start of any guest "
                  "function",
                  address);
        }
    }
}
