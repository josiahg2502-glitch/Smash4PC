#pragma once

#include "rpx.h"

#include <cstdint>
#include <vector>

namespace cafe::recomp {

struct Function {
    uint32_t address{};
    uint32_t size{};
};

// Function boundaries recovered from the RPX symbol table. Most Cafe executables
// keep a sized STT_FUNC symbol for every function even when names are
// stripped, so the table partitions .text exactly; Program::analyze splits
// what it leaves uncovered (all of .text in a stripped executable).
class FunctionTable {
public:
    static FunctionTable build(const rpx::Image& image);

    const std::vector<Function>& functions() const { return functions_; }
    const Function* find_exact(uint32_t address) const;
    const Function* find_containing(uint32_t address) const;
    // Byte ranges of executable sections that no function symbol covers.
    const std::vector<Function>& gaps() const { return gaps_; }

private:
    std::vector<Function> functions_; // sorted, non-overlapping
    std::vector<Function> gaps_;
};

} // namespace cafe::recomp
