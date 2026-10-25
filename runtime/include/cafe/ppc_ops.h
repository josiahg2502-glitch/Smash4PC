#pragma once

// Semantics used by recompiled code. Everything here is behaviour of the
// Espresso as measured on hardware, expressed as host C++ the compiler can
// optimise; tests/ checks it bit for bit against a reference interpreter.
// Assumes FPSCR[RN]=nearest and NI=0: the census found no mffs, mtfsf,
// mtfsfi or mtfsb* in the game, so it never changes them.

#include "cafe/ppc_context.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// ---------------------------------------------------------------------------
// Hooks the runtime implements.
extern "C" {
::cafe::PPCFunc* cafe_ppc_lookup(uint32_t guest_address);
[[noreturn]] void cafe_ppc_trap(::cafe::PPCContext& ctx, uint32_t address);
[[noreturn]] void cafe_ppc_illegal(::cafe::PPCContext& ctx, uint32_t address,
                                   uint32_t word, const char* reason);
[[noreturn]] void cafe_ppc_null_call(::cafe::PPCContext& ctx, uint32_t address);
[[noreturn]] void cafe_ppc_bad_return(::cafe::PPCContext& ctx, uint32_t expected);
uint64_t cafe_ppc_timebase(void);
}

// ---------------------------------------------------------------------------
// Function definition and call conventions.
#define PPC_FUNC(name)                                                         \
    extern "C" void name(::cafe::PPCContext& __restrict ctx,                   \
                         uint8_t* __restrict base)

// Generated bodies are `name_orig`; `name` is a weak alias of it, and every
// call goes through `name`. Define `name` yourself (PPC_FUNC(sub_...) { ... })
// to replace a guest function; call `name_orig` to reuse the original.
#define PPC_WEAK_ALIAS(name)                                                   \
    extern "C" void name(::cafe::PPCContext& __restrict ctx,                   \
                         uint8_t* __restrict base)                             \
        __attribute__((weak, alias(#name "_orig")))

#if defined(__clang__)
#define PPC_MUSTTAIL [[clang::musttail]]
#elif defined(__GNUC__) && __GNUC__ >= 15
#define PPC_MUSTTAIL [[gnu::musttail]]
#else
#define PPC_MUSTTAIL
#endif

// Guest tail calls must not grow the host stack: recursion through tail calls
// is unbounded in guest code.
#define PPC_TAIL_CALL(fn) PPC_MUSTTAIL return fn(ctx, base)
#define PPC_CALL_INDIRECT(target) cafe_ppc_lookup(target)(ctx, base)
// A guest call becomes a host call, so execution resumes after the call site
// when the callee returns. That matches the guest only if the callee's blr
// went to the address the call put in LR; anything else (longjmp-style
// returns) must stop loudly rather than continue in the wrong place.
#ifndef PPC_CHECK_RETURNS
#define PPC_CHECK_RETURNS 1
#endif
#if PPC_CHECK_RETURNS
#define PPC_CHECK_RETURN(expected)                                             \
    do {                                                                       \
        if (__builtin_expect(ctx.lr != (expected), 0))                         \
            cafe_ppc_bad_return(ctx, (expected));                              \
    } while (0)
#else
#define PPC_CHECK_RETURN(expected) ((void)0)
#endif

#define PPC_TAIL_CALL_INDIRECT(target)                                         \
    do {                                                                       \
        ::cafe::PPCFunc* const ppc_fn_ = cafe_ppc_lookup(target);              \
        PPC_MUSTTAIL return ppc_fn_(ctx, base);                                \
    } while (0)

namespace cafe::ppc {

// ---------------------------------------------------------------------------
// Guest memory: `base` maps the 32-bit guest space; data is big-endian.
//
// Guest memory is shared between guest threads (and with the runtime and the
// GPU), so every access is volatile: the compiler performs each load and
// store the guest program performs, in program order, and cannot cache
// memory in registers. Without this a guest loop polling a flag that
// another thread sets compiles to a single read. x86 performs the unaligned
// accesses natively.
namespace mem {
using u16 = uint16_t __attribute__((aligned(1), may_alias));
using u32 = uint32_t __attribute__((aligned(1), may_alias));
using u64 = uint64_t __attribute__((aligned(1), may_alias));
template <typename T>
inline T load(const uint8_t* p) { return *reinterpret_cast<const volatile T*>(p); }
template <typename T, typename V>
inline void store(uint8_t* p, V v) { *reinterpret_cast<volatile T*>(p) = v; }
} // namespace mem

inline uint8_t ld8(const uint8_t* base, uint32_t ea) { return mem::load<uint8_t>(base + ea); }
inline uint16_t ld16(const uint8_t* base, uint32_t ea) { return __builtin_bswap16(mem::load<mem::u16>(base + ea)); }
inline uint32_t ld32(const uint8_t* base, uint32_t ea) { return __builtin_bswap32(mem::load<mem::u32>(base + ea)); }
inline uint64_t ld64(const uint8_t* base, uint32_t ea) { return __builtin_bswap64(mem::load<mem::u64>(base + ea)); }
inline void st8(uint8_t* base, uint32_t ea, uint8_t v) { mem::store<uint8_t>(base + ea, v); }
inline void st16(uint8_t* base, uint32_t ea, uint16_t v) { mem::store<mem::u16>(base + ea, __builtin_bswap16(v)); }
inline void st32(uint8_t* base, uint32_t ea, uint32_t v) { mem::store<mem::u32>(base + ea, __builtin_bswap32(v)); }
inline void st64(uint8_t* base, uint32_t ea, uint64_t v) { mem::store<mem::u64>(base + ea, __builtin_bswap64(v)); }
// Byte-reversed forms (lhbrx/lwbrx/...) read the guest bytes little-endian.
inline uint16_t ld16_le(const uint8_t* base, uint32_t ea) { return mem::load<mem::u16>(base + ea); }
inline uint32_t ld32_le(const uint8_t* base, uint32_t ea) { return mem::load<mem::u32>(base + ea); }
inline void st16_le(uint8_t* base, uint32_t ea, uint16_t v) { mem::store<mem::u16>(base + ea, v); }
inline void st32_le(uint8_t* base, uint32_t ea, uint32_t v) { mem::store<mem::u32>(base + ea, v); }

inline void dcbz(uint8_t* base, uint32_t ea) {
    uint8_t* line = base + (ea & ~31u);
    for (int i = 0; i < 32; i += 8) mem::store<mem::u64>(line + i, uint64_t{0});
}

inline void lmw(PPCContext& ctx, const uint8_t* base, uint32_t rd, uint32_t ea) {
    for (uint32_t r = rd; r < 32; ++r, ea += 4) {
        ctx.r[r] = ld32(base, ea);
    }
}
inline void stmw(const PPCContext& ctx, uint8_t* base, uint32_t rs, uint32_t ea) {
    for (uint32_t r = rs; r < 32; ++r, ea += 4) {
        st32(base, ea, ctx.r[r]);
    }
}
// lswi/stswi: n bytes moved through consecutive registers, wrapping r31->r0.
inline void lsw(PPCContext& ctx, const uint8_t* base, uint32_t rd, uint32_t ea,
                uint32_t n) {
    uint32_t r = rd - 1;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t shift = 24 - 8 * (i & 3);
        if ((i & 3) == 0) {
            r = (r + 1) & 31;
            ctx.r[r] = 0;
        }
        ctx.r[r] |= uint32_t{ld8(base, ea + i)} << shift;
    }
}
inline void stsw(const PPCContext& ctx, uint8_t* base, uint32_t rs, uint32_t ea,
                 uint32_t n) {
    uint32_t r = rs - 1;
    for (uint32_t i = 0; i < n; ++i) {
        if ((i & 3) == 0) {
            r = (r + 1) & 31;
        }
        st8(base, ea + i, static_cast<uint8_t>(ctx.r[r] >> (24 - 8 * (i & 3))));
    }
}

// lwarx/stwcx. as a compare-and-swap on the observed word.
inline uint32_t lwarx(PPCContext& ctx, const uint8_t* base, uint32_t ea) {
    uint32_t raw;
    __atomic_load(reinterpret_cast<const uint32_t*>(base + ea), &raw,
                  __ATOMIC_SEQ_CST);
    ctx.reserve_address = ea;
    ctx.reserve_value = raw;
    ctx.reserve_valid = 1;
    return __builtin_bswap32(raw);
}
inline bool stwcx(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    const bool held = ctx.reserve_valid && ctx.reserve_address == ea;
    ctx.reserve_valid = 0;
    if (!held) {
        return false;
    }
    uint32_t expected = ctx.reserve_value;
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(base + ea),
                                       &expected, __builtin_bswap32(value),
                                       false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

// ---------------------------------------------------------------------------
// Integer helpers.
inline uint32_t rotl(uint32_t v, uint32_t n) { return std::rotl(v, static_cast<int>(n & 31)); }

inline void cmp_signed(CRField& f, int32_t a, int32_t b, uint8_t so) {
    f.lt = a < b;
    f.gt = a > b;
    f.eq = a == b;
    f.so = so;
}
inline void cmp_unsigned(CRField& f, uint32_t a, uint32_t b, uint8_t so) {
    f.lt = a < b;
    f.gt = a > b;
    f.eq = a == b;
    f.so = so;
}

inline uint32_t divw(uint32_t a, uint32_t b) {
    const auto sa = static_cast<int32_t>(a);
    const auto sb = static_cast<int32_t>(b);
    if (sb == 0 || (a == 0x80000000u && sb == -1)) {
        return sa < 0 ? 0xFFFFFFFFu : 0u;
    }
    return static_cast<uint32_t>(sa / sb);
}
inline uint32_t divwu(uint32_t a, uint32_t b) { return b == 0 ? 0 : a / b; }
inline bool divw_overflows(uint32_t a, uint32_t b) {
    return b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu);
}

// sraw/srawi: CA is set when a negative value loses one-bits.
inline uint32_t sraw(uint8_t& ca, uint32_t value, uint32_t shift) {
    const auto s = static_cast<int32_t>(value);
    if (shift & 0x20) {
        ca = s < 0;
        return static_cast<uint32_t>(s >> 31);
    }
    shift &= 31;
    ca = s < 0 && shift != 0 && (value & ((1u << shift) - 1)) != 0;
    return static_cast<uint32_t>(s >> shift);
}

inline uint32_t mfcr(const PPCContext& ctx) {
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        const CRField& f = ctx.cr[i];
        v |= ((uint32_t{f.lt} << 3) | (uint32_t{f.gt} << 2) |
              (uint32_t{f.eq} << 1) | f.so)
             << (28 - 4 * i);
    }
    return v;
}
inline void mtcrf(PPCContext& ctx, uint32_t mask, uint32_t v) {
    for (int i = 0; i < 8; ++i) {
        if (mask & (0x80u >> i)) {
            const uint32_t n = v >> (28 - 4 * i);
            ctx.cr[i] = {static_cast<uint8_t>((n >> 3) & 1),
                         static_cast<uint8_t>((n >> 2) & 1),
                         static_cast<uint8_t>((n >> 1) & 1),
                         static_cast<uint8_t>(n & 1)};
        }
    }
}
inline uint32_t mfxer(const PPCContext& ctx) {
    return (uint32_t{ctx.xer_so} << 31) | (uint32_t{ctx.xer_ov} << 30) |
           (uint32_t{ctx.xer_ca} << 29) | ctx.xer_bc;
}
inline void mtxer(PPCContext& ctx, uint32_t v) {
    ctx.xer_so = (v >> 31) & 1;
    ctx.xer_ov = (v >> 30) & 1;
    ctx.xer_ca = (v >> 29) & 1;
    ctx.xer_bc = v & 0x7F;
}

// ---------------------------------------------------------------------------
// Floating point.
inline uint64_t bits(double d) { return std::bit_cast<uint64_t>(d); }
inline double from_bits(uint64_t v) { return std::bit_cast<double>(v); }
constexpr uint64_t kSign = 0x8000000000000000ull;
constexpr uint64_t kExp = 0x7FF0000000000000ull;
constexpr uint64_t kFrac = 0x000FFFFFFFFFFFFFull;
// The PowerPC default NaN is positive; x86's is negative.
inline double default_nan() { return from_bits(0x7FF8000000000000ull); }
inline double quiet(double d) { return from_bits(bits(d) | 0x0008000000000000ull); }

[[gnu::cold]] inline double nan_result(double a, double b) {
    if (std::isnan(a)) return quiet(a);
    if (std::isnan(b)) return quiet(b);
    return default_nan();
}
[[gnu::cold]] inline double nan_result(double a, double b, double c) {
    if (std::isnan(a)) return quiet(a);
    if (std::isnan(b)) return quiet(b);
    if (std::isnan(c)) return quiet(c);
    return default_nan();
}

inline double round_single(double d) {
    return static_cast<double>(static_cast<float>(d));
}

// Single-precision multiplies use frC with its significand rounded to 25
// bits, a half unit rounding up. A denormal double's 25 bits start at its
// leading one; one with fewer significant bits is used as it is.
inline double round_25bit(double d) {
    const uint64_t v = bits(d);
    const uint64_t frac = v & kFrac;
    // The leading significand bit: the implicit one (bit 52) of normal
    // numbers, zeros, infinities and NaNs, else a denormal's highest one.
    const int lead = ((v & kExp) == 0 && frac != 0) ? 63 - std::countl_zero(frac) : 52;
    if (lead < 25) return d;
    const uint64_t unit = uint64_t{1} << (lead - 24); // the last bit kept
    const uint64_t kept = v & ~(unit - 1);
    return from_bits((v & (unit >> 1)) ? kept + unit : kept);
}

inline double add(double a, double b) {
    const double r = a + b;
    return r == r ? r : nan_result(a, b);
}
inline double sub(double a, double b) {
    const double r = a - b;
    return r == r ? r : nan_result(a, b);
}
inline double mul(double a, double c) {
    const double r = a * c;
    return r == r ? r : nan_result(a, c);
}
inline double div(double a, double b) {
    const double r = a / b;
    return r == r ? r : nan_result(a, b);
}

// fmadd family: a*c + b (or - b), fused, NaN priority a, b, c with the
// un-negated b. The single-precision form rounds frC to 25 bits and corrects
// the one case where rounding the fused double result to single would
// double-round: a result exactly halfway between two singles that is not
// exact.
template <bool single, bool subtract>
inline double madd(double a, double c, double b) {
    const double addend = subtract ? -b : b;
    double r;
    if constexpr (!single) {
        r = std::fma(a, c, addend);
    } else {
        const double cr = round_25bit(c);
        r = std::fma(a, cr, addend);
        const uint64_t rb = bits(r);
        if ((rb & 0x1FFFFFFFull) == 0x10000000ull) {
            const double a1 = addend - r;
            const double b1 = r + a1;
            const double err = std::fma(a, cr, a1) + (addend - b1);
            if (err != 0.0) {
                r = from_bits((err > 0.0) == (r > 0.0) ? rb + 1 : rb - 1);
            }
        }
    }
    return r == r ? r : nan_result(a, b, c);
}
inline double negate_unless_nan(double d) { return d == d ? -d : d; }

inline double fsel(double a, double b, double c) { return a >= -0.0 ? c : b; }

inline void fcmp(CRField& f, double a, double b) {
    f.lt = a < b;
    f.gt = a > b;
    f.eq = a == b;
    f.so = std::isnan(a) || std::isnan(b);
}

// fctiw/fctiwz: result in the low word, 0xFFF8 in the high half.
inline double fcti(double b, bool toward_zero) {
    double rounded;
    if (toward_zero) {
        rounded = std::trunc(b);
    } else {
        const double big = std::copysign(4503599627370496.0, b);
        rounded = (b + big) - big;
    }
    uint32_t value;
    if (std::isnan(b)) {
        value = 0x80000000u;
    } else if (rounded >= 2147483648.0) {
        value = 0x7FFFFFFFu;
    } else if (rounded < -2147483648.0) {
        value = 0x80000000u;
    } else {
        value = static_cast<uint32_t>(static_cast<int32_t>(rounded));
    }
    uint64_t result = 0xFFF8000000000000ull | value;
    if (value == 0 && std::signbit(b)) {
        result |= 0x100000000ull;
    }
    return from_bits(result);
}

// Bit-exact single <-> double conversions used by FP loads and stores.
[[gnu::cold]] inline uint64_t single_nan_to_double(uint32_t x) {
    const uint64_t y = (x >> 30) & 1;
    const uint64_t z = (y << 61) | (y << 60) | (y << 59);
    return (uint64_t{x & 0xC0000000u} << 32) | z |
           (uint64_t{x & 0x3FFFFFFFu} << 29);
}
// lfs keeps signalling NaNs signalling, which a host conversion would not.
inline double load_single(uint32_t x) {
    if ((x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu) != 0) {
        return from_bits(single_nan_to_double(x));
    }
    return static_cast<double>(std::bit_cast<float>(x));
}
// stfs converts without rounding, as the PowerPC architecture defines it
// (Programming Environments Manual, "Floating-Point Store Instructions"): a
// double whose biased exponent is 874..896, too small for a normal single,
// is denormalised by shifting its significand right; any other is stored as
// its sign and top exponent bit followed by bits 5..34.
inline uint32_t store_single(double d) {
    const uint64_t frs = bits(d);
    const uint32_t exponent = static_cast<uint32_t>(frs >> 52) & 0x7FF;
    if (exponent >= 874 && exponent <= 896) {
        const uint64_t significand = (frs & kFrac) | (uint64_t{1} << 52);
        // In units of the single's smallest denormal, 2^-149.
        return (static_cast<uint32_t>(frs >> 32) & 0x80000000u) |
               static_cast<uint32_t>(significand >> (926 - exponent));
    }
    return static_cast<uint32_t>(((frs >> 32) & 0xC0000000u) | ((frs >> 29) & 0x3FFFFFFFu));
}
// Paired-single stores give a signed zero for what would be a single denormal.
inline uint32_t store_single_ftz(double d) {
    const uint64_t frs = bits(d);
    const uint32_t exponent = static_cast<uint32_t>(frs >> 52) & 0x7FF;
    if (exponent <= 896 && (frs & ~kSign) != 0) return static_cast<uint32_t>(frs >> 32) & 0x80000000u;
    return static_cast<uint32_t>(((frs >> 32) & 0xC0000000u) | ((frs >> 29) & 0x3FFFFFFFu));
}

// fres and frsqrte read the processor's estimate tables: the leading bits
// of the significand (for frsqrte also the exponent's parity) choose one of
// 32 segments, each a line given by its value at the segment's start and
// its slope, and the bits after them the position on that line. The
// result's exponent is exact. The segments are hardware constants.
struct EstimateSegment {
    uint32_t start, slope;
};
// frsqrte: the first 16 segments for an even biased exponent, the last 16
// for an odd one; 11 bits of position.
inline constexpr std::array<EstimateSegment, 32> kRsqrteSegments{{
    {0x1a7e800, 0x568}, {0x17cb800, 0x4f3}, {0x1552800, 0x48d}, {0x130c000, 0x435},
    {0x10f2000, 0x3e7}, {0x0eff000, 0x3a2}, {0x0d2e000, 0x365}, {0x0b7c000, 0x32e},
    {0x09e5000, 0x2fc}, {0x0867000, 0x2d0}, {0x06ff000, 0x2a8}, {0x05ab800, 0x283},
    {0x046a000, 0x261}, {0x0339800, 0x243}, {0x0218800, 0x226}, {0x0105800, 0x20b},
    {0x3ffa000, 0x7a4}, {0x3c29000, 0x700}, {0x38aa000, 0x670}, {0x3572000, 0x5f2},
    {0x3279000, 0x584}, {0x2fb7000, 0x524}, {0x2d26000, 0x4cc}, {0x2ac0000, 0x47e},
    {0x2881000, 0x43a}, {0x2665000, 0x3fa}, {0x2468000, 0x3c2}, {0x2287000, 0x38e},
    {0x20c1000, 0x35e}, {0x1f12000, 0x332}, {0x1d79000, 0x30a}, {0x1bf4000, 0x2e6},
}};
// fres: 10 bits of position, the slope applied at half its value (rounded).
inline constexpr std::array<EstimateSegment, 32> kResSegments{{
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
    {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106},
}};

// frsqrte: +0 and -0 give infinities of their sign, +infinity +0, NaNs
// their quiet form, anything else negative the default NaN. Denormals are
// normalised first, their exponent going below 1.
inline double rsqrte(double x) {
    const uint64_t v = bits(x);
    const bool negative = (v & kSign) != 0;
    int64_t exponent = static_cast<int64_t>((v & kExp) >> 52);
    uint64_t frac = v & kFrac;
    if (exponent == 0x7FF) return frac != 0 ? quiet(x) : (negative ? default_nan() : 0.0);
    if (exponent == 0 && frac == 0) return from_bits(v | kExp); // +-infinity
    if (negative) return default_nan();
    if (exponent == 0) {
        const int shift = std::countl_zero(frac) - 11; // the leading one to bit 52
        frac = (frac << shift) & kFrac;
        exponent = 1 - shift;
    }
    const uint32_t index = static_cast<uint32_t>((static_cast<uint64_t>(exponent & 1) << 15) | (frac >> 37));
    const EstimateSegment& s = kRsqrteSegments[index >> 11];
    const uint64_t significand = s.start - uint64_t{s.slope} * (index & 0x7FF);
    // 2^-(e/2) for the unbiased exponent e, rounded down.
    const uint64_t result_exponent = static_cast<uint64_t>((3068 - exponent) >> 1);
    return from_bits((result_exponent << 52) | (significand << 26));
}

// fres: zeros give infinities and infinities zeros, of the same sign; NaNs
// their quiet form; a reciprocal beyond the single range the largest
// single, one below it zero.
inline double res(double x) {
    const uint64_t v = bits(x);
    const uint64_t sign = v & kSign;
    const int64_t exponent = static_cast<int64_t>((v & kExp) >> 52);
    const uint64_t frac = v & kFrac;
    if (exponent == 0x7FF) return frac != 0 ? quiet(x) : from_bits(sign);
    if (exponent == 0 && frac == 0) return from_bits(sign | kExp);
    if (exponent < 895) return from_bits(sign | bits(static_cast<double>(std::numeric_limits<float>::max())));
    if (exponent >= 1149) return from_bits(sign);
    const uint32_t index = static_cast<uint32_t>(frac >> 37);
    const EstimateSegment& s = kResSegments[index >> 10];
    const uint64_t significand = s.start - (uint64_t{s.slope} * (index & 0x3FF) + 1) / 2;
    return from_bits(sign | (static_cast<uint64_t>(0x7FD - exponent) << 52) | (significand << 29));
}

// ---------------------------------------------------------------------------
// Paired-single quantized load/store, driven by GQR[i] at run time.
// GQR: st_type [0:2], st_scale [8:13], ld_type [16:18], ld_scale [24:29].
inline constexpr std::array<float, 64> kDequantize = [] {
    std::array<float, 64> t{};
    for (int i = 0; i < 64; ++i) {
        // 6-bit signed scale: value * 2^-scale
        const int scale = i < 32 ? i : i - 64;
        float f = 1.0f;
        for (int s = 0; s < (scale < 0 ? -scale : scale); ++s) {
            f = scale < 0 ? f * 2.0f : f * 0.5f;
        }
        t[static_cast<size_t>(i)] = f;
    }
    return t;
}();
inline constexpr std::array<float, 64> kQuantize = [] {
    std::array<float, 64> t{};
    for (int i = 0; i < 64; ++i) {
        const int scale = i < 32 ? i : i - 64;
        float f = 1.0f;
        for (int s = 0; s < (scale < 0 ? -scale : scale); ++s) {
            f = scale < 0 ? f * 0.5f : f * 2.0f;
        }
        t[static_cast<size_t>(i)] = f;
    }
    return t;
}();

template <typename T>
inline T quantize(double ps, uint32_t scale) {
    float v = static_cast<float>(ps) * kQuantize[scale];
    if (v != v) {
        return static_cast<T>(0); // x86 conversion of NaN, narrowed
    }
    constexpr float lo = static_cast<float>(std::numeric_limits<T>::min());
    constexpr float hi = static_cast<float>(std::numeric_limits<T>::max());
    v = v < lo ? lo : (v > hi ? hi : v);
    return static_cast<T>(v);
}

template <bool W>
inline void psq_load(PPCContext& ctx, const uint8_t* base, uint32_t frd,
                     uint32_t ea, uint32_t gqr) {
    const uint32_t type = (gqr >> 16) & 7;
    const float scale = kDequantize[(gqr >> 24) & 0x3F];
    double ps0, ps1 = 1.0;
    switch (type) {
    case 4: // u8
        ps0 = static_cast<float>(ld8(base, ea)) * scale;
        if (!W) ps1 = static_cast<float>(ld8(base, ea + 1)) * scale;
        break;
    case 5: // u16
        ps0 = static_cast<float>(ld16(base, ea)) * scale;
        if (!W) ps1 = static_cast<float>(ld16(base, ea + 2)) * scale;
        break;
    case 6: // s8
        ps0 = static_cast<float>(static_cast<int8_t>(ld8(base, ea))) * scale;
        if (!W) ps1 = static_cast<float>(static_cast<int8_t>(ld8(base, ea + 1))) * scale;
        break;
    case 7: // s16
        ps0 = static_cast<float>(static_cast<int16_t>(ld16(base, ea))) * scale;
        if (!W) ps1 = static_cast<float>(static_cast<int16_t>(ld16(base, ea + 2))) * scale;
        break;
    default: // 0 = float; 1-3 are reserved and treated as float
        ps0 = load_single(ld32(base, ea));
        if (!W) ps1 = load_single(ld32(base, ea + 4));
        break;
    }
    ctx.f[frd].ps0 = ps0;
    ctx.f[frd].ps1 = ps1;
}

template <bool W>
inline void psq_store(const PPCContext& ctx, uint8_t* base, uint32_t frs,
                      uint32_t ea, uint32_t gqr) {
    const uint32_t type = gqr & 7;
    const uint32_t scale = (gqr >> 8) & 0x3F;
    const double ps0 = ctx.f[frs].ps0;
    const double ps1 = ctx.f[frs].ps1;
    switch (type) {
    case 4:
        st8(base, ea, quantize<uint8_t>(ps0, scale));
        if (!W) st8(base, ea + 1, quantize<uint8_t>(ps1, scale));
        break;
    case 5:
        st16(base, ea, quantize<uint16_t>(ps0, scale));
        if (!W) st16(base, ea + 2, quantize<uint16_t>(ps1, scale));
        break;
    case 6:
        st8(base, ea, static_cast<uint8_t>(quantize<int8_t>(ps0, scale)));
        if (!W) st8(base, ea + 1, static_cast<uint8_t>(quantize<int8_t>(ps1, scale)));
        break;
    case 7:
        st16(base, ea, static_cast<uint16_t>(quantize<int16_t>(ps0, scale)));
        if (!W) st16(base, ea + 2, static_cast<uint16_t>(quantize<int16_t>(ps1, scale)));
        break;
    default:
        st32(base, ea, store_single_ftz(ps0));
        if (!W) st32(base, ea + 4, store_single_ftz(ps1));
        break;
    }
}

} // namespace cafe::ppc

// Short names used by generated code.
#define PPC_LOAD_U8(ea) ::cafe::ppc::ld8(base, (ea))
#define PPC_LOAD_U16(ea) ::cafe::ppc::ld16(base, (ea))
#define PPC_LOAD_U32(ea) ::cafe::ppc::ld32(base, (ea))
#define PPC_LOAD_U64(ea) ::cafe::ppc::ld64(base, (ea))
#define PPC_STORE_U8(ea, v) ::cafe::ppc::st8(base, (ea), (v))
#define PPC_STORE_U16(ea, v) ::cafe::ppc::st16(base, (ea), (v))
#define PPC_STORE_U32(ea, v) ::cafe::ppc::st32(base, (ea), (v))
#define PPC_STORE_U64(ea, v) ::cafe::ppc::st64(base, (ea), (v))
