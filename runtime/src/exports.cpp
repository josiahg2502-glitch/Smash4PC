#include "cafe/export.h"
#include "cafe/ppc_ops.h"

#include <cstring>

namespace cafe {
namespace {

const Export* g_exports = nullptr;
const DataExport* g_data_exports = nullptr;

} // namespace

ExportRegistrar::ExportRegistrar(Export& entry) {
    entry.next = g_exports;
    g_exports = &entry;
}

ExportRegistrar::ExportRegistrar(DataExport& entry) {
    entry.next = g_data_exports;
    g_data_exports = &entry;
}

const Export* first_export() { return g_exports; }

// Module names arrive with or without ".rpl" (OSDynLoad callers vary).
static bool same_module(std::string_view registered, std::string_view wanted) {
    if (wanted.size() > 4 && wanted.substr(wanted.size() - 4) == ".rpl") {
        wanted.remove_suffix(4);
    }
    return registered == wanted;
}

const Export* find_export(std::string_view module, std::string_view name) {
    for (const Export* e = g_exports; e != nullptr; e = e->next) {
        if (e->name == name && same_module(e->module, module)) return e;
    }
    return nullptr;
}

const DataExport* find_data_export(std::string_view module, std::string_view name) {
    for (const DataExport* e = g_data_exports; e != nullptr; e = e->next) {
        if (e->name == name && same_module(e->module, module)) return e;
    }
    return nullptr;
}

uint32_t call_guest(PPCContext& ctx, uint32_t function, std::initializer_list<uint32_t> args) {
    const uint32_t saved_lr = ctx.lr;
    const uint32_t saved_sp = ctx.r[1];
    // A fresh minimal frame, so the callee's LR save lands in memory we own.
    const uint32_t frame = (saved_sp - 0x20) & ~0xFu;
    ppc::st32(guest_base(), frame, saved_sp);
    ctx.r[1] = frame;
    int reg = 3;
    for (const uint32_t arg : args) {
        ctx.r[reg++] = arg;
    }
    ctx.lr = 0;
    cafe_ppc_lookup(function)(ctx, guest_base());
    const uint32_t result = ctx.r[3];
    ctx.r[1] = saved_sp;
    ctx.lr = saved_lr;
    return result;
}

} // namespace cafe
