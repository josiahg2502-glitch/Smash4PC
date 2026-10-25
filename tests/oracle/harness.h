#pragma once

// Shared machinery for the oracle tests: guest address space, random machine
// states, and state comparison.

#include "oracle.h"

#include "cafe/ppc_ops.h"

#include <sys/mman.h>

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace harness {

// Guest memory visible to the tests: the low window and the top of the
// space, so (rA|0)+d addressing with rA=0 stays mapped either way. Only the
// ranges instructions can reach are randomised and compared.
inline constexpr uint32_t kLowEnd = 0x00090000;
inline constexpr uint32_t kHighBegin = 0xFFFF0000;
inline constexpr uint32_t kHighSize = 0x10010;
struct Range { uint32_t begin, end; };
inline constexpr Range kRandomised[] = {{0x00000000, 0x00008000}, {0x00020000, 0x00058000}};

inline uint8_t* reserve_space() {
    void* p = mmap(nullptr, (uint64_t{1} << 32) + 0x10000, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) std::abort();
    auto* base = static_cast<uint8_t*>(p);
    mprotect(base, kLowEnd, PROT_READ | PROT_WRITE);
    mprotect(base + kHighBegin, 0x20000, PROT_READ | PROT_WRITE);
    return base;
}

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return static_cast<uint32_t>(next()); }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

inline uint32_t random_gpr(Rng& rng) {
    switch (rng.below(8)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 0xFFFFFFFFu;
    case 3: return 0x80000000u;
    case 4: return 0x7FFFFFFFu;
    case 5: return rng.u32() & 0xFF;
    default: return rng.u32();
    }
}

inline uint64_t random_fpr(Rng& rng) {
    static const double specials[] = {
        0.0, -0.0, INFINITY, -INFINITY, NAN, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5,
        DBL_MIN, DBL_TRUE_MIN, FLT_MIN, FLT_TRUE_MIN, FLT_MAX, -FLT_MAX, DBL_MAX,
        2147483648.0, -2147483648.0, 2147483647.5, 4294967296.0, 1e-40, 3.0e38,
    };
    switch (rng.below(10)) {
    case 0: return rng.next(); // any bit pattern, NaN payloads included
    case 1: return std::bit_cast<uint64_t>(cafe::ppc::load_single(rng.u32()));
    case 2: return 0x7FF4000000000000ull | (rng.next() & 0xFFFFFFFFFull); // signalling NaN
    case 3: return std::bit_cast<uint64_t>(specials[rng.below(std::size(specials))]);
    case 4: case 5: case 6: {
        // Typical game values: single-precision numbers of moderate size.
        const float f = std::ldexp(static_cast<float>(rng.below(1u << 24)) / (1u << 24),
                                   static_cast<int>(rng.below(48)) - 24) *
                        (rng.below(2) ? -1.0f : 1.0f);
        return std::bit_cast<uint64_t>(static_cast<double>(f));
    }
    default: {
        const double d = std::ldexp(static_cast<double>(rng.next() >> 11) / 9007199254740992.0,
                                    static_cast<int>(rng.below(80)) - 40) *
                         (rng.below(2) ? -1.0 : 1.0);
        return std::bit_cast<uint64_t>(d);
    }
    }
}

inline uint32_t random_gqr(Rng& rng) {
    static const uint32_t types[] = {0, 4, 5, 6, 7};
    return types[rng.below(5)] | (rng.below(64) << 8) | (types[rng.below(5)] << 16) |
           (rng.below(64) << 24);
}

inline void to_oracle(const cafe::PPCContext& c, OracleState& o) {
    for (int i = 0; i < 32; ++i) {
        o.gpr[i] = c.r[i];
        o.ps0[i] = std::bit_cast<uint64_t>(c.f[i].ps0);
        o.ps1[i] = std::bit_cast<uint64_t>(c.f[i].ps1);
    }
    for (int i = 0; i < 8; ++i) {
        const cafe::CRField& f = c.cr[i];
        o.cr[i] = static_cast<uint8_t>((f.lt << 3) | (f.gt << 2) | (f.eq << 1) | f.so);
        o.gqr[i] = c.gqr[i];
    }
    o.so = c.xer_so; o.ov = c.xer_ov; o.ca = c.xer_ca; o.bc = c.xer_bc;
    o.lr = c.lr; o.ctr = c.ctr;
}

inline std::string compare(const OracleState& expected, const OracleState& actual) {
    std::string diff;
    char line[160];
    for (int i = 0; i < 32; ++i) {
        if (expected.gpr[i] != actual.gpr[i]) {
            std::snprintf(line, sizeof line, " r%d want %08X got %08X;", i, expected.gpr[i], actual.gpr[i]);
            diff += line;
        }
        if (expected.ps0[i] != actual.ps0[i]) {
            std::snprintf(line, sizeof line, " f%d.ps0 want %016llX got %016llX;", i,
                          (unsigned long long)expected.ps0[i], (unsigned long long)actual.ps0[i]);
            diff += line;
        }
        if (expected.ps1[i] != actual.ps1[i]) {
            std::snprintf(line, sizeof line, " f%d.ps1 want %016llX got %016llX;", i,
                          (unsigned long long)expected.ps1[i], (unsigned long long)actual.ps1[i]);
            diff += line;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (expected.cr[i] != actual.cr[i]) {
            std::snprintf(line, sizeof line, " cr%d want %X got %X;", i, expected.cr[i], actual.cr[i]);
            diff += line;
        }
        if (expected.gqr[i] != actual.gqr[i]) {
            std::snprintf(line, sizeof line, " gqr%d want %08X got %08X;", i, expected.gqr[i], actual.gqr[i]);
            diff += line;
        }
    }
    const auto field = [&](const char* name, uint32_t want, uint32_t got) {
        if (want != got) {
            std::snprintf(line, sizeof line, " %s want %X got %X;", name, want, got);
            diff += line;
        }
    };
    field("xer.so", expected.so, actual.so);
    field("xer.ov", expected.ov, actual.ov);
    field("xer.ca", expected.ca, actual.ca);
    field("xer.bc", expected.bc, actual.bc);
    field("lr", expected.lr, actual.lr);
    field("ctr", expected.ctr, actual.ctr);
    return diff;
}


// A random but valid machine state.
inline void randomise(cafe::PPCContext& ctx, Rng& rng) {
    for (auto& r : ctx.r) r = random_gpr(rng);
    for (auto& f : ctx.f) {
        f.ps0 = std::bit_cast<double>(random_fpr(rng));
        f.ps1 = std::bit_cast<double>(random_fpr(rng));
    }
    for (auto& c : ctx.cr) {
        const uint32_t v = rng.below(16);
        c = {static_cast<uint8_t>(v >> 3), static_cast<uint8_t>((v >> 2) & 1),
             static_cast<uint8_t>((v >> 1) & 1), static_cast<uint8_t>(v & 1)};
    }
    for (auto& g : ctx.gqr) g = random_gqr(rng);
    ctx.xer_so = rng.below(2);
    ctx.xer_ov = rng.below(2);
    ctx.xer_ca = rng.below(2);
    ctx.xer_bc = static_cast<uint8_t>(rng.below(0x80));
    ctx.lr = rng.u32();
    ctx.ctr = rng.u32();
}

// Fills the randomised ranges identically in both address spaces and clears
// the top page range.
inline void randomise_memory(uint8_t* ours, uint8_t* theirs, Rng& rng) {
    for (const Range& range : kRandomised) {
        for (uint32_t a = range.begin; a < range.end; a += 8) {
            const uint64_t v = rng.next();
            std::memcpy(ours + a, &v, 8);
        }
        std::memcpy(theirs + range.begin, ours + range.begin, range.end - range.begin);
    }
    std::memset(ours + kHighBegin, 0, kHighSize);
    std::memset(theirs + kHighBegin, 0, kHighSize);
}

inline bool memory_matches(const uint8_t* ours, const uint8_t* theirs) {
    bool same = std::memcmp(ours + kHighBegin, theirs + kHighBegin, kHighSize) == 0;
    for (const Range& range : kRandomised) {
        same = same && std::memcmp(ours + range.begin, theirs + range.begin,
                                   range.end - range.begin) == 0;
    }
    return same;
}

} // namespace harness
