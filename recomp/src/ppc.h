#pragma once

// Espresso (PowerPC 750CL-derived) instruction decoding. Covers the user-mode
// integer, floating-point and paired-single sets plus the supervisor/cache
// instructions a game image can contain. Record (Rc) and overflow (OE) bits
// are not separate opcodes; query them with rc() / oe().

#include <cstdint>
#include <string>

namespace cafe::ppc {

#define CAFE_PPC_OPS(X)                                                        \
    X(invalid, "(invalid)")                                                    \
    /* integer arithmetic */                                                   \
    X(addi, "addi") X(addis, "addis") X(addic, "addic")                        \
    X(addic_rc, "addic.") X(subfic, "subfic") X(mulli, "mulli")                \
    X(add, "add") X(addc, "addc") X(adde, "adde") X(addze, "addze")            \
    X(addme, "addme") X(subf, "subf") X(subfc, "subfc") X(subfe, "subfe")      \
    X(subfze, "subfze") X(subfme, "subfme") X(neg, "neg")                      \
    X(mullw, "mullw") X(mulhw, "mulhw") X(mulhwu, "mulhwu")                    \
    X(divw, "divw") X(divwu, "divwu")                                          \
    /* compare */                                                              \
    X(cmp, "cmp") X(cmpl, "cmpl") X(cmpi, "cmpi") X(cmpli, "cmpli")            \
    /* logical */                                                              \
    X(and_, "and") X(andc, "andc") X(or_, "or") X(orc, "orc") X(xor_, "xor")   \
    X(nand, "nand") X(nor, "nor") X(eqv, "eqv") X(andi_rc, "andi.")            \
    X(andis_rc, "andis.") X(ori, "ori") X(oris, "oris") X(xori, "xori")        \
    X(xoris, "xoris") X(extsb, "extsb") X(extsh, "extsh")                      \
    X(cntlzw, "cntlzw")                                                        \
    /* rotate / shift */                                                       \
    X(rlwimi, "rlwimi") X(rlwinm, "rlwinm") X(rlwnm, "rlwnm") X(slw, "slw")    \
    X(srw, "srw") X(sraw, "sraw") X(srawi, "srawi")                            \
    /* integer load / store */                                                 \
    X(lbz, "lbz") X(lbzu, "lbzu") X(lbzx, "lbzx") X(lbzux, "lbzux")            \
    X(lhz, "lhz") X(lhzu, "lhzu") X(lhzx, "lhzx") X(lhzux, "lhzux")            \
    X(lha, "lha") X(lhau, "lhau") X(lhax, "lhax") X(lhaux, "lhaux")            \
    X(lwz, "lwz") X(lwzu, "lwzu") X(lwzx, "lwzx") X(lwzux, "lwzux")            \
    X(stb, "stb") X(stbu, "stbu") X(stbx, "stbx") X(stbux, "stbux")            \
    X(sth, "sth") X(sthu, "sthu") X(sthx, "sthx") X(sthux, "sthux")            \
    X(stw, "stw") X(stwu, "stwu") X(stwx, "stwx") X(stwux, "stwux")            \
    X(lhbrx, "lhbrx") X(lwbrx, "lwbrx") X(sthbrx, "sthbrx")                    \
    X(stwbrx, "stwbrx") X(lmw, "lmw") X(stmw, "stmw") X(lswi, "lswi")          \
    X(lswx, "lswx") X(stswi, "stswi") X(stswx, "stswx") X(lwarx, "lwarx")      \
    X(stwcx, "stwcx.")                                                         \
    /* branch / system */                                                      \
    X(b, "b") X(bc, "bc") X(bclr, "bclr") X(bcctr, "bcctr") X(sc, "sc")        \
    X(tw, "tw") X(twi, "twi") X(tdi, "tdi") X(rfi, "rfi")                      \
    /* condition register */                                                   \
    X(mcrf, "mcrf") X(crand, "crand") X(crandc, "crandc") X(creqv, "creqv")    \
    X(crnand, "crnand") X(crnor, "crnor") X(cror, "cror") X(crorc, "crorc")    \
    X(crxor, "crxor") X(mfcr, "mfcr") X(mtcrf, "mtcrf") X(mcrxr, "mcrxr")      \
    /* special registers */                                                    \
    X(mfspr, "mfspr") X(mtspr, "mtspr") X(mftb, "mftb") X(mfmsr, "mfmsr")      \
    X(mtmsr, "mtmsr") X(mfsr, "mfsr") X(mtsr, "mtsr") X(mfsrin, "mfsrin")      \
    X(mtsrin, "mtsrin")                                                        \
    /* cache / sync */                                                         \
    X(dcbf, "dcbf") X(dcbi, "dcbi") X(dcbst, "dcbst") X(dcbt, "dcbt")          \
    X(dcbtst, "dcbtst") X(dcbz, "dcbz") X(dcbz_l, "dcbz_l") X(icbi, "icbi")    \
    X(sync, "sync") X(isync, "isync") X(eieio, "eieio") X(tlbie, "tlbie")      \
    X(tlbsync, "tlbsync") X(eciwx, "eciwx") X(ecowx, "ecowx")                  \
    /* floating-point load / store */                                          \
    X(lfs, "lfs") X(lfsu, "lfsu") X(lfsx, "lfsx") X(lfsux, "lfsux")            \
    X(lfd, "lfd") X(lfdu, "lfdu") X(lfdx, "lfdx") X(lfdux, "lfdux")            \
    X(stfs, "stfs") X(stfsu, "stfsu") X(stfsx, "stfsx") X(stfsux, "stfsux")    \
    X(stfd, "stfd") X(stfdu, "stfdu") X(stfdx, "stfdx") X(stfdux, "stfdux")    \
    X(stfiwx, "stfiwx")                                                        \
    /* floating-point arithmetic */                                            \
    X(fadd, "fadd") X(fadds, "fadds") X(fsub, "fsub") X(fsubs, "fsubs")        \
    X(fmul, "fmul") X(fmuls, "fmuls") X(fdiv, "fdiv") X(fdivs, "fdivs")        \
    X(fmadd, "fmadd") X(fmadds, "fmadds") X(fmsub, "fmsub")                    \
    X(fmsubs, "fmsubs") X(fnmadd, "fnmadd") X(fnmadds, "fnmadds")              \
    X(fnmsub, "fnmsub") X(fnmsubs, "fnmsubs") X(fsqrt, "fsqrt")                \
    X(fsqrts, "fsqrts") X(fres, "fres") X(frsqrte, "frsqrte") X(fsel, "fsel")  \
    X(frsp, "frsp") X(fctiw, "fctiw") X(fctiwz, "fctiwz") X(fmr, "fmr")        \
    X(fneg, "fneg") X(fabs, "fabs") X(fnabs, "fnabs") X(fcmpu, "fcmpu")        \
    X(fcmpo, "fcmpo") X(mffs, "mffs") X(mtfsf, "mtfsf") X(mtfsfi, "mtfsfi")    \
    X(mtfsb0, "mtfsb0") X(mtfsb1, "mtfsb1") X(mcrfs, "mcrfs")                  \
    /* paired single */                                                        \
    X(psq_l, "psq_l") X(psq_lu, "psq_lu") X(psq_lx, "psq_lx")                  \
    X(psq_lux, "psq_lux") X(psq_st, "psq_st") X(psq_stu, "psq_stu")            \
    X(psq_stx, "psq_stx") X(psq_stux, "psq_stux")                              \
    X(ps_add, "ps_add") X(ps_sub, "ps_sub") X(ps_mul, "ps_mul")                \
    X(ps_div, "ps_div") X(ps_madd, "ps_madd") X(ps_msub, "ps_msub")            \
    X(ps_nmadd, "ps_nmadd") X(ps_nmsub, "ps_nmsub") X(ps_muls0, "ps_muls0")    \
    X(ps_muls1, "ps_muls1") X(ps_madds0, "ps_madds0")                          \
    X(ps_madds1, "ps_madds1") X(ps_sum0, "ps_sum0") X(ps_sum1, "ps_sum1")      \
    X(ps_sel, "ps_sel") X(ps_res, "ps_res") X(ps_rsqrte, "ps_rsqrte")          \
    X(ps_mr, "ps_mr") X(ps_neg, "ps_neg") X(ps_abs, "ps_abs")                  \
    X(ps_nabs, "ps_nabs") X(ps_merge00, "ps_merge00")                          \
    X(ps_merge01, "ps_merge01") X(ps_merge10, "ps_merge10")                    \
    X(ps_merge11, "ps_merge11") X(ps_cmpu0, "ps_cmpu0")                        \
    X(ps_cmpu1, "ps_cmpu1") X(ps_cmpo0, "ps_cmpo0") X(ps_cmpo1, "ps_cmpo1")

enum class Op : uint16_t {
#define CAFE_PPC_ENUM(id, text) id,
    CAFE_PPC_OPS(CAFE_PPC_ENUM)
#undef CAFE_PPC_ENUM
        count
};

const char* op_name(Op op);
Op decode(uint32_t word);

// Whether the Rc bit is architecturally meaningful (record form) for op.
bool has_record_form(Op op);
// Whether the OE bit is meaningful (XO-form integer arithmetic).
bool has_overflow_form(Op op);

// Field accessors, named after the PowerPC architecture book.
inline uint32_t opcd(uint32_t w) { return w >> 26; }
inline uint32_t rd(uint32_t w) { return (w >> 21) & 31; } // also rs, frd, bo
inline uint32_t ra(uint32_t w) { return (w >> 16) & 31; } // also bi
inline uint32_t rb(uint32_t w) { return (w >> 11) & 31; } // also sh
inline uint32_t frc(uint32_t w) { return (w >> 6) & 31; }
inline uint32_t mb(uint32_t w) { return (w >> 6) & 31; }
inline uint32_t me(uint32_t w) { return (w >> 1) & 31; }
inline bool rc(uint32_t w) { return (w & 1) != 0; }
inline bool lk(uint32_t w) { return (w & 1) != 0; }
inline bool aa(uint32_t w) { return (w & 2) != 0; }
inline bool oe(uint32_t w) { return (w & 0x400) != 0; }
inline int32_t simm(uint32_t w) { return static_cast<int16_t>(w & 0xFFFF); }
inline uint32_t uimm(uint32_t w) { return w & 0xFFFF; }
inline uint32_t crfd(uint32_t w) { return (w >> 23) & 7; }
inline uint32_t crfs(uint32_t w) { return (w >> 18) & 7; }
inline uint32_t crm(uint32_t w) { return (w >> 12) & 0xFF; }
inline uint32_t fm(uint32_t w) { return (w >> 17) & 0xFF; }
inline uint32_t spr(uint32_t w) { return ((w >> 16) & 31) | (((w >> 11) & 31) << 5); }
inline int32_t li(uint32_t w) {
    return static_cast<int32_t>((w & 0x03FFFFFC) << 6) >> 6;
}
inline int32_t bd(uint32_t w) { return static_cast<int16_t>(w & 0xFFFC); }
// psq_l/psq_st (D-form): 12-bit displacement, W and I fields.
inline int32_t psq_d(uint32_t w) {
    return static_cast<int32_t>((w & 0xFFF) << 20) >> 20;
}
inline bool psq_w(uint32_t w) { return (w >> 15) & 1; }
inline uint32_t psq_i(uint32_t w) { return (w >> 12) & 7; }
// psq_lx/psq_stx (X-form): W and I fields sit lower.
inline bool psqx_w(uint32_t w) { return (w >> 10) & 1; }
inline uint32_t psqx_i(uint32_t w) { return (w >> 7) & 7; }

// Branch target for b/bc at `address`, honouring AA.
inline uint32_t branch_target(uint32_t w, uint32_t address) {
    const int32_t offset = opcd(w) == 18 ? li(w) : bd(w);
    return aa(w) ? static_cast<uint32_t>(offset)
                 : address + static_cast<uint32_t>(offset);
}

// Short human-readable disassembly, for comments and diagnostics.
std::string disassemble(uint32_t word, uint32_t address);

} // namespace cafe::ppc
