// Practice: 3 and + pressed together put both fighters back where the stage
// put them, as Tekken 8's position reset does.
//
// The title starts a Practice round (mode kinds 4 and 5) with its situation
// set-up sub_039C2DB0 for kind 15: its Practice positioner (sub_04F21428)
// places the fighters as the Position Settings say (by default 1 m either
// side of the stage's centre; REVERSE swaps them) and every fighter starts
// over, standing. The reset runs the same set-up from the battle's state
// (GameMode::Impl::state_game_battle, sub_0726DF8C) at the start of a frame,
// with the camera calls of the title's own Practice restart (sub_04A28490):
// the round's start camera, and the 2P one when REVERSE is on.
//
// 3 is the title's own button, after the Controller Setup: the fighter's
// buttons at +2032 (1 to 4 = 0x200 to 0x1000), read from the fighter the
// GamePad drives (its raw buttons at +2052; the training dummy's stay 0).
// The input layer holds + back a moment so that it can join 3
// (cafe::os::arm_start_chord), armed only on the fighters' frames, which
// stop while the pause menu is open; a + that is the chord's never opens it.

#include "cafe/input.h"
#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace cafe;

constexpr uint32_t kMatchObject = 0x10872CB8; // *(*(*it + 228) + 228) = the mode manager (sub_06ECB1B0)
constexpr uint32_t kModeKind = 444;
constexpr uint32_t kPracticeKinds[] = {4, 5};                       // as sub_06ECB3CC
constexpr uint32_t kPracticeSettings = 0x108D1C60, kReverse = 100; // sub_04F1BD58; 1: the player on the right
constexpr uint32_t kSidePlayers = 0x1087FB60;                     // per side: its first member (sub_05C5EF14)
constexpr uint32_t kPlayerActive = 7808, kPlayerPartner = 172;
constexpr uint32_t kPlayerButtons = 2032, kButton3 = 0x800, kPlayerPad = 2052;
constexpr uint32_t kPracticeStart = 15; // the situation of a Practice round's start
constexpr uint32_t k2PCamera = 2;

uint32_t load(uint8_t* base, uint32_t address) { return ppc::ld32(base, address); }

bool practice(uint8_t* base) {
    const uint32_t object = load(base, kMatchObject);
    const uint32_t match = object ? load(base, object + 228) : 0;
    const uint32_t mode = match ? load(base, match + 228) : 0;
    if (!mode) return false;
    const uint32_t kind = load(base, mode + kModeKind);
    for (uint32_t k : kPracticeKinds) {
        if (kind == k) return true;
    }
    return false;
}

// The fighter a side has in play (sub_05C5EF14).
uint32_t active_fighter(uint8_t* base, uint32_t side) {
    const uint32_t first = load(base, kSidePlayers + 4 * side);
    if (!first || load(base, first + kPlayerActive)) return first;
    return load(base, first + kPlayerPartner);
}

bool three_held(uint8_t* base) {
    for (uint32_t side = 0; side < 2; ++side) {
        const uint32_t fighter = active_fighter(base, side);
        if (fighter && load(base, fighter + kPlayerPad) && (load(base, fighter + kPlayerButtons) & kButton3)) {
            return true;
        }
    }
    return false;
}

// Calls guest function `f` with arguments (and f1); -> r3. The caller's registers are kept.
uint32_t call(PPCContext& ctx, uint8_t* base, void (*f)(PPCContext&, uint8_t*), std::initializer_list<uint32_t> args,
              double f1 = 0.0) {
    const PPCContext saved = ctx;
    uint32_t r = 3;
    for (uint32_t a : args) ctx.r[r++] = a;
    ctx.f[1] = {f1, f1};
    f(ctx, base);
    const uint32_t result = ctx.r[3];
    ctx = saved;
    return result;
}

} // namespace

PPC_FUNC(sub_039C2DB0); // the situation set-up (r3: the situation)
PPC_FUNC(sub_05FBD4C8); // -> the camera
PPC_FUNC(sub_05FBB480); // the camera (r3) as at a round's start (r4 = 0)
PPC_FUNC(sub_05FBC0A4); // the camera (r3) of a side (r4 = 2: 2P; f1 = 1, r5 = 1)

// The fighters' frame (not while the pause menu is open): the chord can happen.
PPC_FUNC(sub_03ADB118_orig);
PPC_FUNC(sub_03ADB118) {
    if (practice(base)) os::arm_start_chord(three_held(base));
    sub_03ADB118_orig(ctx, base);
}

PPC_FUNC(sub_0726DF8C_orig);
PPC_FUNC(sub_0726DF8C) {
    if (os::take_start_chord() && practice(base)) {
        const bool reverse = load(base, kPracticeSettings + kReverse) == 1;
        if (std::getenv("TTT2_TRACE_PRACTICE")) {
            std::fprintf(stderr, "ttt2: practice: position reset%s\n", reverse ? " (reversed)" : "");
        }
        call(ctx, base, sub_039C2DB0, {kPracticeStart});
        const uint32_t camera = call(ctx, base, sub_05FBD4C8, {});
        call(ctx, base, sub_05FBB480, {camera, 0});
        if (reverse) call(ctx, base, sub_05FBC0A4, {camera, k2PCamera, 1}, 1.0);
    }
    sub_0726DF8C_orig(ctx, base);
}
