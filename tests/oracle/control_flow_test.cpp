// Differential test of generated control flow: small programs (cafe-cfgen)
// run natively and on Dolphin's interpreter from the same random states;
// final registers, memory, and whether a trap fired must match.

#include "control_flow_cases.h"
#include "harness.h"
#include "oracle.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::jmp_buf g_escape;
enum class Exit { returned = 0, trapped = 1, bad_return = 2 };

} // namespace

extern "C" {
cafe::PPCFunc* cafe_ppc_lookup(uint32_t address) {
    for (size_t i = 0; i < kGuestEntryCount; ++i) {
        if (kGuestEntries[i].address == address) return kGuestEntries[i].function;
    }
    std::fprintf(stderr, "lookup of unknown address %08X\n", address);
    std::abort();
}
void cafe_ppc_trap(cafe::PPCContext&, uint32_t) { std::longjmp(g_escape, static_cast<int>(Exit::trapped)); }
void cafe_ppc_bad_return(cafe::PPCContext&, uint32_t) {
    std::longjmp(g_escape, static_cast<int>(Exit::bad_return));
}
void cafe_ppc_illegal(cafe::PPCContext&, uint32_t a, uint32_t w, const char* why) {
    std::fprintf(stderr, "illegal %08X at %08X: %s\n", w, a, why);
    std::abort();
}
void cafe_ppc_null_call(cafe::PPCContext&, uint32_t) { std::abort(); }
uint64_t cafe_ppc_timebase(void) { return 0; }
}

int main(int argc, char** argv) {
    using namespace harness;
    const int trials = argc > 1 ? std::atoi(argv[1]) : 200;
    constexpr uint32_t kReturn = 0x00000100; // sentinel return address
    uint8_t* ours = reserve_space();
    uint8_t* theirs = reserve_space();
    long runs = 0, failures = 0;

    for (size_t c = 0; c < kControlFlowCaseCount; ++c) {
        const ControlFlowCase& test = kControlFlowCases[c];
        Rng rng{0xC0DE0000ull + c};
        int reported = 0;
        for (int trial = 0; trial < trials; ++trial) {
            cafe::PPCContext ctx{};
            randomise(ctx, rng);
            static const uint32_t ctr_values[] = {0, 1, 2, 3};
            if (rng.below(2)) ctx.ctr = ctr_values[rng.below(4)];
            if (test.r4_hi > test.r4_lo) ctx.r[4] = test.r4_lo + rng.below(test.r4_hi - test.r4_lo);
            if (test.r5_hi > test.r5_lo) ctx.r[5] = test.r5_lo + rng.below(test.r5_hi - test.r5_lo);
            if (test.r4_address) {
                ctx.r[4] = 0x00048000u + (rng.below(0x4000) & ~31u);
                ctx.r[8] = ctx.r[4] + 64;
            }
            ctx.lr = kReturn;
            randomise_memory(ours, theirs, rng);
            for (size_t i = 0; i < kCodeWordCount; ++i) {
                cafe::ppc::st32(ours, kCodeBase + static_cast<uint32_t>(i) * 4, kCodeWords[i]);
                cafe::ppc::st32(theirs, kCodeBase + static_cast<uint32_t>(i) * 4, kCodeWords[i]);
            }

            OracleState expected;
            to_oracle(ctx, expected);
            const OracleRun oracle = oracle_run(expected, theirs, test.entry, kReturn, 1000000);
            if (oracle == OracleRun::exception || oracle == OracleRun::step_limit) {
                std::printf("FAIL %s: oracle stopped abnormally (%d)\n", test.name, static_cast<int>(oracle));
                ++failures;
                continue;
            }

            const int exit_code = setjmp(g_escape);
            if (exit_code == 0) {
                test.function(ctx, ours);
            }
            const auto exit = static_cast<Exit>(exit_code);
            OracleState actual;
            to_oracle(ctx, actual);
            std::string diff;
            const bool oracle_trapped = oracle == OracleRun::trapped;
            if (exit == Exit::bad_return) {
                diff += " callee returned elsewhere;";
            } else if ((exit == Exit::trapped) != oracle_trapped) {
                diff += oracle_trapped ? " trap expected;" : " unexpected trap;";
            } else {
                diff += compare(expected, actual);
                if (!memory_matches(ours, theirs)) diff += " memory differs;";
            }
            ++runs;
            if (!diff.empty()) {
                ++failures;
                if (reported++ < 2) std::printf("FAIL %s:%s\n", test.name, diff.c_str());
            }
        }
    }
    std::printf("%zu programs, %ld runs, %ld failures\n", kControlFlowCaseCount, runs, failures);
    return failures == 0 ? 0 : 1;
}
