#include "program.h"

#include "ppc.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <stdexcept>

namespace cafe::recomp {
namespace {

bool is_branch(ppc::Op op) { return op == ppc::Op::b || op == ppc::Op::bc; }

// Whether execution can run past the last word of the body.
bool can_fall_off(uint32_t last) {
    const bool always = (ppc::rd(last) & 0x14) == 0x14;
    switch (ppc::decode(last)) {
    case ppc::Op::b:
        return ppc::lk(last);
    case ppc::Op::bclr:
    case ppc::Op::bcctr:
        return ppc::lk(last) || !always;
    case ppc::Op::tw:
        return ppc::rd(last) != 31;
    default:
        return true;
    }
}

std::string sanitize(const std::string& text) {
    std::string out;
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

bool in_code(const rpx::Image& image, uint32_t address) {
    const rpx::Section* section = image.section_containing(address);
    return section != nullptr && section->executable() && section->type == rpx::kShtProgbits;
}

uint32_t relocation_target(const rpx::Image& image, const rpx::Relocation& relocation) {
    return image.symbols[relocation.symbol].value + static_cast<uint32_t>(relocation.addend);
}

// Where other code enters: the entry point, bl targets and code addresses
// stored in data. Splits the symbol-table gaps, which in a stripped
// executable cover all of .text.
std::set<uint32_t> entered_addresses(const rpx::Image& image) {
    std::set<uint32_t> starts{image.entry_point};
    for (const auto& section : image.sections) {
        if (!section.executable() || section.type != rpx::kShtProgbits) {
            continue;
        }
        for (uint32_t offset = 0; offset + 4 <= section.data.size(); offset += 4) {
            const uint32_t a = section.address + offset;
            const uint32_t w = rpx::read_be32(section.data.data() + offset);
            if (ppc::decode(w) != ppc::Op::b || !ppc::lk(w)) {
                continue;
            }
            const auto relocated = image.rel24_targets.find(a);
            const uint32_t target =
                relocated != image.rel24_targets.end() ? relocated->second : ppc::branch_target(w, a);
            if (target != a + 4) {
                starts.insert(target);
            }
        }
    }
    for (const auto& relocation : image.relocations) {
        if (relocation.type != rpx::kRPpcRel24 && relocation.symbol < image.symbols.size() &&
            !in_code(image, relocation.offset)) {
            starts.insert(relocation_target(image, relocation));
        }
    }
    return starts;
}

} // namespace

std::string function_name(uint32_t address) {
    char name[16];
    std::snprintf(name, sizeof name, "sub_%08X", address);
    return name;
}

uint32_t Program::word(uint32_t address) const {
    const rpx::Section* section = image.section_containing(address);
    if (section == nullptr || section->data.size() < address - section->address + 4) {
        throw std::runtime_error("no code at address");
    }
    return rpx::read_be32(section->data.data() + (address - section->address));
}

const FunctionInfo* Program::function_containing(uint32_t address) const {
    auto it = std::upper_bound(
        functions.begin(), functions.end(), address,
        [](uint32_t a, const FunctionInfo& f) { return a < f.address; });
    if (it == functions.begin()) {
        return nullptr;
    }
    --it;
    return address - it->address < it->size ? &*it : nullptr;
}

uint32_t Program::branch_destination(uint32_t address, uint32_t w) const {
    const auto relocated = image.rel24_targets.find(address);
    return relocated != image.rel24_targets.end()
               ? relocated->second
               : ppc::branch_target(w, address);
}

std::string Program::symbol_for(uint32_t address) const {
    const auto import = import_symbols.find(address);
    if (import != import_symbols.end()) {
        return import->second;
    }
    return function_name(address);
}

bool Program::falls_off_end(const FunctionInfo& function) const {
    return can_fall_off(word(function.address + function.size - 4));
}

Program Program::analyze(rpx::Image loaded) {
    // Bodies: every sized function symbol, plus code found in the gaps
    // between them (the compiler's register save/restore helpers carry no
    // symbol; a stripped executable is all gap). Gaps are split where other
    // code enters them. A split need not match the source functions: a branch
    // between bodies becomes a tail call, so a wrong split only costs speed.
    // Padding words at a body's edges are not code.
    Program scratch;
    scratch.image = std::move(loaded);
    const rpx::Image& image = scratch.image;
    const FunctionTable table = FunctionTable::build(image);
    std::set<uint32_t> starts = entered_addresses(image);

    const auto split_gaps = [&] {
        std::vector<FunctionInfo> bodies;
        for (const auto& f : table.functions()) {
            bodies.push_back({f.address, f.size, false, {}, {}, {}});
        }
        for (const auto& gap : table.gaps()) {
            const uint32_t gap_end = gap.address + gap.size;
            uint32_t begin = gap.address;
            for (auto it = starts.upper_bound(gap.address);; ++it) {
                uint32_t end = it != starts.end() && *it < gap_end ? *it : gap_end;
                const uint32_t next = end;
                while (begin < end && ppc::decode(scratch.word(begin)) == ppc::Op::invalid) {
                    begin += 4;
                }
                while (end > begin && ppc::decode(scratch.word(end - 4)) == ppc::Op::invalid) {
                    end -= 4;
                }
                if (begin < end) {
                    bodies.push_back({begin, end - begin, true, {}, {}, {}});
                }
                if (next == gap_end) {
                    break;
                }
                begin = next;
            }
        }
        std::sort(bodies.begin(), bodies.end(),
                  [](const FunctionInfo& a, const FunctionInfo& b) { return a.address < b.address; });
        return bodies;
    };

    // Code addresses formed in code (lis/addi of a callback) are entries too,
    // unless they point into their own body at a run of `b`: a switch table,
    // which analyze() turns into labels.
    scratch.functions = split_gaps();
    for (const auto& relocation : image.relocations) {
        if (relocation.type == rpx::kRPpcRel24 || relocation.symbol >= image.symbols.size() ||
            !in_code(image, relocation.offset)) {
            continue;
        }
        const uint32_t target = relocation_target(image, relocation);
        const FunctionInfo* owner = scratch.function_containing(target);
        if (owner == nullptr || !owner->synthetic || owner->address == target) {
            continue;
        }
        const uint32_t w = scratch.word(target);
        const bool switch_table = scratch.function_containing(relocation.offset) == owner &&
                                  ppc::decode(w) == ppc::Op::b && !ppc::lk(w);
        if (!switch_table) {
            starts.insert(target);
        }
    }
    std::vector<FunctionInfo> bodies = split_gaps();
    return analyze(std::move(scratch.image), std::move(bodies));
}

Program Program::synthetic(uint32_t address, const std::vector<uint32_t>& words,
                           uint32_t words_per_function) {
    rpx::Image image;
    image.sections.emplace_back();
    rpx::Section text;
    text.index = 1;
    text.name = ".text";
    text.type = rpx::kShtProgbits;
    text.flags = rpx::kShfAlloc | rpx::kShfExecinstr;
    text.address = address;
    text.data.resize(words.size() * 4);
    for (size_t i = 0; i < words.size(); ++i) {
        rpx::write_be32(text.data.data() + i * 4, words[i]);
    }
    text.size = static_cast<uint32_t>(text.data.size());
    image.sections.push_back(std::move(text));
    std::vector<FunctionInfo> bodies;
    for (size_t i = 0; i + words_per_function <= words.size(); i += words_per_function) {
        bodies.push_back({address + static_cast<uint32_t>(i * 4), words_per_function * 4,
                          false, {}, {}, {}});
    }
    return analyze(std::move(image), std::move(bodies));
}

Program Program::analyze(rpx::Image loaded, std::vector<FunctionInfo> bodies) {
    Program program;
    program.image = std::move(loaded);
    const rpx::Image& image = program.image;

    for (const auto& import : image.imports) {
        if (!import.is_data) {
            program.import_symbols[import.stub_address] =
                "imp_" + sanitize(import.module) + "_" + sanitize(import.name);
        }
    }
    program.functions = std::move(bodies);
    std::sort(program.functions.begin(), program.functions.end(),
              [](const FunctionInfo& a, const FunctionInfo& b) {
                  return a.address < b.address;
              });

    for (size_t index = 0; index < program.functions.size(); ++index) {
        program.entries[program.functions[index].address] = {
            program.functions[index].address, index, false};
    }

    std::set<uint32_t> alternate;
    const auto note_foreign_target = [&](uint32_t target) {
        if (program.entries.count(target) || program.import_symbols.count(target)) {
            return;
        }
        if (program.function_containing(target) != nullptr) {
            alternate.insert(target);
        }
    };

    // Direct control flow.
    for (auto& function : program.functions) {
        const uint32_t end = function.address + function.size;
        for (uint32_t a = function.address; a < end; a += 4) {
            const uint32_t w = program.word(a);
            const ppc::Op op = ppc::decode(w);
            if (!is_branch(op)) {
                continue;
            }
            const uint32_t target = program.branch_destination(a, w);
            const bool local = target >= function.address && target < end;
            if (ppc::lk(w)) {
                // bl to the next instruction only reads the PC into LR.
                if (target != a + 4) {
                    note_foreign_target(target);
                }
            } else if (local) {
                function.labels.insert(target);
            } else {
                note_foreign_target(target);
            }
        }
    }

    // Address-taken code. Inside the referencing function it is a computed
    // jump (switch table of `b` instructions); from anywhere else it is an
    // entry point.
    for (const auto& relocation : image.relocations) {
        if (relocation.type == rpx::kRPpcRel24 ||
            relocation.symbol >= image.symbols.size()) {
            continue;
        }
        const uint32_t target = image.symbols[relocation.symbol].value +
                                static_cast<uint32_t>(relocation.addend);
        FunctionInfo* owner = const_cast<FunctionInfo*>(program.function_containing(target));
        if (owner == nullptr) {
            continue;
        }
        const uint32_t site = relocation.offset & ~3u;
        const uint32_t end = owner->address + owner->size;
        if (site >= owner->address && site < end && target != owner->address) {
            for (uint32_t a = target; a < end; a += 4) {
                owner->switch_targets.insert(a);
                owner->labels.insert(a);
                const uint32_t w = program.word(a);
                if (ppc::decode(w) != ppc::Op::b || ppc::lk(w)) {
                    break;
                }
            }
        } else {
            note_foreign_target(target);
        }
    }

    for (const uint32_t target : alternate) {
        const FunctionInfo* owner = program.function_containing(target);
        const size_t index = static_cast<size_t>(owner - program.functions.data());
        program.functions[index].alternate_entries.push_back(target);
        program.functions[index].labels.insert(target);
        program.entries[target] = {target, index, false};
    }

    // Reachability from the entry point and every address-taken entry. Every
    // stored code address in an RPL carries a relocation, so this bound is
    // sound; the runtime still reports calls into anything left out.
    std::deque<uint32_t> work;
    const auto mark = [&](uint32_t address) {
        auto it = program.entries.find(address);
        if (it != program.entries.end() && !it->second.reachable) {
            it->second.reachable = true;
            work.push_back(address);
        }
    };
    mark(image.entry_point);
    for (const auto& relocation : image.relocations) {
        if (relocation.type != rpx::kRPpcRel24 &&
            relocation.symbol < image.symbols.size()) {
            mark(image.symbols[relocation.symbol].value +
                 static_cast<uint32_t>(relocation.addend));
        }
    }
    std::set<size_t> scanned;
    while (!work.empty()) {
        const Entry entry = program.entries.at(work.front());
        work.pop_front();
        if (!scanned.insert(entry.function).second) {
            continue;
        }
        const FunctionInfo& function = program.functions[entry.function];
        const uint32_t end = function.address + function.size;
        for (uint32_t a = function.address; a < end; a += 4) {
            const uint32_t w = program.word(a);
            if (is_branch(ppc::decode(w))) {
                mark(program.branch_destination(a, w));
            }
        }
        // Falling off the end continues into the next body.
        if (can_fall_off(program.word(end - 4))) {
            mark(end);
        }
    }
    return program;
}

} // namespace cafe::recomp
