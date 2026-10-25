#pragma once

#include "function_table.h"
#include "rpx.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cafe::recomp {

// A guest function body as the code generator sees it.
struct FunctionInfo {
    uint32_t address{};
    uint32_t size{};
    bool synthetic{};              // recovered from a symbol-table gap
    std::set<uint32_t> labels;     // in-body targets needing a C++ label
    std::set<uint32_t> switch_targets; // computed-jump (bctr) destinations
    std::vector<uint32_t> alternate_entries; // entered mid-body by other code
};

// One generated C++ function: a function start or an alternate entry. An
// alternate entry is emitted as a copy of its containing body that begins by
// jumping to the entry label.
struct Entry {
    uint32_t address{};
    size_t function{}; // index into Program::functions
    bool reachable{};
};

struct Program {
    rpx::Image image;
    std::vector<FunctionInfo> functions; // sorted by address
    std::map<uint32_t, Entry> entries;   // by guest address
    std::map<uint32_t, std::string> import_symbols; // stub address -> C name

    // Bodies from the RPX symbol table (and code in its gaps).
    static Program analyze(rpx::Image image);
    // Bodies given explicitly.
    static Program analyze(rpx::Image image, std::vector<FunctionInfo> bodies);
    // A one-section image holding `words` at `address`, split into functions
    // of `words_per_function` words. Used to test the code generator.
    static Program synthetic(uint32_t address, const std::vector<uint32_t>& words,
                             uint32_t words_per_function);

    uint32_t word(uint32_t address) const;
    const FunctionInfo* function_containing(uint32_t address) const;
    // Destination of the b/bc at `address`, honouring REL24 relocations
    // (needed for import calls, which no displacement can encode).
    uint32_t branch_destination(uint32_t address, uint32_t word) const;
    bool falls_off_end(const FunctionInfo& function) const;
    // C identifier for a callable guest address: sub_XXXXXXXX or an import.
    std::string symbol_for(uint32_t address) const;
};

std::string function_name(uint32_t address);

} // namespace cafe::recomp
