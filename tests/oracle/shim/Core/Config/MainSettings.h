#pragma once
// Oracle shim: Dolphin configuration. Hacks stay off, as on hardware.
namespace Config {
inline constexpr int MAIN_LOW_DCBZ_HACK = 0;
template <typename T>
bool Get(T) { return false; }
} // namespace Config
