// Differential test of generated instruction semantics against Dolphin's
// hardware-verified interpreter. Each test function is one real Tekken
// instruction encoding followed by blr (see cafe-semgen); each runs on many
// random machine states and must match the oracle bit for bit.

#include "harness.h"
#include "oracle.h"

#include "cafe/ppc_ops.h"
#include "ppc.h"

#include <sys/mman.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

struct SemanticsTest {
    uint32_t word;
    cafe::PPCFunc* function;
};
extern const SemanticsTest kSemanticsTests[];
extern const size_t kSemanticsTestCount;

// Hooks the generated code may reference.
extern "C" {
cafe::PPCFunc* cafe_ppc_lookup(uint32_t) { std::abort(); }
void cafe_ppc_trap(cafe::PPCContext&, uint32_t) { std::abort(); }
void cafe_ppc_illegal(cafe::PPCContext&, uint32_t a, uint32_t w, const char* why) {
    std::fprintf(stderr, "illegal %08X at %08X: %s\n", w, a, why);
    std::abort();
}
void cafe_ppc_null_call(cafe::PPCContext&, uint32_t) { std::abort(); }
void cafe_ppc_bad_return(cafe::PPCContext&, uint32_t) { std::abort(); }
uint64_t cafe_ppc_timebase(void) { return 0; }
}

namespace {

bool is_memory_op(cafe::ppc::Op op) {
    using cafe::ppc::Op;
    switch (op) {
    case Op::lbz: case Op::lbzu: case Op::lbzx: case Op::lbzux: case Op::lhz: case Op::lhzu:
    case Op::lhzx: case Op::lhzux: case Op::lha: case Op::lhau: case Op::lhax: case Op::lhaux:
    case Op::lwz: case Op::lwzu: case Op::lwzx: case Op::lwzux: case Op::stb: case Op::stbu:
    case Op::stbx: case Op::stbux: case Op::sth: case Op::sthu: case Op::sthx: case Op::sthux:
    case Op::stw: case Op::stwu: case Op::stwx: case Op::stwux: case Op::lhbrx: case Op::lwbrx:
    case Op::sthbrx: case Op::stwbrx: case Op::lmw: case Op::stmw: case Op::lswi: case Op::lswx:
    case Op::stswi: case Op::stswx: case Op::lfs: case Op::lfsu: case Op::lfsx: case Op::lfsux:
    case Op::lfd: case Op::lfdu: case Op::lfdx: case Op::lfdux: case Op::stfs: case Op::stfsu:
    case Op::stfsx: case Op::stfsux: case Op::stfd: case Op::stfdu: case Op::stfdx: case Op::stfdux:
    case Op::stfiwx: case Op::psq_l: case Op::psq_lu: case Op::psq_lx: case Op::psq_lux:
    case Op::psq_st: case Op::psq_stu: case Op::psq_stx: case Op::psq_stux: case Op::dcbz:
    case Op::dcbz_l:
        return true;
    default:
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace harness;
    const int trials = argc > 1 ? std::atoi(argv[1]) : 300;
    uint8_t* ours = reserve_space();
    uint8_t* theirs = reserve_space();
    struct Tally { int encodings = 0; long runs = 0; long skipped = 0; long failures = 0; };
    std::map<std::string, Tally> tallies;
    long total_failures = 0;

    for (size_t t = 0; t < kSemanticsTestCount; ++t) {
        const SemanticsTest& test = kSemanticsTests[t];
        const uint32_t w = test.word;
        const cafe::ppc::Op op = cafe::ppc::decode(w);
        std::string name = cafe::ppc::op_name(op);
        if (cafe::ppc::has_overflow_form(op) && cafe::ppc::oe(w)) name += 'o';
        if (cafe::ppc::has_record_form(op) && cafe::ppc::rc(w)) name += '.';
        Tally& tally = tallies[name];
        ++tally.encodings;
        int reported = 0;
        Rng rng{0x5EED0000ull + w};
        for (int trial = 0; trial < trials; ++trial) {
            cafe::PPCContext ctx{};
            randomise(ctx, rng);
            if (is_memory_op(op)) {
                // Address registers point into the middle of the low window,
                // word aligned, as compiled code's are.
                const uint32_t ra = cafe::ppc::ra(w);
                if (ra != 0) ctx.r[ra] = 0x00048000u + (rng.below(0x4000) & ~31u);
                const uint32_t rb = cafe::ppc::rb(w);
                const bool indexed = cafe::ppc::opcd(w) == 31 || (cafe::ppc::opcd(w) == 4);
                if (indexed && rb == ra && ra != 0) {
                    ctx.r[ra] = 0x00024000u + (rng.below(0x2000) & ~31u); // rA + rA
                } else if (indexed) {
                    // With rA=0 the index register is the whole address.
                    ctx.r[rb] = ra == 0 ? 0x00048000u + (rng.below(0x4000) & ~31u)
                                        : rng.below(0x400) & ~3u;
                }
            }
            randomise_memory(ours, theirs, rng);

            OracleState expected;
            to_oracle(ctx, expected);
            const OracleState before = expected;
            if (!oracle_execute(expected, theirs, w)) {
                ++tally.skipped;
                continue;
            }
            // Dolphin derives addme/subfme's carry as Carry(a, CA - 1), which
            // is 0 whenever CA was 1. Architecturally the carry is that of
            // a + 0xFFFFFFFF + CA, which is always 1 when CA is 1; Cemu's
            // Espresso interpreter agrees (CA = a || ca). Correct the oracle.
            if ((op == cafe::ppc::Op::addme || op == cafe::ppc::Op::subfme) && before.ca) {
                expected.ca = 1;
            }
            test.function(ctx, ours);
            OracleState actual;
            to_oracle(ctx, actual);
            std::string diff = compare(expected, actual);
            if (!memory_matches(ours, theirs)) {
                diff += " memory differs;";
            }
            ++tally.runs;
            if (!diff.empty()) {
                ++tally.failures;
                ++total_failures;
                if (reported++ < 2) {
                    std::printf("FAIL %08X %-28s%s\n", w, cafe::ppc::disassemble(w, 0).c_str(),
                                diff.c_str());
                    const uint32_t b = cafe::ppc::rb(w), a = cafe::ppc::ra(w), c = cafe::ppc::frc(w);
                    std::printf("     inputs: rA=%08X rB=%08X fA=%016llX/%016llX fB=%016llX/%016llX "
                                "fC=%016llX/%016llX\n",
                                before.gpr[a], before.gpr[b], (unsigned long long)before.ps0[a],
                                (unsigned long long)before.ps1[a], (unsigned long long)before.ps0[b],
                                (unsigned long long)before.ps1[b], (unsigned long long)before.ps0[c],
                                (unsigned long long)before.ps1[c]);
                }
            }
        }
    }

    std::printf("\n%-14s %5s %9s %8s %8s\n", "variant", "enc", "runs", "skipped", "FAILED");
    for (const auto& [name, t] : tallies) {
        std::printf("%-14s %5d %9ld %8ld %8ld%s\n", name.c_str(), t.encodings, t.runs, t.skipped,
                    t.failures, t.failures ? "  <--" : "");
    }
    std::printf("\n%zu encodings, %ld mismatching runs\n", kSemanticsTestCount, total_failures);
    return total_failures == 0 ? 0 : 1;
}
