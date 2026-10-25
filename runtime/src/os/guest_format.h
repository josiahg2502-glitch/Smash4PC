#pragma once

// printf-style formatting of a guest format string and guest varargs, as
// passed under the PowerPC EABI: integers in r3..r10, floating point in
// f1..f8, further arguments in the caller's parameter save area.

#include "cafe/ppc_context.h"

#include <cstdint>
#include <string>

namespace cafe::os {

std::string format_guest(PPCContext& ctx, uint32_t format_address, int first_gpr);

} // namespace cafe::os
