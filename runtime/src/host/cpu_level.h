#pragma once
// Whether this processor runs code built for x86-64-v3 (AVX2, FMA, BMI1/2, LZCNT, MOVBE, with the OS saving
// the AVX registers). smash.exe's translated code needs it; smash-compat.exe is built for x86-64-v2.

namespace cafe::host {

inline bool cpu_has_x86_64_v3() { return __builtin_cpu_supports("x86-64-v3"); }

} // namespace cafe::host
