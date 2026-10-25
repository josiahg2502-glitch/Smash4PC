#pragma once

#include "program.h"

#include <set>
#include <string>

namespace cafe::recomp {

struct EmitOptions {
    bool comments = true; // disassembly comment above each instruction
    bool mid_hooks = true; // calls to the runtime before the instructions of mid_hooks.h
};

// The name of the runtime function called before the instruction at `address`.
std::string mid_hook_name(uint32_t address);

// C++ definition of one entry (function start or alternate entry). Symbols it
// calls are added to `referenced` so the caller can declare them.
std::string emit_entry(const Program& program, const Entry& entry,
                       const EmitOptions& options,
                       std::set<std::string>& referenced);

} // namespace cafe::recomp
