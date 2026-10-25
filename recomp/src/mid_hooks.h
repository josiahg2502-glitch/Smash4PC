#pragma once

#include <cstdint>

namespace cafe::recomp {

// Instructions before which the generated code calls the runtime:
// `cafe_mid_hook_<ADDRESS>(ctx, base)`, a weak no-op unless the runtime
// defines it. For a change inside a guest function too large to replace.
constexpr uint32_t kMidHooks[] = {
    0x06F4D014, // select screen: before the check that a team's members differ (runtime/src/title/roster.cpp)
};

} // namespace cafe::recomp
