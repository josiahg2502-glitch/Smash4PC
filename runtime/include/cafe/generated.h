#pragma once

// Tables the recompiler emits alongside the translated functions.

#include "cafe/ppc_context.h"

#include <cstddef>
#include <cstdint>

namespace cafe {

struct GuestFunction {
    uint32_t address;
    PPCFunc* function; // null when the entry was judged unreachable
};

struct GuestImport {
    const char* module;
    const char* name;
    uint32_t stub_address; // the guest-visible address code calls or takes
    uint32_t address;      // data imports: where the object must live
    PPCFunc* function;     // null for data imports
    bool is_data;
};

struct ProgramInfo {
    const char* rpx_sha256;
    uint32_t entry_point;
    uint32_t sda_base;
    uint32_t sda2_base;
    uint32_t stack_size;
    uint32_t heap_size;
    size_t compiled_entries;
    size_t total_entries;
};

} // namespace cafe

extern "C" {
extern const cafe::GuestFunction cafe_guest_functions[];
extern const size_t cafe_guest_function_count;
extern const cafe::GuestImport cafe_guest_imports[];
extern const size_t cafe_guest_import_count;
extern const cafe::ProgramInfo cafe_program_info;
// Returns only for the closed services it answers itself (diagnostics.cpp).
void cafe_unimplemented_import(cafe::PPCContext& ctx,
                               const char* module,
                               const char* name);
}
