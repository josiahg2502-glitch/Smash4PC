#pragma once

// Oracle shim: big-endian memory over a flat test buffer that mirrors the
// guest address space layout used by the generated code under test.

#include <cstring>

#include "Common/CommonTypes.h"

namespace PowerPC {

class MMU {
public:
    explicit MMU(u8* base) : m_base(base) {}

    template <typename T>
    T Read(u32 address) {
        T value;
        std::memcpy(&value, m_base + address, sizeof(T));
        if constexpr (sizeof(T) == 2) return static_cast<T>(__builtin_bswap16(value));
        else if constexpr (sizeof(T) == 4) return static_cast<T>(__builtin_bswap32(value));
        else if constexpr (sizeof(T) == 8) return static_cast<T>(__builtin_bswap64(value));
        else return value;
    }
    template <typename T>
    void Write(T value, u32 address) {
        if constexpr (sizeof(T) == 2) value = static_cast<T>(__builtin_bswap16(value));
        else if constexpr (sizeof(T) == 4) value = static_cast<T>(__builtin_bswap32(value));
        else if constexpr (sizeof(T) == 8) value = static_cast<T>(__builtin_bswap64(value));
        std::memcpy(m_base + address, &value, sizeof(T));
    }
    // Byte-reversed accesses (lhbrx & co) go through these in Dolphin.
    void Write_U16_Swap(u16 value, u32 address) { Write<u16>(__builtin_bswap16(value), address); }
    void Write_U32_Swap(u32 value, u32 address) { Write<u32>(__builtin_bswap32(value), address); }
    void Write_U8(u8 value, u32 address) { Write<u8>(value, address); }
    void Write_U16(u16 value, u32 address) { Write<u16>(value, address); }
    void Write_U32(u32 value, u32 address) { Write<u32>(value, address); }
    void Write_U64(u64 value, u32 address) { Write<u64>(value, address); }
    u16 Read_U16_Swap(u32 address) { return __builtin_bswap16(Read<u16>(address)); }
    u32 Read_U32_Swap(u32 address) { return __builtin_bswap32(Read<u32>(address)); }

    void ClearDCacheLine(u32 address) { std::memset(m_base + (address & ~31u), 0, 32); }
    void StoreDCacheLine(u32) {}
    void InvalidateDCacheLine(u32) {}
    void FlushDCacheLine(u32) {}
    void DMA_MemoryToLC(u32, u32, u32) {}
    void DMA_LCToMemory(u32, u32, u32) {}
    void InvalidateTLBEntry(u32) {}
    void SDRUpdated() {}
    void SRUpdated() {}
    void IBATUpdated() {}
    void DBATUpdated() {}

private:
    u8* m_base;
};

} // namespace PowerPC
