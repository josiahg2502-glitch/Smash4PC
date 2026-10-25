#include "function_table.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace cafe::recomp {

FunctionTable FunctionTable::build(const rpx::Image& image) {
    std::map<uint32_t, uint32_t> sizes;
    for (const auto& symbol : image.symbols) {
        if (symbol.type != rpx::kSttFunc || symbol.section == 0 ||
            symbol.section >= image.sections.size()) {
            continue;
        }
        const rpx::Section& section = image.sections[symbol.section];
        if (!section.executable() || section.type != rpx::kShtProgbits ||
            !section.contains(symbol.value)) {
            continue;
        }
        // Zero-sized aliases share an address with the sized definition.
        uint32_t& size = sizes[symbol.value];
        size = std::max(size, symbol.size);
    }

    FunctionTable table;
    for (const auto& [address, size] : sizes) {
        if (size == 0) {
            continue;
        }
        if ((address & 3) != 0 || (size & 3) != 0) {
            throw std::runtime_error("misaligned function symbol");
        }
        if (!table.functions_.empty()) {
            const Function& previous = table.functions_.back();
            if (previous.address + previous.size > address) {
                throw std::runtime_error("overlapping function symbols");
            }
        }
        table.functions_.push_back({address, size});
    }

    for (const auto& section : image.sections) {
        if (!section.executable() || section.type != rpx::kShtProgbits ||
            section.size == 0) {
            continue;
        }
        uint32_t cursor = section.address;
        const uint32_t end = section.address + section.size;
        for (const auto& function : table.functions_) {
            if (function.address < section.address || function.address >= end) {
                continue;
            }
            if (function.address > cursor) {
                table.gaps_.push_back({cursor, function.address - cursor});
            }
            cursor = function.address + function.size;
        }
        if (cursor < end) {
            table.gaps_.push_back({cursor, end - cursor});
        }
    }
    return table;
}

const Function* FunctionTable::find_exact(uint32_t address) const {
    const auto it = std::lower_bound(
        functions_.begin(), functions_.end(), address,
        [](const Function& f, uint32_t a) { return f.address < a; });
    return it != functions_.end() && it->address == address ? &*it : nullptr;
}

const Function* FunctionTable::find_containing(uint32_t address) const {
    auto it = std::upper_bound(
        functions_.begin(), functions_.end(), address,
        [](uint32_t a, const Function& f) { return a < f.address; });
    if (it == functions_.begin()) {
        return nullptr;
    }
    --it;
    return address - it->address < it->size ? &*it : nullptr;
}

} // namespace cafe::recomp
