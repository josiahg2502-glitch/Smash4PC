#pragma once

// Tables cafe-cfgen emits for the control-flow test.

#include "cafe/ppc_context.h"

#include <cstddef>
#include <cstdint>

struct GuestEntry {
    uint32_t address;
    cafe::PPCFunc* function;
};

struct ControlFlowCase {
    const char* name;
    uint32_t entry;
    cafe::PPCFunc* function;
    uint32_t r4_lo, r4_hi; // r4 drawn from [lo, hi) when hi > lo
    uint32_t r5_lo, r5_hi;
    bool r4_address; // r4/r8 point at distinct aligned words in the data window
};

extern const uint32_t kCodeBase;
extern const uint32_t kCodeWords[];
extern const size_t kCodeWordCount;
extern const GuestEntry kGuestEntries[];
extern const size_t kGuestEntryCount;
extern const ControlFlowCase kControlFlowCases[];
extern const size_t kControlFlowCaseCount;
