#include "ppc.h"

#include <array>
#include <cstdio>

namespace cafe::ppc {
namespace {

constexpr std::array kNames{
#define CAFE_PPC_NAME(id, text) text,
    CAFE_PPC_OPS(CAFE_PPC_NAME)
#undef CAFE_PPC_NAME
};
static_assert(kNames.size() == static_cast<size_t>(Op::count));

Op decode_19(uint32_t w) {
    switch ((w >> 1) & 0x3FF) {
    case 0: return Op::mcrf;
    case 16: return Op::bclr;
    case 33: return Op::crnor;
    case 50: return Op::rfi;
    case 129: return Op::crandc;
    case 150: return Op::isync;
    case 193: return Op::crxor;
    case 225: return Op::crnand;
    case 257: return Op::crand;
    case 289: return Op::creqv;
    case 417: return Op::crorc;
    case 449: return Op::cror;
    case 528: return Op::bcctr;
    default: return Op::invalid;
    }
}

Op decode_31(uint32_t w) {
    switch ((w >> 1) & 0x3FF) {
    case 0: return Op::cmp;
    case 4: return Op::tw;
    case 19: return Op::mfcr;
    case 20: return Op::lwarx;
    case 23: return Op::lwzx;
    case 24: return Op::slw;
    case 26: return Op::cntlzw;
    case 28: return Op::and_;
    case 32: return Op::cmpl;
    case 54: return Op::dcbst;
    case 55: return Op::lwzux;
    case 60: return Op::andc;
    case 83: return Op::mfmsr;
    case 86: return Op::dcbf;
    case 87: return Op::lbzx;
    case 119: return Op::lbzux;
    case 124: return Op::nor;
    case 144: return Op::mtcrf;
    case 146: return Op::mtmsr;
    case 150: return Op::stwcx;
    case 151: return Op::stwx;
    case 183: return Op::stwux;
    case 210: return Op::mtsr;
    case 215: return Op::stbx;
    case 242: return Op::mtsrin;
    case 246: return Op::dcbtst;
    case 247: return Op::stbux;
    case 278: return Op::dcbt;
    case 279: return Op::lhzx;
    case 284: return Op::eqv;
    case 306: return Op::tlbie;
    case 310: return Op::eciwx;
    case 311: return Op::lhzux;
    case 316: return Op::xor_;
    case 339: return Op::mfspr;
    case 343: return Op::lhax;
    case 371: return Op::mftb;
    case 375: return Op::lhaux;
    case 407: return Op::sthx;
    case 412: return Op::orc;
    case 438: return Op::ecowx;
    case 439: return Op::sthux;
    case 444: return Op::or_;
    case 467: return Op::mtspr;
    case 470: return Op::dcbi;
    case 476: return Op::nand;
    case 512: return Op::mcrxr;
    case 533: return Op::lswx;
    case 534: return Op::lwbrx;
    case 535: return Op::lfsx;
    case 536: return Op::srw;
    case 566: return Op::tlbsync;
    case 567: return Op::lfsux;
    case 595: return Op::mfsr;
    case 597: return Op::lswi;
    case 598: return Op::sync;
    case 599: return Op::lfdx;
    case 631: return Op::lfdux;
    case 659: return Op::mfsrin;
    case 661: return Op::stswx;
    case 662: return Op::stwbrx;
    case 663: return Op::stfsx;
    case 695: return Op::stfsux;
    case 725: return Op::stswi;
    case 727: return Op::stfdx;
    case 759: return Op::stfdux;
    case 790: return Op::lhbrx;
    case 792: return Op::sraw;
    case 824: return Op::srawi;
    case 854: return Op::eieio;
    case 918: return Op::sthbrx;
    case 922: return Op::extsh;
    case 954: return Op::extsb;
    case 982: return Op::icbi;
    case 983: return Op::stfiwx;
    case 1014: return Op::dcbz;
    default: break;
    }
    // XO-form arithmetic: 9-bit extended opcode, bit 21 is OE.
    switch ((w >> 1) & 0x1FF) {
    case 8: return Op::subfc;
    case 10: return Op::addc;
    case 11: return oe(w) ? Op::invalid : Op::mulhwu;
    case 40: return Op::subf;
    case 75: return oe(w) ? Op::invalid : Op::mulhw;
    case 104: return Op::neg;
    case 136: return Op::subfe;
    case 138: return Op::adde;
    case 200: return Op::subfze;
    case 202: return Op::addze;
    case 232: return Op::subfme;
    case 234: return Op::addme;
    case 235: return Op::mullw;
    case 266: return Op::add;
    case 459: return Op::divwu;
    case 491: return Op::divw;
    default: return Op::invalid;
    }
}

Op decode_59(uint32_t w) {
    switch ((w >> 1) & 0x1F) {
    case 18: return Op::fdivs;
    case 20: return Op::fsubs;
    case 21: return Op::fadds;
    case 22: return Op::fsqrts;
    case 24: return Op::fres;
    case 25: return Op::fmuls;
    case 28: return Op::fmsubs;
    case 29: return Op::fmadds;
    case 30: return Op::fnmsubs;
    case 31: return Op::fnmadds;
    default: return Op::invalid;
    }
}

Op decode_63(uint32_t w) {
    // A-form opcodes occupy low-five-bit values no X-form opcode here uses.
    switch ((w >> 1) & 0x1F) {
    case 18: return Op::fdiv;
    case 20: return Op::fsub;
    case 21: return Op::fadd;
    case 22: return Op::fsqrt;
    case 23: return Op::fsel;
    case 25: return Op::fmul;
    case 26: return Op::frsqrte;
    case 28: return Op::fmsub;
    case 29: return Op::fmadd;
    case 30: return Op::fnmsub;
    case 31: return Op::fnmadd;
    default: break;
    }
    switch ((w >> 1) & 0x3FF) {
    case 0: return Op::fcmpu;
    case 12: return Op::frsp;
    case 14: return Op::fctiw;
    case 15: return Op::fctiwz;
    case 32: return Op::fcmpo;
    case 38: return Op::mtfsb1;
    case 40: return Op::fneg;
    case 64: return Op::mcrfs;
    case 70: return Op::mtfsb0;
    case 72: return Op::fmr;
    case 134: return Op::mtfsfi;
    case 136: return Op::fnabs;
    case 264: return Op::fabs;
    case 583: return Op::mffs;
    case 711: return Op::mtfsf;
    default: return Op::invalid;
    }
}

Op decode_4(uint32_t w) {
    switch ((w >> 1) & 0x1F) {
    case 6: return (w & 0x40) ? Op::psq_lux : Op::psq_lx;
    case 7: return (w & 0x40) ? Op::psq_stux : Op::psq_stx;
    case 10: return Op::ps_sum0;
    case 11: return Op::ps_sum1;
    case 12: return Op::ps_muls0;
    case 13: return Op::ps_muls1;
    case 14: return Op::ps_madds0;
    case 15: return Op::ps_madds1;
    case 18: return Op::ps_div;
    case 20: return Op::ps_sub;
    case 21: return Op::ps_add;
    case 23: return Op::ps_sel;
    case 24: return Op::ps_res;
    case 25: return Op::ps_mul;
    case 26: return Op::ps_rsqrte;
    case 28: return Op::ps_msub;
    case 29: return Op::ps_madd;
    case 30: return Op::ps_nmsub;
    case 31: return Op::ps_nmadd;
    default: break;
    }
    switch ((w >> 1) & 0x3FF) {
    case 0: return Op::ps_cmpu0;
    case 32: return Op::ps_cmpo0;
    case 40: return Op::ps_neg;
    case 64: return Op::ps_cmpu1;
    case 72: return Op::ps_mr;
    case 96: return Op::ps_cmpo1;
    case 136: return Op::ps_nabs;
    case 264: return Op::ps_abs;
    case 528: return Op::ps_merge00;
    case 560: return Op::ps_merge01;
    case 592: return Op::ps_merge10;
    case 624: return Op::ps_merge11;
    case 1014: return Op::dcbz_l;
    default: return Op::invalid;
    }
}

} // namespace

const char* op_name(Op op) {
    const auto index = static_cast<size_t>(op);
    return index < kNames.size() ? kNames[index] : "(bad op)";
}

Op decode(uint32_t w) {
    switch (opcd(w)) {
    case 2: return Op::tdi;
    case 3: return Op::twi;
    case 4: return decode_4(w);
    case 7: return Op::mulli;
    case 8: return Op::subfic;
    case 10: return Op::cmpli;
    case 11: return Op::cmpi;
    case 12: return Op::addic;
    case 13: return Op::addic_rc;
    case 14: return Op::addi;
    case 15: return Op::addis;
    case 16: return Op::bc;
    case 17: return (w & 2) ? Op::sc : Op::invalid;
    case 18: return Op::b;
    case 19: return decode_19(w);
    case 20: return Op::rlwimi;
    case 21: return Op::rlwinm;
    case 23: return Op::rlwnm;
    case 24: return Op::ori;
    case 25: return Op::oris;
    case 26: return Op::xori;
    case 27: return Op::xoris;
    case 28: return Op::andi_rc;
    case 29: return Op::andis_rc;
    case 31: return decode_31(w);
    case 32: return Op::lwz;
    case 33: return Op::lwzu;
    case 34: return Op::lbz;
    case 35: return Op::lbzu;
    case 36: return Op::stw;
    case 37: return Op::stwu;
    case 38: return Op::stb;
    case 39: return Op::stbu;
    case 40: return Op::lhz;
    case 41: return Op::lhzu;
    case 42: return Op::lha;
    case 43: return Op::lhau;
    case 44: return Op::sth;
    case 45: return Op::sthu;
    case 46: return Op::lmw;
    case 47: return Op::stmw;
    case 48: return Op::lfs;
    case 49: return Op::lfsu;
    case 50: return Op::lfd;
    case 51: return Op::lfdu;
    case 52: return Op::stfs;
    case 53: return Op::stfsu;
    case 54: return Op::stfd;
    case 55: return Op::stfdu;
    case 56: return Op::psq_l;
    case 57: return Op::psq_lu;
    case 59: return decode_59(w);
    case 60: return Op::psq_st;
    case 61: return Op::psq_stu;
    case 63: return decode_63(w);
    default: return Op::invalid;
    }
}

bool has_record_form(Op op) {
    switch (op) {
    case Op::add: case Op::addc: case Op::adde: case Op::addze:
    case Op::addme: case Op::subf: case Op::subfc: case Op::subfe:
    case Op::subfze: case Op::subfme: case Op::neg: case Op::mullw:
    case Op::mulhw: case Op::mulhwu: case Op::divw: case Op::divwu:
    case Op::and_: case Op::andc: case Op::or_: case Op::orc: case Op::xor_:
    case Op::nand: case Op::nor: case Op::eqv: case Op::extsb: case Op::extsh:
    case Op::cntlzw: case Op::rlwimi: case Op::rlwinm: case Op::rlwnm:
    case Op::slw: case Op::srw: case Op::sraw: case Op::srawi:
    case Op::fadd: case Op::fadds: case Op::fsub: case Op::fsubs:
    case Op::fmul: case Op::fmuls: case Op::fdiv: case Op::fdivs:
    case Op::fmadd: case Op::fmadds: case Op::fmsub: case Op::fmsubs:
    case Op::fnmadd: case Op::fnmadds: case Op::fnmsub: case Op::fnmsubs:
    case Op::fsqrt: case Op::fsqrts: case Op::fres: case Op::frsqrte:
    case Op::fsel: case Op::frsp: case Op::fctiw: case Op::fctiwz:
    case Op::fmr: case Op::fneg: case Op::fabs: case Op::fnabs:
    case Op::mffs: case Op::mtfsf: case Op::mtfsfi: case Op::mtfsb0:
    case Op::mtfsb1:
    case Op::ps_add: case Op::ps_sub: case Op::ps_mul: case Op::ps_div:
    case Op::ps_madd: case Op::ps_msub: case Op::ps_nmadd: case Op::ps_nmsub:
    case Op::ps_muls0: case Op::ps_muls1: case Op::ps_madds0:
    case Op::ps_madds1: case Op::ps_sum0: case Op::ps_sum1: case Op::ps_sel:
    case Op::ps_res: case Op::ps_rsqrte: case Op::ps_mr: case Op::ps_neg:
    case Op::ps_abs: case Op::ps_nabs: case Op::ps_merge00:
    case Op::ps_merge01: case Op::ps_merge10: case Op::ps_merge11:
        return true;
    default:
        return false;
    }
}

bool has_overflow_form(Op op) {
    switch (op) {
    case Op::add: case Op::addc: case Op::adde: case Op::addze:
    case Op::addme: case Op::subf: case Op::subfc: case Op::subfe:
    case Op::subfze: case Op::subfme: case Op::neg: case Op::mullw:
    case Op::divw: case Op::divwu:
        return true;
    default:
        return false;
    }
}

std::string disassemble(uint32_t w, uint32_t address) {
    const Op op = decode(w);
    std::string text = op_name(op);
    if (has_overflow_form(op) && oe(w)) {
        text += 'o';
    }
    if (has_record_form(op) && rc(w)) {
        text += '.';
    }
    char operands[96]{};
    switch (op) {
    case Op::b:
    case Op::bc:
        if (lk(w)) {
            text += 'l';
        }
        if (aa(w)) {
            text += 'a';
        }
        if (op == Op::b) {
            std::snprintf(operands, sizeof operands, "0x%08X",
                          branch_target(w, address));
        } else {
            std::snprintf(operands, sizeof operands, "%u, %u, 0x%08X", rd(w),
                          ra(w), branch_target(w, address));
        }
        break;
    case Op::bclr:
    case Op::bcctr:
        if (lk(w)) {
            text += 'l';
        }
        std::snprintf(operands, sizeof operands, "%u, %u", rd(w), ra(w));
        break;
    case Op::addi: case Op::addis: case Op::addic: case Op::addic_rc:
    case Op::subfic: case Op::mulli: case Op::twi: case Op::tdi:
        std::snprintf(operands, sizeof operands, "r%u, r%u, %d", rd(w), ra(w),
                      simm(w));
        break;
    case Op::ori: case Op::oris: case Op::xori: case Op::xoris:
    case Op::andi_rc: case Op::andis_rc:
        std::snprintf(operands, sizeof operands, "r%u, r%u, 0x%X", ra(w), rd(w),
                      uimm(w));
        break;
    case Op::cmpi:
        std::snprintf(operands, sizeof operands, "cr%u, r%u, %d", crfd(w),
                      ra(w), simm(w));
        break;
    case Op::cmpli:
        std::snprintf(operands, sizeof operands, "cr%u, r%u, 0x%X", crfd(w),
                      ra(w), uimm(w));
        break;
    case Op::cmp: case Op::cmpl:
        std::snprintf(operands, sizeof operands, "cr%u, r%u, r%u", crfd(w),
                      ra(w), rb(w));
        break;
    case Op::fcmpu: case Op::fcmpo: case Op::ps_cmpu0: case Op::ps_cmpu1:
    case Op::ps_cmpo0: case Op::ps_cmpo1:
        std::snprintf(operands, sizeof operands, "cr%u, f%u, f%u", crfd(w),
                      ra(w), rb(w));
        break;
    case Op::rlwimi: case Op::rlwinm:
        std::snprintf(operands, sizeof operands, "r%u, r%u, %u, %u, %u", ra(w),
                      rd(w), rb(w), mb(w), me(w));
        break;
    case Op::rlwnm:
        std::snprintf(operands, sizeof operands, "r%u, r%u, r%u, %u, %u", ra(w),
                      rd(w), rb(w), mb(w), me(w));
        break;
    case Op::lwz: case Op::lwzu: case Op::lbz: case Op::lbzu: case Op::lhz:
    case Op::lhzu: case Op::lha: case Op::lhau: case Op::stw: case Op::stwu:
    case Op::stb: case Op::stbu: case Op::sth: case Op::sthu: case Op::lmw:
    case Op::stmw:
        std::snprintf(operands, sizeof operands, "r%u, %d(r%u)", rd(w), simm(w),
                      ra(w));
        break;
    case Op::lfs: case Op::lfsu: case Op::lfd: case Op::lfdu: case Op::stfs:
    case Op::stfsu: case Op::stfd: case Op::stfdu:
        std::snprintf(operands, sizeof operands, "f%u, %d(r%u)", rd(w), simm(w),
                      ra(w));
        break;
    case Op::psq_l: case Op::psq_lu: case Op::psq_st: case Op::psq_stu:
        std::snprintf(operands, sizeof operands, "f%u, %d(r%u), %u, qr%u",
                      rd(w), psq_d(w), ra(w), psq_w(w) ? 1u : 0u, psq_i(w));
        break;
    case Op::psq_lx: case Op::psq_lux: case Op::psq_stx: case Op::psq_stux:
        std::snprintf(operands, sizeof operands, "f%u, r%u, r%u, %u, qr%u",
                      rd(w), ra(w), rb(w), psqx_w(w) ? 1u : 0u, psqx_i(w));
        break;
    case Op::mfspr: case Op::mtspr:
        std::snprintf(operands, sizeof operands, "r%u, spr%u", rd(w), spr(w));
        break;
    default:
        std::snprintf(operands, sizeof operands, "%u, %u, %u, %u", rd(w), ra(w),
                      rb(w), frc(w));
        break;
    }
    return text + ' ' + operands;
}

} // namespace cafe::ppc
