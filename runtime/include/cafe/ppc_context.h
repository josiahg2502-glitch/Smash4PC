#pragma once

// Guest CPU state shared by generated code and the runtime.

#include <cstdint>

namespace cafe {

struct CRField {
    uint8_t lt, gt, eq, so;
};

// Floating-point register as the Espresso holds it: two slots. Scalar double
// instructions touch ps0 only; single-precision and paired-single instructions
// write both.
struct FPR {
    double ps0, ps1;
};

struct alignas(64) PPCContext {
    uint32_t r[32];
    FPR f[32];
    CRField cr[8];
    uint32_t lr;
    uint32_t ctr;
    uint8_t xer_so, xer_ov, xer_ca, xer_bc; // bc: lswx/stswx byte count
    uint32_t fpscr;
    uint32_t gqr[8];
    uint32_t reserve_address;
    uint32_t reserve_value; // raw big-endian word observed by lwarx
    uint8_t reserve_valid;
    void* host_thread; // owned by the runtime
};

using PPCFunc = void(PPCContext& __restrict ctx, uint8_t* __restrict base);

} // namespace cafe
