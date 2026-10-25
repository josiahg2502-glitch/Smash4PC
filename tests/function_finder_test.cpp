// Function bodies recovered from a stripped image (no function symbols).

#include "program.h"

#include <cstdio>
#include <cstdlib>

using namespace cafe;

namespace {

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

} // namespace

int main() {
    const uint32_t base = 0x02000000;
    const uint32_t words[] = {
        0x48000011, // 00 bl 0x10
        0x4E800020, // 04 blr
        0x00000000, // 08 padding
        0x00000000, // 0C padding
        0x38630018, // 10 addi r3, r3, lo(0x18): a callback address
        0x4E800020, // 14 blr
        0x38600000, // 18 li r3, 0 (only reached through the callback)
        0x4E800020, // 1C blr
    };
    rpx::Image image;
    image.entry_point = base;
    image.sections.emplace_back();
    rpx::Section text;
    text.index = 1;
    text.name = ".text";
    text.type = rpx::kShtProgbits;
    text.flags = rpx::kShfAlloc | rpx::kShfExecinstr;
    text.address = base;
    for (const uint32_t w : words) {
        text.data.resize(text.data.size() + 4);
        rpx::write_be32(text.data.data() + text.data.size() - 4, w);
    }
    text.size = static_cast<uint32_t>(text.data.size());
    image.sections.push_back(std::move(text));
    image.symbols.push_back({});
    image.symbols.push_back({".text", base, 0, rpx::kSttSection, 0, 1});
    image.relocations.push_back({base + 0x12, rpx::kRPpcAddr16Lo, 1, 0x18, 1});

    const recomp::Program program = recomp::Program::analyze(std::move(image));
    expect(program.functions.size() == 3, "three bodies");
    expect(program.functions[0].address == base && program.functions[0].size == 8,
           "entry body ends before the padding");
    expect(program.functions[1].address == base + 0x10 && program.functions[1].size == 8,
           "bl target body");
    expect(program.functions[2].address == base + 0x18 && program.functions[2].size == 8,
           "callback body split off");
    expect(program.entries.at(base + 0x18).reachable, "callback reachable");
    std::puts("function_finder_test: ok");
    return 0;
}
