#pragma once

// What Cemu's NEX client (Cemu/nex, util/crypto/md5; MPL 2.0, see LICENSE.txt)
// takes from the rest of Cemu, so its files build here nearly unchanged.
// Force-included before each of them.

#ifdef _WIN32
#define BOOST_OS_WINDOWS 1
#define BOOST_OS_UNIX 0
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#define BOOST_OS_WINDOWS 0
#define BOOST_OS_UNIX 1
#include <chrono>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using sint8 = int8_t;
using sint16 = int16_t;
using sint32 = int32_t;
using sint64 = int64_t;

enum class LogType { Force, PRUDP, Socket, Placeholder };

template <typename... Args>
void cemuLog_log(LogType type, std::format_string<Args...> format, Args&&... args) {
    if (type == LogType::PRUDP) return; // packet-level detail
    std::fprintf(stderr, "ttt2: nex: %s\n", std::format(format, std::forward<Args>(args)...).c_str());
}
inline void cemuLog_log(LogType type, std::string_view text) {
    if (type == LogType::PRUDP) return;
    std::fprintf(stderr, "ttt2: nex: %.*s\n", static_cast<int>(text.size()), text.data());
}
#define cemuLog_logDebug(...) ((void)0)
#define cemuLog_logOnce(...) cemuLog_log(__VA_ARGS__)

#define cemu_assert(cond) ((void)(cond))
#define cemu_assert_debug(cond) ((void)0)
#define cemu_assert_suspicious() ((void)0)
#define cemu_assert_unimplemented() ((void)0)
#define assert_dbg() ((void)0)

#if !BOOST_OS_WINDOWS
inline uint32 GetTickCount() {
    return static_cast<uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
#endif

// The title the friends server is told the player is in (presence); set by the runtime.
namespace CafeSystem {
uint64 GetForegroundTitleId();
uint16 GetForegroundTitleVersion();
} // namespace CafeSystem
inline uint32 GetTitleIdHigh(uint64 titleId) { return static_cast<uint32>(titleId >> 32); }

constexpr int ACT_NICKNAME_LENGTH = 11;

namespace StringHelpers {
// UTF-16 little-endian to UTF-8 (BMP only, up to the first null).
inline std::string ToUtf8FromLE(const uint16* text, size_t length) {
    std::string out;
    for (size_t i = 0; i < length && text[i]; ++i) {
        const uint32 c = text[i];
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}
} // namespace StringHelpers
