// Instruction and control-flow census of an RPX, using the symbol table's
// function boundaries. Answers: which instructions must the code generator
// support, and which control flow cannot be resolved statically.

#include "function_table.h"
#include "ppc.h"
#include "rpx.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace cafe;

namespace {

std::string hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", value);
    return text;
}

struct Samples {
    uint64_t count = 0;
    std::vector<uint32_t> addresses;
    void add(uint32_t address) {
        ++count;
        if (addresses.size() < 8) {
            addresses.push_back(address);
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s Tekken.rpx [report.json]\n", argv[0]);
        return 2;
    }
    try {
        const rpx::Image image = rpx::load(argv[1]);
        const recomp::FunctionTable functions = recomp::FunctionTable::build(image);
        const rpx::Section& text = *image.section_named(".text");

        std::map<std::string, uint64_t> mnemonics;
        std::map<uint32_t, uint64_t> mfspr_numbers, mtspr_numbers;
        std::map<std::string, Samples> flow;
        Samples invalid;
        uint64_t words = 0;

        for (const auto& function : functions.functions()) {
            const uint32_t end = function.address + function.size;
            const rpx::Section& code = *image.section_containing(function.address);
            for (uint32_t address = function.address; address < end;
                 address += 4) {
                const uint32_t w = rpx::read_be32(
                    code.data.data() + (address - code.address));
                ++words;
                const ppc::Op op = ppc::decode(w);
                if (op == ppc::Op::invalid) {
                    invalid.add(address);
                    continue;
                }
                std::string name = ppc::op_name(op);
                if (ppc::has_overflow_form(op) && ppc::oe(w)) {
                    name += 'o';
                }
                if (ppc::has_record_form(op) && ppc::rc(w)) {
                    name += '.';
                }
                if ((op == ppc::Op::b || op == ppc::Op::bc ||
                     op == ppc::Op::bclr || op == ppc::Op::bcctr) &&
                    ppc::lk(w)) {
                    name += "(l)";
                }
                ++mnemonics[name];
                if (op == ppc::Op::mfspr) {
                    ++mfspr_numbers[ppc::spr(w)];
                } else if (op == ppc::Op::mtspr) {
                    ++mtspr_numbers[ppc::spr(w)];
                }

                if (op == ppc::Op::b || op == ppc::Op::bc) {
                    uint32_t target = ppc::branch_target(w, address);
                    const auto relocated = image.rel24_targets.find(address);
                    if (relocated != image.rel24_targets.end()) {
                        target = relocated->second;
                    }
                    const bool link = ppc::lk(w);
                    const char* kind = op == ppc::Op::b ? (link ? "bl" : "b")
                                                        : (link ? "bcl" : "bc");
                    std::string where;
                    if (image.import_by_stub.count(target)) {
                        where = "import";
                    } else if (target >= function.address && target < end) {
                        where = target == function.address ? "own-entry"
                                                           : "local";
                    } else if (functions.find_exact(target)) {
                        where = "function-entry";
                    } else if (functions.find_containing(target)) {
                        where = "inside-other-function";
                    } else {
                        where = "outside-functions";
                    }
                    flow[std::string(kind) + " -> " + where].add(address);
                } else if (op == ppc::Op::bclr) {
                    const bool conditional = (ppc::rd(w) & 0x14) != 0x14;
                    flow[std::string(ppc::lk(w) ? "blrl" : "blr") +
                         (conditional ? " (conditional)" : "")]
                        .add(address);
                } else if (op == ppc::Op::bcctr) {
                    const bool conditional = (ppc::rd(w) & 0x14) != 0x14;
                    flow[std::string(ppc::lk(w) ? "bctrl" : "bctr") +
                         (conditional ? " (conditional)" : "")]
                        .add(address);
                } else if (op == ppc::Op::sc) {
                    flow["sc"].add(address);
                }
            }
        }

        // Code addresses the image materialises as data or address constants.
        // Function entries are indirect-call targets; anything else inside a
        // function is a computed-jump target such as a switch case.
        std::map<std::string, Samples> code_refs;
        for (const auto& relocation : image.relocations) {
            if (relocation.type == rpx::kRPpcRel24 ||
                relocation.symbol >= image.symbols.size()) {
                continue;
            }
            const uint32_t target = image.symbols[relocation.symbol].value +
                                    static_cast<uint32_t>(relocation.addend);
            if (!text.contains(target)) {
                continue;
            }
            const std::string from =
                image.sections.at(relocation.target_section).name;
            const char* kind =
                relocation.type == rpx::kRPpcAddr32 ? "addr32" : "addr16";
            const char* where = functions.find_exact(target)
                                    ? "function-entry"
                                    : functions.find_containing(target)
                                          ? "inside-function"
                                          : "outside-functions";
            code_refs[from + " " + kind + " -> " + where].add(relocation.offset);
        }

        std::printf("functions: %zu  words: %llu  invalid words: %llu\n",
                    functions.functions().size(),
                    static_cast<unsigned long long>(words),
                    static_cast<unsigned long long>(invalid.count));
        std::printf("imports: %zu  relocations: %zu  sda=%s sda2=%s stack=%s\n",
                    image.imports.size(), image.relocations.size(),
                    hex(image.file_info.sda_base).c_str(),
                    hex(image.file_info.sda2_base).c_str(),
                    hex(image.file_info.stack_size).c_str());
        std::vector<std::pair<uint64_t, std::string>> sorted;
        for (const auto& [name, count] : mnemonics) {
            sorted.emplace_back(count, name);
        }
        std::sort(sorted.rbegin(), sorted.rend());
        std::printf("\n%zu distinct mnemonics:\n", sorted.size());
        for (const auto& [count, name] : sorted) {
            std::printf("  %-14s %10llu\n", name.c_str(),
                        static_cast<unsigned long long>(count));
        }
        std::printf("\nmfspr:");
        for (const auto& [number, count] : mfspr_numbers) {
            std::printf(" spr%u=%llu", number, static_cast<unsigned long long>(count));
        }
        std::printf("\nmtspr:");
        for (const auto& [number, count] : mtspr_numbers) {
            std::printf(" spr%u=%llu", number, static_cast<unsigned long long>(count));
        }
        std::printf("\n\ncontrol flow:\n");
        for (const auto& [kind, samples] : flow) {
            std::printf("  %-40s %9llu  e.g.", kind.c_str(),
                        static_cast<unsigned long long>(samples.count));
            for (size_t i = 0; i < std::min<size_t>(3, samples.addresses.size()); ++i) {
                std::printf(" %s", hex(samples.addresses[i]).c_str());
            }
            std::printf("\n");
        }
        std::printf("\ncode addresses referenced by relocations:\n");
        for (const auto& [kind, samples] : code_refs) {
            std::printf("  %-48s %9llu\n", kind.c_str(),
                        static_cast<unsigned long long>(samples.count));
        }
        if (invalid.count) {
            std::printf("\ninvalid word samples:");
            for (uint32_t address : invalid.addresses) {
                std::printf(" %s=%s", hex(address).c_str(),
                            hex(rpx::read_be32(text.data.data() +
                                               (address - text.address)))
                                .c_str());
            }
            std::printf("\n");
        }

        if (argc == 3) {
            std::ofstream out(argv[2]);
            out << "{\n  \"functions\": " << functions.functions().size()
                << ",\n  \"words\": " << words
                << ",\n  \"invalid_words\": " << invalid.count
                << ",\n  \"mnemonics\": {";
            bool first = true;
            for (const auto& [count, name] : sorted) {
                out << (first ? "\n" : ",\n") << "    \"" << name
                    << "\": " << count;
                first = false;
            }
            out << "\n  },\n  \"control_flow\": {";
            first = true;
            for (const auto& [kind, samples] : flow) {
                out << (first ? "\n" : ",\n") << "    \"" << kind
                    << "\": " << samples.count;
                first = false;
            }
            out << "\n  },\n  \"code_references\": {";
            first = true;
            for (const auto& [kind, samples] : code_refs) {
                out << (first ? "\n" : ",\n") << "    \"" << kind
                    << "\": " << samples.count;
                first = false;
            }
            out << "\n  }\n}\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
