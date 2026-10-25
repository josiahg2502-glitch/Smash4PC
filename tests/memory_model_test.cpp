// Guest memory is shared between guest threads. Generated code polls it in
// loops such as
//
//   loop: lwz r0, 0(r31); cmpwi r0, 0; bne loop
//
// waiting for another thread's store. If guest loads were ordinary C++
// loads, the compiler could read once and drop the loop (an infinite loop
// without side effects is undefined behaviour), and the thread would never
// wait. This compiles such loops exactly as cafe-recomp emits them, with the
// port's optimisation flags, and checks they see another thread's store.

#include "cafe/ppc_ops.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

extern "C" cafe::PPCFunc* cafe_ppc_lookup(uint32_t) { std::abort(); }
extern "C" void cafe_ppc_bad_return(cafe::PPCContext&, uint32_t) { std::abort(); }

// while (*(u32*)r31 != 0) {}
PPC_FUNC(poll_word) {
loc_loop:
    ctx.r[0] = PPC_LOAD_U32(ctx.r[31]);
    ::cafe::ppc::cmp_signed(ctx.cr[0], static_cast<int32_t>(ctx.r[0]), 0, ctx.xer_so);
    if (!ctx.cr[0].eq) { goto loc_loop; }
    return;
}

// while (*(u8*)r31 == 0) {}, then store 1 to the word at r30.
PPC_FUNC(wait_byte_then_ack) {
loc_loop:
    ctx.r[0] = PPC_LOAD_U8(ctx.r[31]);
    ::cafe::ppc::cmp_signed(ctx.cr[0], static_cast<int32_t>(ctx.r[0]), 0, ctx.xer_so);
    if (ctx.cr[0].eq) { goto loc_loop; }
    ctx.r[0] = 1;
    PPC_STORE_U32(ctx.r[30], ctx.r[0]);
    return;
}

namespace {

// The loop must still be waiting before `release` and finish after it.
bool waits_then_finishes(void (*body)(cafe::PPCContext&, uint8_t*), uint8_t* base, cafe::PPCContext ctx,
                         void (*release)(uint8_t*)) {
    std::atomic<bool> done{false};
    std::thread guest([&] {
        body(ctx, base);
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (done) {
        guest.join();
        return false; // returned without waiting: the loop was compiled away
    }
    release(base);
    for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!done) {
        // The loop cannot see the store: leave the thread spinning and fail.
        guest.detach();
        return false;
    }
    guest.join();
    return true;
}

} // namespace

int main() {
    std::vector<uint8_t> memory(0x1000, 0);
    uint8_t* base = memory.data();
    int failures = 0;

    cafe::ppc::st32(base, 0x100, 1);
    cafe::PPCContext a{};
    a.r[31] = 0x100;
    if (!waits_then_finishes(poll_word, base, a, [](uint8_t* b) { cafe::ppc::st32(b, 0x100, 0); })) {
        std::printf("FAIL: a word-polling loop did not wait for the other thread's store\n");
        ++failures;
    }

    cafe::PPCContext b{};
    b.r[31] = 0x200;
    b.r[30] = 0x204;
    if (!waits_then_finishes(wait_byte_then_ack, base, b, [](uint8_t* m) { cafe::ppc::st8(m, 0x200, 1); }) ||
        cafe::ppc::ld32(base, 0x204) != 1) {
        std::printf("FAIL: a byte-polling loop did not wait for the other thread's store\n");
        ++failures;
    }

    std::printf("%s\n", failures == 0 ? "memory model: guest polling loops observe other threads" : "memory model: FAILED");
    std::fflush(stdout);
    std::_Exit(failures == 0 ? 0 : 1);
}
