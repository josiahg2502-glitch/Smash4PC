#pragma once

// Implementing Cafe OS functions in host C++.
//
//   static uint32_t OSGetCoreId() { ... }
//   CAFE_EXPORT(coreinit, OSGetCoreId, OSGetCoreId);
//
// defines imp_coreinit_OSGetCoreId, the strong symbol replacing the
// generated "unimplemented" default, and registers it for OSDynLoad. The
// function's parameters and result are mapped onto the PowerPC EABI:
// integers and guest pointers in r3..r10 (64-bit values in an odd-aligned
// register pair), floating point in f1..f8, results in r3 (r3:r4) or f1.
// A `PPCContext&` parameter receives the caller's context and consumes no
// register; `T*` parameters are guest pointers translated to host pointers
// (null stays null); `GuestAddress` keeps the raw address.

#include "cafe/guest.h"
#include "cafe/ppc_context.h"

#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace cafe {

struct Export {
    const char* module;
    const char* name;
    PPCFunc* function;
    const Export* next;
};

// Data a library exports (e.g. coreinit's MEMAllocFromDefaultHeap pointer).
// `initialize` fills the object at `address` when the game imports it.
struct DataExport {
    const char* module;
    const char* name;
    void (*initialize)(uint32_t address);
    const DataExport* next;
};

struct ExportRegistrar {
    explicit ExportRegistrar(Export& entry);
    explicit ExportRegistrar(DataExport& entry);
};

const Export* find_export(std::string_view module, std::string_view name);
const DataExport* find_data_export(std::string_view module, std::string_view name);
const Export* first_export();

namespace abi {

struct Cursor {
    int gpr = 3;
    int fpr = 1;
};

template <typename T>
T read_arg(PPCContext& ctx, Cursor& cursor) {
    if constexpr (std::is_same_v<T, GuestAddress>) {
        return GuestAddress{ctx.r[cursor.gpr++]};
    } else if constexpr (std::is_pointer_v<T>) {
        return guest<std::remove_pointer_t<T>>(ctx.r[cursor.gpr++]);
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(ctx.f[cursor.fpr++].ps0);
    } else if constexpr (sizeof(T) == 8) {
        if ((cursor.gpr & 1) == 0) ++cursor.gpr;
        const uint64_t v = (uint64_t{ctx.r[cursor.gpr]} << 32) | ctx.r[cursor.gpr + 1];
        cursor.gpr += 2;
        return static_cast<T>(v);
    } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "unsupported argument type");
        return static_cast<T>(ctx.r[cursor.gpr++]);
    }
}

template <typename T>
void write_result(PPCContext& ctx, T value) {
    if constexpr (std::is_same_v<T, GuestAddress>) {
        ctx.r[3] = value.value;
    } else if constexpr (std::is_pointer_v<T>) {
        ctx.r[3] = guest_address(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        ctx.f[1].ps0 = static_cast<double>(value);
        if constexpr (sizeof(T) == 4) ctx.f[1].ps1 = ctx.f[1].ps0;
    } else if constexpr (sizeof(T) == 8) {
        ctx.r[3] = static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32);
        ctx.r[4] = static_cast<uint32_t>(value);
    } else if constexpr (std::is_enum_v<T>) {
        ctx.r[3] = static_cast<uint32_t>(static_cast<std::underlying_type_t<T>>(value));
    } else {
        ctx.r[3] = static_cast<uint32_t>(static_cast<std::conditional_t<std::is_signed_v<T>, int32_t, uint32_t>>(value));
    }
}

template <typename T>
T fetch(PPCContext& ctx, Cursor& cursor) {
    if constexpr (std::is_same_v<T, PPCContext&>) {
        return ctx;
    } else {
        return read_arg<T>(ctx, cursor);
    }
}

template <typename F>
struct Signature;
template <typename R, typename... A>
struct Signature<R (*)(A...)> {
    using Result = R;
    using Args = std::tuple<A...>;
};

template <auto Fn, typename R, typename... A>
void invoke_impl(PPCContext& ctx, std::tuple<A...>*) {
    Cursor cursor;
    // Braced initialisation evaluates left to right, matching register order.
    std::tuple<A...> args{fetch<A>(ctx, cursor)...};
    if constexpr (std::is_void_v<R>) {
        std::apply(Fn, std::move(args));
    } else {
        write_result(ctx, std::apply(Fn, std::move(args)));
    }
}

template <auto Fn>
void invoke(PPCContext& ctx) {
    using S = Signature<decltype(Fn)>;
    invoke_impl<Fn, typename S::Result>(ctx, static_cast<typename S::Args*>(nullptr));
}

} // namespace abi

// Calls guest code from host code: arguments in r3.., result from r3. The
// caller's LR and stack pointer are preserved.
uint32_t call_guest(PPCContext& ctx, uint32_t function, std::initializer_list<uint32_t> args);

// TTT2_COUNT_IMPORTS=1: how often the title calls each export, every 5 s (diagnostics.cpp).
extern bool g_count_imports;
void count_import(const char* name);

} // namespace cafe

#define CAFE_EXPORT(module, name, fn)                                          \
    extern "C" void imp_##module##_##name(::cafe::PPCContext& ctx, uint8_t*) { \
        if (::cafe::g_count_imports) ::cafe::count_import(#module ":" #name);  \
        ::cafe::abi::invoke<fn>(ctx);                                          \
    }                                                                          \
    static ::cafe::Export cafe_export_##module##_##name{#module, #name,        \
                                                        &imp_##module##_##name, \
                                                        nullptr};              \
    static ::cafe::ExportRegistrar cafe_registrar_##module##_##name{           \
        cafe_export_##module##_##name}

// For functions that need the raw context (variadic ones like OSReport).
#define CAFE_EXPORT_RAW(module, name)                                          \
    extern "C" void imp_##module##_##name(::cafe::PPCContext& ctx, uint8_t* base); \
    static ::cafe::Export cafe_export_##module##_##name{#module, #name,        \
                                                        &imp_##module##_##name, \
                                                        nullptr};              \
    static ::cafe::ExportRegistrar cafe_registrar_##module##_##name{           \
        cafe_export_##module##_##name};                                        \
    extern "C" void imp_##module##_##name(::cafe::PPCContext& ctx, [[maybe_unused]] uint8_t* base)

#define CAFE_DATA_EXPORT(module, name, initializer)                           \
    static ::cafe::DataExport cafe_data_export_##module##_##name{#module, #name, \
                                                                 initializer, nullptr}; \
    static ::cafe::ExportRegistrar cafe_registrar_##module##_##name{           \
        cafe_data_export_##module##_##name}
