#pragma once

// Reference execution of one instruction on Dolphin's Broadway interpreter
// (built from third_party/ref, test-only). Kept free of Dolphin types so the
// test driver never mixes Dolphin's headers with the port's.

#include <cstdint>

struct OracleState {
    uint32_t gpr[32];
    uint64_t ps0[32];
    uint64_t ps1[32];
    uint8_t cr[8]; // 4-bit fields: LT GT EQ SO
    uint8_t so, ov, ca, bc;
    uint32_t lr, ctr;
    uint32_t gqr[8];
};

// Runs `word` against `memory` (guest address space base). Returns false if
// the interpreter raised an exception, in which case the result is not
// comparable.
bool oracle_execute(OracleState& state, uint8_t* memory, uint32_t word);

enum class OracleRun { returned, trapped, exception, step_limit };

// Runs code from `entry` until control reaches `stop` (normally the return
// address placed in LR), fetching instructions from `memory`.
OracleRun oracle_run(OracleState& state, uint8_t* memory, uint32_t entry, uint32_t stop,
                     uint32_t max_steps);
