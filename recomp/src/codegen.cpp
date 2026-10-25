#include "codegen.h"

#include "mid_hooks.h"
#include "ppc.h"
#include "text.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace cafe::recomp {
namespace {

using ppc::Op;

std::string hex(uint32_t v) { return text::format("0x{:08X}u", v); }

uint32_t rotate_mask(uint32_t mb, uint32_t me) {
    const uint32_t begin = 0xFFFFFFFFu >> mb;
    const uint32_t end = me < 31 ? 0xFFFFFFFFu >> (me + 1) : 0;
    const uint32_t m = begin ^ end;
    return mb <= me ? m : ~m;
}

class Emitter {
public:
    Emitter(const Program& program, const FunctionInfo& function,
            const EmitOptions& options, std::set<std::string>& referenced)
        : program_(program), function_(function), options_(options),
          referenced_(referenced) {}

    std::string run(const Entry& entry) {
        const std::string name = function_name(entry.address);
        out_ += text::format("// {} body {:08X}+{:X}{}\n", name, function_.address,
                             function_.size,
                             entry.address != function_.address ? " (alternate entry)"
                                                                 : "");
        out_ += text::format("PPC_FUNC({}_orig) {{\n", name);
        if (entry.address != function_.address) {
            line(text::format("goto loc_{:08X};", entry.address));
        }
        const uint32_t end = function_.address + function_.size;
        for (uint32_t a = function_.address; a < end; a += 4) {
            const uint32_t w = program_.word(a);
            if (function_.labels.count(a)) {
                out_ += text::format("loc_{:08X}:\n", a);
            }
            if (options_.comments) {
                out_ += text::format("\t// {:08X} {:08X}  {}\n", a, w,
                                     ppc::disassemble(w, a));
            }
            if (options_.mid_hooks && std::find(std::begin(kMidHooks), std::end(kMidHooks), a) != std::end(kMidHooks)) {
                referenced_.insert(mid_hook_name(a));
                line(text::format("{}(ctx, base);", mid_hook_name(a)));
            }
            instruction(a, w);
        }
        if (program_.falls_off_end(function_)) {
            if (program_.entries.count(end)) {
                line(text::format("PPC_TAIL_CALL({});", callee(end)));
            } else {
                line(text::format("cafe_ppc_illegal(ctx, {}, 0, \"ran off the end of a "
                                  "function\");",
                                  hex(end)));
            }
        }
        out_ += "}\n\n";
        return std::move(out_);
    }

private:
    void line(const std::string& text) { out_ += "\t" + text + "\n"; }

    static std::string r(uint32_t n) { return text::format("ctx.r[{}]", n); }
    static std::string f0(uint32_t n) { return text::format("ctx.f[{}].ps0", n); }
    static std::string f1(uint32_t n) { return text::format("ctx.f[{}].ps1", n); }
    static std::string crf(uint32_t n) { return text::format("ctx.cr[{}]", n); }
    static std::string crbit(uint32_t bit) {
        static const char* names[] = {"lt", "gt", "eq", "so"};
        return text::format("ctx.cr[{}].{}", bit / 4, names[bit % 4]);
    }
    static std::string signed_offset(int32_t value) {
        return value < 0 ? text::format(" - {}", -int64_t{value})
                         : text::format(" + {}", value);
    }
    // D-form effective address: (rA|0) + d.
    static std::string ea_d(uint32_t ra, int32_t d) {
        if (ra == 0) {
            return hex(static_cast<uint32_t>(d));
        }
        return d == 0 ? r(ra) : r(ra) + signed_offset(d);
    }
    // X-form effective address: (rA|0) + rB.
    static std::string ea_x(uint32_t ra, uint32_t rb) {
        return ra == 0 ? r(rb) : r(ra) + " + " + r(rb);
    }

    void record(const std::string& value) {
        line(text::format("ppc::cmp_signed(ctx.cr[0], static_cast<int32_t>({}), 0, "
                          "ctx.xer_so);",
                          value));
    }

    void illegal(uint32_t a, uint32_t w, const char* reason) {
        line(text::format("cafe_ppc_illegal(ctx, {}, {}, \"{}\");", hex(a), hex(w),
                          reason));
    }

    // C call target for a guest address, or empty when there is none.
    std::string callee(uint32_t target) {
        if (program_.import_symbols.count(target) || program_.entries.count(target)) {
            std::string symbol = program_.symbol_for(target);
            referenced_.insert(symbol);
            return symbol;
        }
        return {};
    }

    // Branch condition from BO/BI. Emits the CTR decrement when BO asks.
    std::string condition(uint32_t bo, uint32_t bi) {
        std::string cond;
        if (!(bo & 4)) {
            line("--ctx.ctr;");
            cond = (bo & 2) ? "ctx.ctr == 0" : "ctx.ctr != 0";
        }
        if (!(bo & 0x10)) {
            const std::string bit = (bo & 8) ? crbit(bi) : "!" + crbit(bi);
            cond = cond.empty() ? bit : cond + " && " + bit;
        }
        return cond;
    }
    void guarded(const std::string& cond, const std::string& body) {
        if (cond.empty()) {
            line(body);
        } else {
            line(text::format("if ({}) {{ {} }}", cond, body));
        }
    }

    void branch(uint32_t a, uint32_t w, Op op) {
        const uint32_t bo = op == Op::b ? 0x14 : ppc::rd(w);
        const uint32_t target = program_.branch_destination(a, w);
        const bool link = ppc::lk(w);
        const uint32_t end = function_.address + function_.size;
        const bool local = target >= function_.address && target < end;
        // LR is written whether or not a conditional branch is taken.
        if (link) {
            line(text::format("ctx.lr = {};", hex(a + 4)));
            if (target == a + 4) {
                condition(bo, ppc::ra(w));
                return;
            }
        }
        const std::string cond = condition(bo, ppc::ra(w));
        if (!link && local) {
            guarded(cond, text::format("goto loc_{:08X};", target));
            return;
        }
        const std::string symbol = callee(target);
        std::string body;
        if (target == 0) {
            body = text::format("cafe_ppc_null_call(ctx, {});", hex(a));
        } else if (symbol.empty()) {
            body = text::format("cafe_ppc_illegal(ctx, {}, {}, \"branch to code with no "
                                "entry\");",
                                hex(a), hex(w));
        } else if (link) {
            body = text::format("{}(ctx, base); PPC_CHECK_RETURN({});", symbol, hex(a + 4));
        } else {
            body = text::format("PPC_TAIL_CALL({});", symbol);
        }
        guarded(cond, body);
    }

    void branch_register(uint32_t a, uint32_t w, Op op) {
        const uint32_t bo = ppc::rd(w);
        const bool link = ppc::lk(w);
        if (op == Op::bclr) {
            if (link) {
                line("{");
                line(text::format("\tconst uint32_t target = ctx.lr; ctx.lr = {};", hex(a + 4)));
                const std::string cond = condition(bo, ppc::ra(w));
                guarded(cond, text::format("PPC_CALL_INDIRECT(target); PPC_CHECK_RETURN({});",
                                           hex(a + 4)));
                line("}");
            } else {
                guarded(condition(bo, ppc::ra(w)), "return;");
            }
            return;
        }
        // bcctr: BO must not ask for a CTR decrement.
        if (!(bo & 4)) {
            illegal(a, w, "bcctr with CTR decrement");
            return;
        }
        if (link) {
            line(text::format("ctx.lr = {};", hex(a + 4)));
            guarded(condition(bo, ppc::ra(w)),
                    text::format("PPC_CALL_INDIRECT(ctx.ctr); PPC_CHECK_RETURN({});", hex(a + 4)));
            return;
        }
        std::string body;
        if (!function_.switch_targets.empty()) {
            body = "switch (ctx.ctr) {";
            for (const uint32_t target : function_.switch_targets) {
                body += text::format(" case {}: goto loc_{:08X};", hex(target), target);
            }
            body += " default: break; } ";
        }
        body += "PPC_TAIL_CALL_INDIRECT(ctx.ctr);";
        guarded(condition(bo, ppc::ra(w)), body);
    }

    // XO-form add/subtract family: rD = x + y + carry_in.
    void add_family(uint32_t w, const std::string& x, const std::string& y,
                    const std::string& carry_in, bool sets_ca) {
        const uint32_t d = ppc::rd(w);
        const bool oe = ppc::oe(w);
        if (!sets_ca && !oe) {
            // Plain forms: add, subf, neg.
            if (carry_in == "0") {
                line(text::format("{} = {} + {};", r(d), x, y));
            } else if (y == "0u") {
                line(text::format("{} = -{};", r(d), r(ppc::ra(w))));
            } else {
                line(text::format("{} = {} - {};", r(d), y, r(ppc::ra(w))));
            }
        } else {
            line("{");
            line(text::format("\tconst uint32_t x = {}, y = {};", x, y));
            line(text::format("\tconst uint64_t t = uint64_t{{x}} + y + {};", carry_in));
            line(text::format("\t{} = static_cast<uint32_t>(t);", r(d)));
            if (sets_ca) {
                line("\tctx.xer_ca = static_cast<uint8_t>(t >> 32);");
            }
            if (oe) {
                line(text::format("\tctx.xer_ov = (((x ^ {0}) & (y ^ {0})) >> 31) != 0;", r(d)));
                line("\tctx.xer_so |= ctx.xer_ov;");
            }
            line("}");
        }
        if (ppc::rc(w)) {
            record(r(d));
        }
    }

    void fp_unary_bits(uint32_t w, const char* op) {
        line(text::format("{} = ppc::from_bits(ppc::bits({}) {});", f0(ppc::rd(w)),
                          f0(ppc::rb(w)), op));
    }
    void fill(uint32_t frd, const std::string& value) {
        line(text::format("{{ const double v = {}; {} = v; {} = v; }}", value, f0(frd),
                          f1(frd)));
    }
    void pair(uint32_t frd, const std::string& p0, const std::string& p1) {
        line(text::format("{{ const double p0 = {}; const double p1 = {}; {} = p0; {} = p1; }}",
                          p0, p1, f0(frd), f1(frd)));
    }

    void instruction(uint32_t a, uint32_t w) {
        const Op op = ppc::decode(w);
        const uint32_t d = ppc::rd(w); // also rS, frD, frS, BO, TO
        const uint32_t ra = ppc::ra(w);
        const uint32_t rb = ppc::rb(w);
        const uint32_t rc = ppc::frc(w);
        const int32_t simm = ppc::simm(w);
        const uint32_t uimm = ppc::uimm(w);

        // Record forms of FP instructions copy FPSCR bits we do not model.
        if (ppc::has_record_form(op) && ppc::rc(w) && op >= Op::fadd) {
            illegal(a, w, "floating-point record form");
            return;
        }

        switch (op) {
        // ---------------------------------------------------------- integer
        case Op::addi:
            line(text::format("{} = {};", r(d),
                              ra ? r(ra) + signed_offset(simm) : hex(static_cast<uint32_t>(simm))));
            return;
        case Op::addis: {
            const uint32_t v = static_cast<uint32_t>(simm) << 16;
            line(text::format("{} = {};", r(d), ra ? text::format("{} + {}", r(ra), hex(v)) : hex(v)));
            return;
        }
        case Op::addic:
        case Op::addic_rc:
            line("{");
            line(text::format("\tconst uint32_t x = {};", r(ra)));
            line(text::format("\t{} = x + {};", r(d), hex(static_cast<uint32_t>(simm))));
            line(text::format("\tctx.xer_ca = {} < x;", r(d)));
            line("}");
            if (op == Op::addic_rc) {
                record(r(d));
            }
            return;
        case Op::subfic:
            line("{");
            line(text::format("\tconst uint64_t t = uint64_t{{~{}}} + {} + 1;", r(ra),
                              hex(static_cast<uint32_t>(simm))));
            line(text::format("\t{} = static_cast<uint32_t>(t);", r(d)));
            line("\tctx.xer_ca = static_cast<uint8_t>(t >> 32);");
            line("}");
            return;
        case Op::mulli:
            line(text::format("{} = {} * {};", r(d), r(ra), hex(static_cast<uint32_t>(simm))));
            return;
        case Op::add: add_family(w, r(ra), r(rb), "0", false); return;
        case Op::addc: add_family(w, r(ra), r(rb), "0", true); return;
        case Op::adde: add_family(w, r(ra), r(rb), "ctx.xer_ca", true); return;
        case Op::addze: add_family(w, r(ra), "0u", "ctx.xer_ca", true); return;
        case Op::addme: add_family(w, r(ra), "0xFFFFFFFFu", "ctx.xer_ca", true); return;
        case Op::subf: add_family(w, "~" + r(ra), r(rb), "1", false); return;
        case Op::subfc: add_family(w, "~" + r(ra), r(rb), "1", true); return;
        case Op::subfe: add_family(w, "~" + r(ra), r(rb), "ctx.xer_ca", true); return;
        case Op::subfze: add_family(w, "~" + r(ra), "0u", "ctx.xer_ca", true); return;
        case Op::subfme: add_family(w, "~" + r(ra), "0xFFFFFFFFu", "ctx.xer_ca", true); return;
        case Op::neg: add_family(w, "~" + r(ra), "0u", "1", false); return;
        case Op::mullw:
            if (ppc::oe(w)) {
                line("{");
                line(text::format("\tconst int64_t p = int64_t{{static_cast<int32_t>({})}} * "
                                  "static_cast<int32_t>({});",
                                  r(ra), r(rb)));
                line(text::format("\t{} = static_cast<uint32_t>(p);", r(d)));
                line("\tctx.xer_ov = p != static_cast<int32_t>(p);");
                line("\tctx.xer_so |= ctx.xer_ov;");
                line("}");
            } else {
                line(text::format("{} = {} * {};", r(d), r(ra), r(rb)));
            }
            if (ppc::rc(w)) record(r(d));
            return;
        case Op::mulhw:
            line(text::format("{} = static_cast<uint32_t>((int64_t{{static_cast<int32_t>({})}} * "
                              "static_cast<int32_t>({})) >> 32);",
                              r(d), r(ra), r(rb)));
            if (ppc::rc(w)) record(r(d));
            return;
        case Op::mulhwu:
            line(text::format("{} = static_cast<uint32_t>((uint64_t{{{}}} * {}) >> 32);", r(d),
                              r(ra), r(rb)));
            if (ppc::rc(w)) record(r(d));
            return;
        case Op::divw:
        case Op::divwu: {
            const char* fn = op == Op::divw ? "divw" : "divwu";
            if (ppc::oe(w)) {
                line("{");
                line(text::format("\tconst uint32_t x = {}, y = {};", r(ra), r(rb)));
                line(text::format("\tctx.xer_ov = {};", op == Op::divw ? "ppc::divw_overflows(x, y)"
                                                                         : "y == 0"));
                line("\tctx.xer_so |= ctx.xer_ov;");
                line(text::format("\t{} = ppc::{}(x, y);", r(d), fn));
                line("}");
            } else {
                line(text::format("{} = ppc::{}({}, {});", r(d), fn, r(ra), r(rb)));
            }
            if (ppc::rc(w)) record(r(d));
            return;
        }

        case Op::cmp:
            line(text::format("ppc::cmp_signed({}, static_cast<int32_t>({}), "
                              "static_cast<int32_t>({}), ctx.xer_so);",
                              crf(ppc::crfd(w)), r(ra), r(rb)));
            return;
        case Op::cmpl:
            line(text::format("ppc::cmp_unsigned({}, {}, {}, ctx.xer_so);", crf(ppc::crfd(w)),
                              r(ra), r(rb)));
            return;
        case Op::cmpi:
            line(text::format("ppc::cmp_signed({}, static_cast<int32_t>({}), {}, ctx.xer_so);",
                              crf(ppc::crfd(w)), r(ra), simm));
            return;
        case Op::cmpli:
            line(text::format("ppc::cmp_unsigned({}, {}, {}, ctx.xer_so);", crf(ppc::crfd(w)),
                              r(ra), hex(uimm)));
            return;

        case Op::and_: logical(w, "{0} & {1}"); return;
        case Op::andc: logical(w, "{0} & ~{1}"); return;
        case Op::or_:
            if (d == rb) {
                line(text::format("{} = {};", r(ra), r(d))); // mr
                if (ppc::rc(w)) record(r(ra));
            } else {
                logical(w, "{0} | {1}");
            }
            return;
        case Op::orc: logical(w, "{0} | ~{1}"); return;
        case Op::xor_: logical(w, "{0} ^ {1}"); return;
        case Op::nand: logical(w, "~({0} & {1})"); return;
        case Op::nor: logical(w, "~({0} | {1})"); return;
        case Op::eqv: logical(w, "~({0} ^ {1})"); return;
        case Op::andi_rc:
            line(text::format("{} = {} & {};", r(ra), r(d), hex(uimm)));
            record(r(ra));
            return;
        case Op::andis_rc:
            line(text::format("{} = {} & {};", r(ra), r(d), hex(uimm << 16)));
            record(r(ra));
            return;
        case Op::ori:
            if (w == 0x60000000) {
                line(";"); // nop
            } else {
                line(text::format("{} = {} | {};", r(ra), r(d), hex(uimm)));
            }
            return;
        case Op::oris: line(text::format("{} = {} | {};", r(ra), r(d), hex(uimm << 16))); return;
        case Op::xori: line(text::format("{} = {} ^ {};", r(ra), r(d), hex(uimm))); return;
        case Op::xoris: line(text::format("{} = {} ^ {};", r(ra), r(d), hex(uimm << 16))); return;
        case Op::extsb:
            line(text::format("{} = static_cast<uint32_t>(static_cast<int8_t>({}));", r(ra), r(d)));
            if (ppc::rc(w)) record(r(ra));
            return;
        case Op::extsh:
            line(text::format("{} = static_cast<uint32_t>(static_cast<int16_t>({}));", r(ra), r(d)));
            if (ppc::rc(w)) record(r(ra));
            return;
        case Op::cntlzw:
            line(text::format("{} = static_cast<uint32_t>(std::countl_zero({}));", r(ra), r(d)));
            if (ppc::rc(w)) record(r(ra));
            return;

        case Op::rlwinm: {
            const uint32_t m = rotate_mask(ppc::mb(w), ppc::me(w));
            std::string v = rb ? text::format("ppc::rotl({}, {})", r(d), rb) : r(d);
            if (m != 0xFFFFFFFFu) v += " & " + hex(m);
            line(text::format("{} = {};", r(ra), v));
            if (ppc::rc(w)) record(r(ra));
            return;
        }
        case Op::rlwnm: {
            const uint32_t m = rotate_mask(ppc::mb(w), ppc::me(w));
            line(text::format("{} = ppc::rotl({}, {}) & {};", r(ra), r(d), r(rb), hex(m)));
            if (ppc::rc(w)) record(r(ra));
            return;
        }
        case Op::rlwimi: {
            const uint32_t m = rotate_mask(ppc::mb(w), ppc::me(w));
            line(text::format("{} = (ppc::rotl({}, {}) & {}) | ({} & {});", r(ra), r(d), rb,
                              hex(m), r(ra), hex(~m)));
            if (ppc::rc(w)) record(r(ra));
            return;
        }
        case Op::slw:
            line(text::format("{0} = ({2} & 0x20) ? 0u : {1} << ({2} & 31);", r(ra), r(d), r(rb)));
            if (ppc::rc(w)) record(r(ra));
            return;
        case Op::srw:
            line(text::format("{0} = ({2} & 0x20) ? 0u : {1} >> ({2} & 31);", r(ra), r(d), r(rb)));
            if (ppc::rc(w)) record(r(ra));
            return;
        case Op::sraw:
            line(text::format("{} = ppc::sraw(ctx.xer_ca, {}, {} & 0x3F);", r(ra), r(d), r(rb)));
            if (ppc::rc(w)) record(r(ra));
            return;
        case Op::srawi:
            line(text::format("{} = ppc::sraw(ctx.xer_ca, {}, {});", r(ra), r(d), rb));
            if (ppc::rc(w)) record(r(ra));
            return;

        // ------------------------------------------------------ load/store
        case Op::lbz: load(w, "PPC_LOAD_U8({})", false, false); return;
        case Op::lbzu: load(w, "PPC_LOAD_U8({})", true, false); return;
        case Op::lbzx: load(w, "PPC_LOAD_U8({})", false, true); return;
        case Op::lbzux: load(w, "PPC_LOAD_U8({})", true, true); return;
        case Op::lhz: load(w, "PPC_LOAD_U16({})", false, false); return;
        case Op::lhzu: load(w, "PPC_LOAD_U16({})", true, false); return;
        case Op::lhzx: load(w, "PPC_LOAD_U16({})", false, true); return;
        case Op::lhzux: load(w, "PPC_LOAD_U16({})", true, true); return;
        case Op::lha: load(w, kLha, false, false); return;
        case Op::lhau: load(w, kLha, true, false); return;
        case Op::lhax: load(w, kLha, false, true); return;
        case Op::lhaux: load(w, kLha, true, true); return;
        case Op::lwz: load(w, "PPC_LOAD_U32({})", false, false); return;
        case Op::lwzu: load(w, "PPC_LOAD_U32({})", true, false); return;
        case Op::lwzx: load(w, "PPC_LOAD_U32({})", false, true); return;
        case Op::lwzux: load(w, "PPC_LOAD_U32({})", true, true); return;
        case Op::lhbrx: load(w, "ppc::ld16_le(base, {})", false, true); return;
        case Op::lwbrx: load(w, "ppc::ld32_le(base, {})", false, true); return;
        case Op::stb: store(w, "PPC_STORE_U8({}, static_cast<uint8_t>({}));", false, false); return;
        case Op::stbu: store(w, "PPC_STORE_U8({}, static_cast<uint8_t>({}));", true, false); return;
        case Op::stbx: store(w, "PPC_STORE_U8({}, static_cast<uint8_t>({}));", false, true); return;
        case Op::stbux: store(w, "PPC_STORE_U8({}, static_cast<uint8_t>({}));", true, true); return;
        case Op::sth: store(w, "PPC_STORE_U16({}, static_cast<uint16_t>({}));", false, false); return;
        case Op::sthu: store(w, "PPC_STORE_U16({}, static_cast<uint16_t>({}));", true, false); return;
        case Op::sthx: store(w, "PPC_STORE_U16({}, static_cast<uint16_t>({}));", false, true); return;
        case Op::sthux: store(w, "PPC_STORE_U16({}, static_cast<uint16_t>({}));", true, true); return;
        case Op::stw: store(w, "PPC_STORE_U32({}, {});", false, false); return;
        case Op::stwu: store(w, "PPC_STORE_U32({}, {});", true, false); return;
        case Op::stwx: store(w, "PPC_STORE_U32({}, {});", false, true); return;
        case Op::stwux: store(w, "PPC_STORE_U32({}, {});", true, true); return;
        case Op::sthbrx: store(w, "ppc::st16_le(base, {}, static_cast<uint16_t>({}));", false, true); return;
        case Op::stwbrx: store(w, "ppc::st32_le(base, {}, {});", false, true); return;
        case Op::lmw:
            line(text::format("ppc::lmw(ctx, base, {}, {});", d, ea_d(ra, simm)));
            return;
        case Op::stmw:
            line(text::format("ppc::stmw(ctx, base, {}, {});", d, ea_d(ra, simm)));
            return;
        case Op::lswi:
            line(text::format("ppc::lsw(ctx, base, {}, {}, {});", d, ra ? r(ra) : "0u",
                              rb ? rb : 32));
            return;
        case Op::stswi:
            line(text::format("ppc::stsw(ctx, base, {}, {}, {});", d, ra ? r(ra) : "0u",
                              rb ? rb : 32));
            return;
        case Op::lswx:
            line(text::format("ppc::lsw(ctx, base, {}, {}, ctx.xer_bc);", d, ea_x(ra, rb)));
            return;
        case Op::stswx:
            line(text::format("ppc::stsw(ctx, base, {}, {}, ctx.xer_bc);", d, ea_x(ra, rb)));
            return;
        case Op::lwarx:
            line(text::format("{} = ppc::lwarx(ctx, base, {});", r(d), ea_x(ra, rb)));
            return;
        case Op::stwcx:
            line("{");
            line(text::format("\tconst bool stored = ppc::stwcx(ctx, base, {}, {});", ea_x(ra, rb),
                              r(d)));
            line("\tctx.cr[0] = {0, 0, static_cast<uint8_t>(stored), ctx.xer_so};");
            line("}");
            return;

        // ------------------------------------------------------- branches
        case Op::b:
        case Op::bc:
            branch(a, w, op);
            return;
        case Op::bclr:
        case Op::bcctr:
            branch_register(a, w, op);
            return;
        case Op::tw:
        case Op::twi: {
            const std::string x = r(ra);
            const std::string y = op == Op::tw ? r(rb) : hex(static_cast<uint32_t>(simm));
            if (d == 31) {
                line(text::format("cafe_ppc_trap(ctx, {});", hex(a)));
                return;
            }
            std::string cond;
            const auto add = [&](uint32_t bit, const std::string& c) {
                if (d & bit) cond += (cond.empty() ? "" : " || ") + c;
            };
            add(16, text::format("static_cast<int32_t>({}) < static_cast<int32_t>({})", x, y));
            add(8, text::format("static_cast<int32_t>({}) > static_cast<int32_t>({})", x, y));
            add(4, text::format("{} == {}", x, y));
            add(2, text::format("{} < {}", x, y));
            add(1, text::format("{} > {}", x, y));
            if (!cond.empty()) {
                line(text::format("if ({}) cafe_ppc_trap(ctx, {});", cond, hex(a)));
            }
            return;
        }

        // ------------------------------------------- condition register
        case Op::mcrf:
            line(text::format("{} = {};", crf(ppc::crfd(w)), crf(ppc::crfs(w))));
            return;
        case Op::crand: crlogic(w, "{0} & {1}"); return;
        case Op::crandc: crlogic(w, "{0} & !{1}"); return;
        case Op::creqv: crlogic(w, "!({0} ^ {1})"); return;
        case Op::crnand: crlogic(w, "!({0} & {1})"); return;
        case Op::crnor: crlogic(w, "!({0} | {1})"); return;
        case Op::cror: crlogic(w, "{0} | {1}"); return;
        case Op::crorc: crlogic(w, "{0} | !{1}"); return;
        case Op::crxor:
            if (ra == rb) {
                line(text::format("{} = 0;", crbit(d)));
            } else {
                crlogic(w, "{0} ^ {1}");
            }
            return;
        case Op::mfcr:
            line(text::format("{} = ppc::mfcr(ctx);", r(d)));
            return;
        case Op::mtcrf:
            line(text::format("ppc::mtcrf(ctx, {}, {});", hex(ppc::crm(w)), r(d)));
            return;
        case Op::mcrxr:
            line(text::format("{} = {{ctx.xer_so, ctx.xer_ov, ctx.xer_ca, 0}};", crf(ppc::crfd(w))));
            line("ctx.xer_so = ctx.xer_ov = ctx.xer_ca = 0;");
            return;

        // ---------------------------------------------- special registers
        case Op::mfspr: {
            const uint32_t spr = ppc::spr(w);
            std::string v;
            if (spr == 1) v = "ppc::mfxer(ctx)";
            else if (spr == 8) v = "ctx.lr";
            else if (spr == 9) v = "ctx.ctr";
            else if (spr >= 896 && spr < 904) v = text::format("ctx.gqr[{}]", spr - 896);
            else if (spr >= 912 && spr < 920) v = text::format("ctx.gqr[{}]", spr - 912);
            else if (spr == 268) v = "static_cast<uint32_t>(cafe_ppc_timebase())";
            else if (spr == 269) v = "static_cast<uint32_t>(cafe_ppc_timebase() >> 32)";
            if (v.empty()) {
                illegal(a, w, "mfspr of an unsupported register");
            } else {
                line(text::format("{} = {};", r(d), v));
            }
            return;
        }
        case Op::mtspr: {
            const uint32_t spr = ppc::spr(w);
            if (spr == 1) line(text::format("ppc::mtxer(ctx, {});", r(d)));
            else if (spr == 8) line(text::format("ctx.lr = {};", r(d)));
            else if (spr == 9) line(text::format("ctx.ctr = {};", r(d)));
            else if (spr >= 896 && spr < 904) line(text::format("ctx.gqr[{}] = {};", spr - 896, r(d)));
            else if (spr >= 912 && spr < 920) line(text::format("ctx.gqr[{}] = {};", spr - 912, r(d)));
            else illegal(a, w, "mtspr of an unsupported register");
            return;
        }
        case Op::mftb: {
            const uint32_t tbr = ppc::spr(w);
            if (tbr == 268) {
                line(text::format("{} = static_cast<uint32_t>(cafe_ppc_timebase());", r(d)));
            } else if (tbr == 269) {
                line(text::format("{} = static_cast<uint32_t>(cafe_ppc_timebase() >> 32);", r(d)));
            } else {
                illegal(a, w, "mftb of an unknown time-base register");
            }
            return;
        }

        // ---------------------------------------------------- cache/sync
        case Op::sync:
            line("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
            return;
        case Op::isync:
        case Op::eieio:
            line("__atomic_signal_fence(__ATOMIC_SEQ_CST);");
            return;
        case Op::dcbf:
        case Op::dcbi:
        case Op::dcbst:
        case Op::dcbt:
        case Op::dcbtst:
        case Op::icbi:
            line(";"); // caches are coherent on the host
            return;
        case Op::dcbz:
        case Op::dcbz_l:
            line(text::format("ppc::dcbz(base, {});", ea_x(ra, rb)));
            return;

        // ---------------------------------------------- FP load/store
        case Op::lfs: fload(w, true, false, false); return;
        case Op::lfsu: fload(w, true, true, false); return;
        case Op::lfsx: fload(w, true, false, true); return;
        case Op::lfsux: fload(w, true, true, true); return;
        case Op::lfd: fload(w, false, false, false); return;
        case Op::lfdu: fload(w, false, true, false); return;
        case Op::lfdx: fload(w, false, false, true); return;
        case Op::lfdux: fload(w, false, true, true); return;
        case Op::stfs: fstore(w, "PPC_STORE_U32({}, ppc::store_single({}));", false, false); return;
        case Op::stfsu: fstore(w, "PPC_STORE_U32({}, ppc::store_single({}));", true, false); return;
        case Op::stfsx: fstore(w, "PPC_STORE_U32({}, ppc::store_single({}));", false, true); return;
        case Op::stfsux: fstore(w, "PPC_STORE_U32({}, ppc::store_single({}));", true, true); return;
        case Op::stfd: fstore(w, "PPC_STORE_U64({}, ppc::bits({}));", false, false); return;
        case Op::stfdu: fstore(w, "PPC_STORE_U64({}, ppc::bits({}));", true, false); return;
        case Op::stfdx: fstore(w, "PPC_STORE_U64({}, ppc::bits({}));", false, true); return;
        case Op::stfdux: fstore(w, "PPC_STORE_U64({}, ppc::bits({}));", true, true); return;
        case Op::stfiwx:
            fstore(w, "PPC_STORE_U32({}, static_cast<uint32_t>(ppc::bits({})));", false, true);
            return;

        // ---------------------------------------------- FP arithmetic
        case Op::fadd: line(text::format("{} = ppc::add({}, {});", f0(d), f0(ra), f0(rb))); return;
        case Op::fsub: line(text::format("{} = ppc::sub({}, {});", f0(d), f0(ra), f0(rb))); return;
        case Op::fmul: line(text::format("{} = ppc::mul({}, {});", f0(d), f0(ra), f0(rc))); return;
        case Op::fdiv: line(text::format("{} = ppc::div({}, {});", f0(d), f0(ra), f0(rb))); return;
        case Op::fadds: fill(d, text::format("ppc::round_single(ppc::add({}, {}))", f0(ra), f0(rb))); return;
        case Op::fsubs: fill(d, text::format("ppc::round_single(ppc::sub({}, {}))", f0(ra), f0(rb))); return;
        case Op::fmuls:
            fill(d, text::format("ppc::round_single(ppc::mul({}, ppc::round_25bit({})))", f0(ra), f0(rc)));
            return;
        case Op::fdivs: fill(d, text::format("ppc::round_single(ppc::div({}, {}))", f0(ra), f0(rb))); return;
        case Op::fmadd: line(text::format("{} = {};", f0(d), madd(false, false, f0(ra), f0(rc), f0(rb)))); return;
        case Op::fmsub: line(text::format("{} = {};", f0(d), madd(false, true, f0(ra), f0(rc), f0(rb)))); return;
        case Op::fnmadd:
            line(text::format("{} = ppc::negate_unless_nan({});", f0(d), madd(false, false, f0(ra), f0(rc), f0(rb))));
            return;
        case Op::fnmsub:
            line(text::format("{} = ppc::negate_unless_nan({});", f0(d), madd(false, true, f0(ra), f0(rc), f0(rb))));
            return;
        case Op::fmadds: fill(d, "ppc::round_single(" + madd(true, false, f0(ra), f0(rc), f0(rb)) + ")"); return;
        case Op::fmsubs: fill(d, "ppc::round_single(" + madd(true, true, f0(ra), f0(rc), f0(rb)) + ")"); return;
        case Op::fnmadds:
            fill(d, "ppc::negate_unless_nan(ppc::round_single(" + madd(true, false, f0(ra), f0(rc), f0(rb)) + "))");
            return;
        case Op::fnmsubs:
            fill(d, "ppc::negate_unless_nan(ppc::round_single(" + madd(true, true, f0(ra), f0(rc), f0(rb)) + "))");
            return;
        case Op::fsqrt: line(text::format("{} = std::sqrt({});", f0(d), f0(rb))); return;
        case Op::fsqrts: fill(d, text::format("ppc::round_single(std::sqrt({}))", f0(rb))); return;
        case Op::fres: fill(d, text::format("ppc::res({})", f0(rb))); return;
        case Op::frsqrte: line(text::format("{} = ppc::rsqrte({});", f0(d), f0(rb))); return;
        case Op::fsel:
            line(text::format("{} = ppc::fsel({}, {}, {});", f0(d), f0(ra), f0(rb), f0(rc)));
            return;
        case Op::frsp: fill(d, text::format("ppc::round_single({})", f0(rb))); return;
        case Op::fctiw: line(text::format("{} = ppc::fcti({}, false);", f0(d), f0(rb))); return;
        case Op::fctiwz: line(text::format("{} = ppc::fcti({}, true);", f0(d), f0(rb))); return;
        case Op::fmr: line(text::format("{} = {};", f0(d), f0(rb))); return;
        case Op::fneg: fp_unary_bits(w, "^ ppc::kSign"); return;
        case Op::fabs: fp_unary_bits(w, "& ~ppc::kSign"); return;
        case Op::fnabs: fp_unary_bits(w, "| ppc::kSign"); return;
        case Op::fcmpu:
        case Op::fcmpo:
            line(text::format("ppc::fcmp({}, {}, {});", crf(ppc::crfd(w)), f0(ra), f0(rb)));
            return;

        // ------------------------------------------------ paired single
        case Op::psq_l:
        case Op::psq_lu: {
            const std::string ea = ea_d(ra, ppc::psq_d(w));
            psq(true, op == Op::psq_lu, ea, d, ppc::psq_w(w), ppc::psq_i(w), ra);
            return;
        }
        case Op::psq_st:
        case Op::psq_stu: {
            const std::string ea = ea_d(ra, ppc::psq_d(w));
            psq(false, op == Op::psq_stu, ea, d, ppc::psq_w(w), ppc::psq_i(w), ra);
            return;
        }
        case Op::psq_lx:
        case Op::psq_lux:
            psq(true, op == Op::psq_lux, ea_x(ra, rb), d, ppc::psqx_w(w), ppc::psqx_i(w), ra);
            return;
        case Op::psq_stx:
        case Op::psq_stux:
            psq(false, op == Op::psq_stux, ea_x(ra, rb), d, ppc::psqx_w(w), ppc::psqx_i(w), ra);
            return;
        case Op::ps_add: ps2(d, "ppc::round_single(ppc::add({0}, {1}))", ra, rb); return;
        case Op::ps_sub: ps2(d, "ppc::round_single(ppc::sub({0}, {1}))", ra, rb); return;
        case Op::ps_div: ps2(d, "ppc::round_single(ppc::div({0}, {1}))", ra, rb); return;
        case Op::ps_mul: ps2(d, "ppc::round_single(ppc::mul({0}, ppc::round_25bit({1})))", ra, rc); return;
        case Op::ps_madd:
            pair(d, "ppc::round_single(" + madd(true, false, f0(ra), f0(rc), f0(rb)) + ")",
                 "ppc::round_single(" + madd(true, false, f1(ra), f1(rc), f1(rb)) + ")");
            return;
        case Op::ps_msub:
            pair(d, "ppc::round_single(" + madd(true, true, f0(ra), f0(rc), f0(rb)) + ")",
                 "ppc::round_single(" + madd(true, true, f1(ra), f1(rc), f1(rb)) + ")");
            return;
        case Op::ps_nmadd:
            pair(d, "ppc::negate_unless_nan(ppc::round_single(" + madd(true, false, f0(ra), f0(rc), f0(rb)) + "))",
                 "ppc::negate_unless_nan(ppc::round_single(" + madd(true, false, f1(ra), f1(rc), f1(rb)) + "))");
            return;
        case Op::ps_nmsub:
            pair(d, "ppc::negate_unless_nan(ppc::round_single(" + madd(true, true, f0(ra), f0(rc), f0(rb)) + "))",
                 "ppc::negate_unless_nan(ppc::round_single(" + madd(true, true, f1(ra), f1(rc), f1(rb)) + "))");
            return;
        case Op::ps_madds0:
            pair(d, "ppc::round_single(" + madd(true, false, f0(ra), f0(rc), f0(rb)) + ")",
                 "ppc::round_single(" + madd(true, false, f1(ra), f0(rc), f1(rb)) + ")");
            return;
        case Op::ps_madds1:
            pair(d, "ppc::round_single(" + madd(true, false, f0(ra), f1(rc), f0(rb)) + ")",
                 "ppc::round_single(" + madd(true, false, f1(ra), f1(rc), f1(rb)) + ")");
            return;
        case Op::ps_muls0:
            pair(d, text::format("ppc::round_single(ppc::mul({}, ppc::round_25bit({})))", f0(ra), f0(rc)),
                 text::format("ppc::round_single(ppc::mul({}, ppc::round_25bit({})))", f1(ra), f0(rc)));
            return;
        case Op::ps_muls1:
            pair(d, text::format("ppc::round_single(ppc::mul({}, ppc::round_25bit({})))", f0(ra), f1(rc)),
                 text::format("ppc::round_single(ppc::mul({}, ppc::round_25bit({})))", f1(ra), f1(rc)));
            return;
        case Op::ps_sum0:
            pair(d, text::format("ppc::round_single(ppc::add({}, {}))", f0(ra), f1(rb)),
                 text::format("ppc::round_single({})", f1(rc)));
            return;
        case Op::ps_sum1:
            pair(d, text::format("ppc::round_single({})", f0(rc)),
                 text::format("ppc::round_single(ppc::add({}, {}))", f0(ra), f1(rb)));
            return;
        case Op::ps_sel:
            pair(d, text::format("ppc::fsel({}, {}, {})", f0(ra), f0(rb), f0(rc)),
                 text::format("ppc::fsel({}, {}, {})", f1(ra), f1(rb), f1(rc)));
            return;
        case Op::ps_res:
            pair(d, text::format("ppc::res({})", f0(rb)), text::format("ppc::res({})", f1(rb)));
            return;
        case Op::ps_rsqrte:
            pair(d, text::format("ppc::round_single(ppc::rsqrte({}))", f0(rb)),
                 text::format("ppc::round_single(ppc::rsqrte({}))", f1(rb)));
            return;
        case Op::ps_mr: pair(d, f0(rb), f1(rb)); return;
        case Op::ps_neg: ps_bits(d, rb, "^ ppc::kSign"); return;
        case Op::ps_abs: ps_bits(d, rb, "& ~ppc::kSign"); return;
        case Op::ps_nabs: ps_bits(d, rb, "| ppc::kSign"); return;
        case Op::ps_merge00: pair(d, f0(ra), f0(rb)); return;
        case Op::ps_merge01: pair(d, f0(ra), f1(rb)); return;
        case Op::ps_merge10: pair(d, f1(ra), f0(rb)); return;
        case Op::ps_merge11: pair(d, f1(ra), f1(rb)); return;
        case Op::ps_cmpu0:
        case Op::ps_cmpo0:
            line(text::format("ppc::fcmp({}, {}, {});", crf(ppc::crfd(w)), f0(ra), f0(rb)));
            return;
        case Op::ps_cmpu1:
        case Op::ps_cmpo1:
            line(text::format("ppc::fcmp({}, {}, {});", crf(ppc::crfd(w)), f1(ra), f1(rb)));
            return;

        // ------------------------------------- never valid in a game image
        case Op::sc:
        case Op::rfi:
        case Op::tdi:
        case Op::mfmsr:
        case Op::mtmsr:
        case Op::mfsr:
        case Op::mtsr:
        case Op::mfsrin:
        case Op::mtsrin:
        case Op::tlbie:
        case Op::tlbsync:
        case Op::eciwx:
        case Op::ecowx:
        case Op::mffs:
        case Op::mtfsf:
        case Op::mtfsfi:
        case Op::mtfsb0:
        case Op::mtfsb1:
        case Op::mcrfs:
            illegal(a, w, "instruction not supported in user code");
            return;
        case Op::invalid:
        case Op::count:
            illegal(a, w, "undecodable instruction");
            return;
        }
        throw std::logic_error("unhandled opcode in code generator");
    }

    static constexpr const char* kLha = "static_cast<uint32_t>(static_cast<int16_t>(PPC_LOAD_U16({})))";

    void logical(uint32_t w, const char* pattern) {
        const uint32_t ra = ppc::ra(w);
        const std::string s = r(ppc::rd(w));
        const std::string b = r(ppc::rb(w));
        line(text::format("{} = {};", r(ra), text::format(pattern, s, b)));
        if (ppc::rc(w)) record(r(ra));
    }

    void crlogic(uint32_t w, const char* pattern) {
        const std::string a = crbit(ppc::ra(w));
        const std::string b = crbit(ppc::rb(w));
        line(text::format("{} = {};", crbit(ppc::rd(w)),
                          text::format(pattern, a, b)));
    }

    void load(uint32_t w, const char* pattern, bool update, bool indexed) {
        const uint32_t d = ppc::rd(w);
        const uint32_t ra = ppc::ra(w);
        const std::string ea = indexed ? ea_x(ra, ppc::rb(w)) : ea_d(ra, ppc::simm(w));
        if (!update) {
            line(text::format("{} = {};", r(d), text::format(pattern, ea)));
            return;
        }
        const std::string e = "ea";
        line(text::format("{{ const uint32_t ea = {}; {} = {}; {} = ea; }}", ea, r(d),
                          text::format(pattern, e), r(ra)));
    }

    void store(uint32_t w, const char* pattern, bool update, bool indexed) {
        const uint32_t s = ppc::rd(w);
        const uint32_t ra = ppc::ra(w);
        const std::string ea = indexed ? ea_x(ra, ppc::rb(w)) : ea_d(ra, ppc::simm(w));
        const std::string value = r(s);
        if (!update) {
            line(text::format(pattern, ea, value));
            return;
        }
        const std::string e = "ea";
        line(text::format("{{ const uint32_t ea = {}; {} {} = ea; }}", ea,
                          text::format(pattern, e, value), r(ra)));
    }

    void fload(uint32_t w, bool single, bool update, bool indexed) {
        const uint32_t d = ppc::rd(w);
        const uint32_t ra = ppc::ra(w);
        const std::string ea = indexed ? ea_x(ra, ppc::rb(w)) : ea_d(ra, ppc::simm(w));
        std::string body = text::format("const uint32_t ea = {}; ", ea);
        if (single) {
            body += text::format("const double v = ppc::load_single(PPC_LOAD_U32(ea)); {} = v; {} = v;",
                                 f0(d), f1(d));
        } else {
            body += text::format("{} = ppc::from_bits(PPC_LOAD_U64(ea));", f0(d));
        }
        if (update) {
            body += text::format(" {} = ea;", r(ra));
        }
        line("{ " + body + " }");
    }

    void fstore(uint32_t w, const char* pattern, bool update, bool indexed) {
        const uint32_t s = ppc::rd(w);
        const uint32_t ra = ppc::ra(w);
        const std::string ea = indexed ? ea_x(ra, ppc::rb(w)) : ea_d(ra, ppc::simm(w));
        const std::string e = "ea";
        const std::string value = f0(s);
        std::string body = text::format("const uint32_t ea = {}; {}", ea,
                                        text::format(pattern, e, value));
        if (update) {
            body += text::format(" {} = ea;", r(ra));
        }
        line("{ " + body + " }");
    }

    static std::string madd(bool single, bool subtract, const std::string& a,
                            const std::string& c, const std::string& b) {
        return text::format("ppc::madd<{}, {}>({}, {}, {})", single, subtract, a, c, b);
    }

    void ps2(uint32_t d, const char* pattern, uint32_t x, uint32_t y) {
        const std::string x0 = f0(x), y0 = f0(y), x1 = f1(x), y1 = f1(y);
        pair(d, text::format(pattern, x0, y0),
             text::format(pattern, x1, y1));
    }

    void ps_bits(uint32_t d, uint32_t b, const char* op) {
        pair(d, text::format("ppc::from_bits(ppc::bits({}) {})", f0(b), op),
             text::format("ppc::from_bits(ppc::bits({}) {})", f1(b), op));
    }

    void psq(bool load, bool update, const std::string& ea, uint32_t fr, bool w_bit,
             uint32_t gqr, uint32_t ra) {
        std::string body = text::format("const uint32_t ea = {}; ", ea);
        body += text::format("ppc::psq_{}<{}>(ctx, base, {}, ea, ctx.gqr[{}]);",
                             load ? "load" : "store", w_bit, fr, gqr);
        if (update) {
            body += text::format(" {} = ea;", r(ra));
        }
        line("{ " + body + " }");
    }

    const Program& program_;
    const FunctionInfo& function_;
    const EmitOptions& options_;
    std::set<std::string>& referenced_;
    std::string out_;
};

} // namespace

std::string mid_hook_name(uint32_t address) { return text::format("cafe_mid_hook_{:08X}", address); }

std::string emit_entry(const Program& program, const Entry& entry,
                       const EmitOptions& options,
                       std::set<std::string>& referenced) {
    const FunctionInfo& function = program.functions.at(entry.function);
    return Emitter(program, function, options, referenced).run(entry);
}

} // namespace cafe::recomp
