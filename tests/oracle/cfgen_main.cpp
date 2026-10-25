// cafe-cfgen: assemble small PowerPC programs that exercise control flow
// (every BO/BI form, CTR loops, calls, indirect and tail calls, switch
// tables, alternate entries, fall-through, reservations, traps), run them
// through the code generator, and emit C++ plus the raw words for the oracle.

#include "codegen.h"
#include "program.h"
#include "rpx.h"

#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace cafe;

namespace {

constexpr uint32_t kCodeBase = 0x00060000;

uint32_t spr_field(uint32_t spr) { return ((spr & 31) << 16) | (((spr >> 5) & 31) << 11); }

struct Asm {
    std::vector<uint32_t> words;
    std::vector<uint32_t> function_starts;
    std::map<std::string, uint32_t> labels;
    struct Fixup { size_t index; std::string label; char kind; }; // b, c(bc), h(ha), l(lo)
    std::vector<Fixup> fixups;
    std::vector<std::pair<uint32_t, uint32_t>> relocations; // ADDR16 site -> target (type in high bit)

    uint32_t here() const { return kCodeBase + static_cast<uint32_t>(words.size()) * 4; }
    void emit(uint32_t w) { words.push_back(w); }
    void label(const std::string& name) { labels[name] = here(); }
    void function(const std::string& name) {
        function_starts.push_back(here());
        label(name);
    }

    // Instruction encoders (operand order as in assembly).
    void addi(uint32_t d, uint32_t a, int32_t v) { emit((14u << 26) | (d << 21) | (a << 16) | (v & 0xFFFF)); }
    void li(uint32_t d, int32_t v) { addi(d, 0, v); }
    void cmpwi(uint32_t crf, uint32_t a, int32_t v) { emit((11u << 26) | (crf << 23) | (a << 16) | (v & 0xFFFF)); }
    void cmplwi(uint32_t crf, uint32_t a, uint32_t v) { emit((10u << 26) | (crf << 23) | (a << 16) | (v & 0xFFFF)); }
    void cmpw(uint32_t crf, uint32_t a, uint32_t b) { emit((31u << 26) | (crf << 23) | (a << 16) | (b << 11)); }
    void add(uint32_t d, uint32_t a, uint32_t b) { emit((31u << 26) | (d << 21) | (a << 16) | (b << 11) | (266u << 1)); }
    void rlwinm(uint32_t a, uint32_t s, uint32_t sh, uint32_t mb, uint32_t me) {
        emit((21u << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1));
    }
    void mtspr(uint32_t spr, uint32_t s) { emit((31u << 26) | (s << 21) | spr_field(spr) | (467u << 1)); }
    void mfspr(uint32_t d, uint32_t spr) { emit((31u << 26) | (d << 21) | spr_field(spr) | (339u << 1)); }
    void mtctr(uint32_t s) { mtspr(9, s); }
    void mtlr(uint32_t s) { mtspr(8, s); }
    void mflr(uint32_t d) { mfspr(d, 8); }
    void mfcr(uint32_t d) { emit((31u << 26) | (d << 21) | (19u << 1)); }
    void blr() { emit(0x4E800020); }
    void bclr(uint32_t bo, uint32_t bi, bool lk) { emit((19u << 26) | (bo << 21) | (bi << 16) | (16u << 1) | lk); }
    void bcctr(uint32_t bo, uint32_t bi, bool lk) { emit((19u << 26) | (bo << 21) | (bi << 16) | (528u << 1) | lk); }
    void b(const std::string& target, bool lk = false) {
        fixups.push_back({words.size(), target, 'b'});
        emit((18u << 26) | lk);
    }
    void bc(uint32_t bo, uint32_t bi, const std::string& target, bool lk = false) {
        fixups.push_back({words.size(), target, 'c'});
        emit((16u << 26) | (bo << 21) | (bi << 16) | lk);
    }
    // lis/addi of a label, with relocations as the game's linker emits them.
    void load_address(uint32_t d, const std::string& target) {
        fixups.push_back({words.size(), target, 'h'});
        emit((15u << 26) | (d << 21));
        fixups.push_back({words.size(), target, 'l'});
        emit((14u << 26) | (d << 21) | (d << 16));
    }
    void lwarx(uint32_t d, uint32_t a, uint32_t b) { emit((31u << 26) | (d << 21) | (a << 16) | (b << 11) | (20u << 1)); }
    void stwcx(uint32_t s, uint32_t a, uint32_t b) { emit((31u << 26) | (s << 21) | (a << 16) | (b << 11) | (150u << 1) | 1); }
    void twi(uint32_t to, uint32_t a, int32_t v) { emit((3u << 26) | (to << 21) | (a << 16) | (v & 0xFFFF)); }
    void tw(uint32_t to, uint32_t a, uint32_t b) { emit((31u << 26) | (to << 21) | (a << 16) | (b << 11) | (4u << 1)); }
    void dcbz(uint32_t a, uint32_t b) { emit((31u << 26) | (a << 16) | (b << 11) | (1014u << 1)); }

    void resolve() {
        for (const Fixup& f : fixups) {
            const uint32_t target = labels.at(f.label);
            const uint32_t site = kCodeBase + static_cast<uint32_t>(f.index) * 4;
            uint32_t& w = words[f.index];
            switch (f.kind) {
            case 'b': w |= (target - site) & 0x03FFFFFC; break;
            case 'c': w |= (target - site) & 0xFFFC; break;
            case 'h':
                w |= ((target + 0x8000) >> 16) & 0xFFFF;
                relocations.push_back({site + 2, target | 0x80000000u});
                break;
            case 'l':
                w |= target & 0xFFFF;
                relocations.push_back({site + 2, target});
                break;
            }
        }
    }
};

// Input constraints the driver applies before running a case.
struct Case {
    std::string name;
    std::string entry;
    uint32_t r4_lo = 0, r4_hi = 0; // r4 range when hi > lo
    uint32_t r5_lo = 0, r5_hi = 0;
    bool r4_address = false; // r4 (and r8) point at aligned data
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s OUTPUT.cpp\n", argv[0]);
        return 2;
    }
    Asm as;
    std::vector<Case> cases;
    int serial = 0;
    const auto fresh = [&](const std::string& stem) { return std::format("{}_{}", stem, serial++); };

    const uint32_t bos[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A,
                            0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14};
    const uint32_t bis[] = {0, 2, 3, 6, 31};

    // Conditional branch, every BO form; result in r3 tells which way it went.
    for (const uint32_t bo : bos) {
        for (const uint32_t bi : bis) {
            const std::string f = fresh("bc"), taken = f + "_taken";
            as.function(f);
            as.li(3, 0);
            as.bc(bo, bi, taken);
            as.li(3, 1);
            as.blr();
            as.label(taken);
            as.li(3, 2);
            as.blr();
            cases.push_back({std::format("bc bo={:#04x} bi={}", bo, bi), f});
        }
    }
    // Conditional call: LR is written whether or not the branch is taken.
    for (const uint32_t bo : bos) {
        for (const uint32_t bi : {2u, 31u}) {
            const std::string f = fresh("bcl"), callee = f + "_callee";
            as.function(f);
            as.mflr(31);
            as.bc(bo, bi, callee, true);
            as.mflr(4);
            as.mtlr(31);
            as.blr();
            as.function(callee);
            as.mflr(5);
            as.li(3, 7);
            as.blr();
            cases.push_back({std::format("bcl bo={:#04x} bi={}", bo, bi), f});
        }
    }
    // Conditional return, including CTR-decrementing forms.
    for (const uint32_t bo : bos) {
        for (const uint32_t bi : bis) {
            const std::string f = fresh("bclr");
            as.function(f);
            as.li(3, 5);
            as.bclr(bo, bi, false);
            as.li(3, 99);
            as.blr();
            cases.push_back({std::format("bclr bo={:#04x} bi={}", bo, bi), f});
        }
    }
    // Conditional indirect tail call / call through CTR (no CTR decrement).
    for (const uint32_t bo : {0x04u, 0x05u, 0x06u, 0x0Cu, 0x0Du, 0x0Eu, 0x14u}) {
        for (const uint32_t bi : bis) {
            const std::string f = fresh("bcctr"), callee = f + "_callee";
            as.function(f);
            as.load_address(12, callee);
            as.mtctr(12);
            as.li(3, 1);
            as.bcctr(bo, bi, false);
            as.li(3, 2);
            as.blr();
            as.function(callee);
            as.li(3, 3);
            as.blr();
            cases.push_back({std::format("bcctr bo={:#04x} bi={}", bo, bi), f});

            const std::string g = fresh("bcctrl"), gcallee = g + "_callee";
            as.function(g);
            as.mflr(31);
            as.load_address(12, gcallee);
            as.mtctr(12);
            as.li(3, 1);
            as.bcctr(bo, bi, true);
            as.mflr(6);
            as.mtlr(31);
            as.blr();
            as.function(gcallee);
            as.addi(3, 3, 40);
            as.blr();
            cases.push_back({std::format("bcctrl bo={:#04x} bi={}", bo, bi), g});
        }
    }
    // Loops on CTR, with and without a condition.
    for (const uint32_t bo : {0x00u, 0x02u, 0x08u, 0x0Au, 0x10u, 0x12u}) {
        const std::string f = fresh("loop"), top = f + "_top";
        as.function(f);
        as.mtctr(4);
        as.li(3, 0);
        as.label(top);
        as.addi(3, 3, 3);
        as.cmpw(0, 3, 5);
        as.bc(bo, 1, top); // BI=1: cr0.gt
        as.blr();
        cases.push_back({std::format("ctr loop bo={:#04x}", bo), f, 1, 40, 0, 120});
    }
    {
        // Direct call and return; LR afterwards is the call's return address.
        const std::string f = fresh("call"), callee = f + "_callee";
        as.function(f);
        as.mflr(31);
        as.li(3, 5);
        as.b(callee, true);
        as.mflr(6);
        as.mtlr(31);
        as.blr();
        as.function(callee);
        as.addi(3, 3, 10);
        as.blr();
        cases.push_back({"call/return", f});
    }
    {
        // bcl 20,31,$+4: the position-reading idiom.
        const std::string f = fresh("pc"), next = f + "_next";
        as.function(f);
        as.mflr(31);
        as.bc(20, 31, next, true);
        as.label(next);
        as.mflr(4);
        as.mtlr(31);
        as.blr();
        cases.push_back({"bcl $+4", f});
    }
    {
        // Indirect call through LR.
        const std::string f = fresh("blrl"), callee = f + "_callee";
        as.function(f);
        as.mflr(31);
        as.load_address(12, callee);
        as.mtlr(12);
        as.bclr(20, 0, true);
        as.mflr(7);
        as.mtlr(31);
        as.blr();
        as.function(callee);
        as.li(3, 42);
        as.blr();
        cases.push_back({"blrl", f});
    }
    {
        // Direct and indirect tail calls: the callee returns to our caller.
        const std::string f = fresh("tail"), callee = f + "_callee";
        as.function(f);
        as.li(3, 1);
        as.b(callee);
        as.function(callee);
        as.addi(3, 3, 2);
        as.blr();
        cases.push_back({"tail call", f});
        const std::string g = fresh("tailctr"), gcallee = g + "_callee";
        as.function(g);
        as.load_address(12, gcallee);
        as.mtctr(12);
        as.bcctr(20, 0, false);
        as.function(gcallee);
        as.li(3, 9);
        as.blr();
        cases.push_back({"indirect tail call", g});
    }
    {
        // Switch: lis/addi of an in-function table of branches, as GHS emits.
        const std::string f = fresh("switch"), table = f + "_table", dflt = f + "_default";
        as.function(f);
        as.cmplwi(0, 4, 3);
        as.bc(12, 1, dflt); // bgt
        as.load_address(12, table);
        as.rlwinm(0, 4, 2, 0, 29);
        as.add(12, 12, 0);
        as.mtctr(12);
        as.bcctr(20, 0, false);
        as.label(table);
        for (int i = 0; i < 4; ++i) as.b(std::format("{}_case{}", f, i));
        for (int i = 0; i < 4; ++i) {
            as.label(std::format("{}_case{}", f, i));
            as.li(3, 10 + i);
            as.blr();
        }
        as.label(dflt);
        as.li(3, 99);
        as.blr();
        cases.push_back({"switch table", f, 0, 6});
    }
    {
        // Entering another function mid-body (save/restore-helper style).
        const std::string f = fresh("alt"), callee = f + "_callee", mid = f + "_mid";
        as.function(f);
        as.mflr(31);
        as.li(3, 0);
        as.b(mid, true);
        as.mtlr(31);
        as.blr();
        as.function(callee);
        as.addi(3, 3, 100);
        as.addi(3, 3, 10);
        as.label(mid);
        as.addi(3, 3, 1);
        as.blr();
        cases.push_back({"alternate entry", f});
    }
    {
        // A body without a final branch runs into the next one.
        const std::string f = fresh("fall"), next = f + "_next";
        as.function(f);
        as.li(3, 1);
        as.function(next);
        as.addi(3, 3, 2);
        as.blr();
        cases.push_back({"fall through", f});
    }
    {
        // Reservations: a paired lwarx/stwcx. succeeds once; an unpaired or
        // mismatched stwcx. fails.
        const std::string f = fresh("rsv");
        as.function(f);
        as.lwarx(5, 0, 4);
        as.addi(5, 5, 1);
        as.stwcx(5, 0, 4);
        as.mfcr(6);
        as.stwcx(5, 0, 4);
        as.mfcr(7);
        as.lwarx(9, 0, 4);
        as.stwcx(9, 0, 8);
        as.mfcr(10);
        as.blr();
        cases.push_back({"lwarx/stwcx.", f, 0, 0, 0, 0, true});
    }
    // Traps: twi/tw for every TO, against random and equal operands.
    for (uint32_t to = 1; to < 32; ++to) {
        const std::string f = fresh("trap");
        as.function(f);
        as.twi(to, 4, 7);
        as.tw(to, 4, 5);
        as.li(3, 1);
        as.blr();
        cases.push_back({std::format("tw/twi to={}", to), f, 5, 9, 5, 9});
    }
    {
        const std::string f = fresh("dcbz");
        as.function(f);
        as.dcbz(4, 5);
        as.dcbz(0, 4);
        as.blr();
        cases.push_back({"dcbz", f, 0, 0, 0, 0x60, true});
    }
    as.resolve();

    // One image holding every program, with the relocations the switch uses.
    rpx::Image image;
    image.sections.emplace_back();
    rpx::Section text;
    text.index = 1;
    text.name = ".text";
    text.type = rpx::kShtProgbits;
    text.flags = rpx::kShfAlloc | rpx::kShfExecinstr;
    text.address = kCodeBase;
    text.data.resize(as.words.size() * 4);
    for (size_t i = 0; i < as.words.size(); ++i) rpx::write_be32(text.data.data() + i * 4, as.words[i]);
    text.size = static_cast<uint32_t>(text.data.size());
    image.sections.push_back(std::move(text));
    for (const auto& [site, target] : as.relocations) {
        image.symbols.push_back({"", target & 0x7FFFFFFF, 0, rpx::kSttFunc, 0, 1});
        image.relocations.push_back({site, (target & 0x80000000u) ? rpx::kRPpcAddr16Ha : rpx::kRPpcAddr16Lo,
                                     static_cast<uint32_t>(image.symbols.size() - 1), 0, 1});
    }
    std::vector<recomp::FunctionInfo> bodies;
    for (size_t i = 0; i < as.function_starts.size(); ++i) {
        const uint32_t start = as.function_starts[i];
        const uint32_t end = i + 1 < as.function_starts.size() ? as.function_starts[i + 1] : as.here();
        bodies.push_back({start, end - start, false, {}, {}, {}});
    }
    const recomp::Program program = recomp::Program::analyze(std::move(image), std::move(bodies));

    std::string out = "// Generated by cafe-cfgen.\n#include \"cafe/ppc_ops.h\"\n\nnamespace ppc = ::cafe::ppc;\n\n";
    std::set<std::string> referenced;
    for (const auto& [address, entry] : program.entries) {
        out += std::format("PPC_WEAK_ALIAS({});\n", recomp::function_name(address));
    }
    for (const auto& [address, entry] : program.entries) {
        out += recomp::emit_entry(program, entry, recomp::EmitOptions{true}, referenced);
    }
    out += "#include \"control_flow_cases.h\"\n\n";
    out += std::format("extern const uint32_t kCodeBase = 0x{:08X}u;\n", kCodeBase);
    out += "extern const uint32_t kCodeWords[] = {";
    for (size_t i = 0; i < as.words.size(); ++i) out += std::format("{}0x{:08X}u,", i % 8 ? " " : "\n\t", as.words[i]);
    out += std::format("\n}};\nextern const size_t kCodeWordCount = {};\n\n", as.words.size());
    out += "extern const GuestEntry kGuestEntries[] = {\n";
    for (const auto& [address, entry] : program.entries) {
        out += std::format("\t{{0x{:08X}u, {}}},\n", address, recomp::function_name(address));
    }
    out += std::format("}};\nextern const size_t kGuestEntryCount = {};\n\n", program.entries.size());
    out += "extern const ControlFlowCase kControlFlowCases[] = {\n";
    for (const Case& c : cases) {
        const uint32_t entry = as.labels.at(c.entry);
        out += std::format("\t{{\"{}\", 0x{:08X}u, {}, {}u, {}u, {}u, {}u, {}}},\n", c.name, entry,
                           recomp::function_name(entry), c.r4_lo, c.r4_hi, c.r5_lo, c.r5_hi, c.r4_address);
    }
    out += std::format("}};\nextern const size_t kControlFlowCaseCount = {};\n", cases.size());
    std::ofstream(argv[1], std::ios::binary) << out;
    std::printf("%zu programs, %zu words, %zu generated entries\n", cases.size(), as.words.size(),
                program.entries.size());
    return 0;
}
