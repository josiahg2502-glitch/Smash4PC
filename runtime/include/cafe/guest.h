#pragma once

// Types for guest data seen from host code: guest memory is big-endian and
// addressed by 32-bit guest addresses.

#include "cafe/runtime.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace cafe {

template <typename T>
constexpr T byteswap_value(T value) {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        return static_cast<T>(byteswap_value(static_cast<U>(value)));
    } else if constexpr (std::is_floating_point_v<T>) {
        using U = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
        return std::bit_cast<T>(byteswap_value(std::bit_cast<U>(value)));
    } else {
        using U = std::make_unsigned_t<T>;
        if constexpr (sizeof(T) == 2) return static_cast<T>(__builtin_bswap16(static_cast<U>(value)));
        else if constexpr (sizeof(T) == 4) return static_cast<T>(__builtin_bswap32(static_cast<U>(value)));
        else return static_cast<T>(__builtin_bswap64(static_cast<U>(value)));
    }
}

// A big-endian value as it sits in guest memory. Same size and alignment as
// T, so guest structures can be declared field by field.
template <typename T>
struct be {
    T stored;

    be() = default;
    be(T value) : stored(byteswap_value(value)) {}
    T value() const { return byteswap_value(stored); }
    operator T() const { return value(); }
    be& operator=(T value) {
        stored = byteswap_value(value);
        return *this;
    }
    be& operator+=(T v) { return *this = static_cast<T>(value() + v); }
    be& operator-=(T v) { return *this = static_cast<T>(value() - v); }
    be& operator|=(T v) { return *this = static_cast<T>(value() | v); }
    be& operator&=(T v) { return *this = static_cast<T>(value() & v); }
};
static_assert(sizeof(be<uint32_t>) == 4 && alignof(be<uint64_t>) == 8);

template <typename T>
T* guest(uint32_t address) {
    return address == 0 ? nullptr : reinterpret_cast<T*>(guest_base() + address);
}

inline uint32_t guest_address(const void* host) {
    if (host == nullptr) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(host) - guest_base());
}

// A guest pointer field: 32-bit big-endian address.
template <typename T>
struct gptr {
    be<uint32_t> address;

    gptr() = default;
    gptr(T* host) : address(guest_address(host)) {}
    T* get() const { return guest<T>(address); }
    T* operator->() const { return get(); }
    explicit operator bool() const { return address != 0u; }
    gptr& operator=(T* host) {
        address = guest_address(host);
        return *this;
    }
};
static_assert(sizeof(gptr<int>) == 4);

// A guest address passed by value: what a guest pointer argument is before
// the callee decides how to look at it.
struct GuestAddress {
    uint32_t value;
    template <typename T>
    T* as() const { return guest<T>(value); }
};

inline std::string_view guest_string(uint32_t address) {
    if (address == 0) return {};
    const char* text = guest<const char>(address);
    return {text, std::strlen(text)};
}

} // namespace cafe
