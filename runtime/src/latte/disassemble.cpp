#include "latte/program.h"

#include <bit>
#include <cstdarg>
#include <cstdio>

namespace cafe::latte {
namespace {

using namespace isa;

constexpr char kChan[] = "xyzwt";
constexpr char kSel[] = "xyzw01?_";

void append(std::string& out, const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    out += buffer;
}

std::string gpr(uint32_t index, bool rel, uint32_t index_mode) {
    char b[48];
    if (!rel) {
        std::snprintf(b, sizeof(b), "R%u", index);
    } else {
        static constexpr const char* kModes[] = {"AR.x", "AR.y", "AR.z", "AR.w", "AL"};
        std::snprintf(b, sizeof(b), "R[%u+%s]", index, index_mode < 5 ? kModes[index_mode] : "?");
    }
    return b;
}

std::string source(const AluInstruction& in, const AluSource& s, const AluGroup& group, const CfInstruction& cf) {
    std::string text;
    char b[64];
    const uint32_t sel = s.sel;
    if (sel <= alu::kGprLast) {
        text = gpr(sel, s.rel, in.index_mode) + "." + kChan[s.chan];
    } else if (sel >= alu::kKcacheBank0 && sel < alu::kKcacheBank1 + 32) {
        const int bank = sel >= alu::kKcacheBank1 ? 1 : 0;
        const uint32_t index = sel - (bank ? alu::kKcacheBank1 : alu::kKcacheBank0);
        std::snprintf(b, sizeof(b), "KC%d[%u].%c", bank, cf.kcache[bank].addr * 16 + index, kChan[s.chan]);
        text = b;
    } else if (sel >= alu::kConstFile) {
        std::snprintf(b, sizeof(b), "C%u%s.%c", sel - alu::kConstFile, s.rel ? "[AR]" : "", kChan[s.chan]);
        text = b;
    } else {
        switch (sel) {
        case alu::kImm0: text = "0.0"; break;
        case alu::kImm1: text = "1.0"; break;
        case alu::kImm1Int: text = "1"; break;
        case alu::kImmMinus1Int: text = "-1"; break;
        case alu::kImmHalf: text = "0.5"; break;
        case alu::kLiteral: {
            const uint32_t v = group.literals[s.chan];
            std::snprintf(b, sizeof(b), "0x%08X(%g)", v, static_cast<double>(std::bit_cast<float>(v)));
            text = b;
            break;
        }
        case alu::kPv: text = std::string("PV.") + kChan[s.chan]; break;
        case alu::kPs: text = "PS"; break;
        default:
            std::snprintf(b, sizeof(b), "S%u.%c", sel, kChan[s.chan]);
            text = b;
        }
    }
    if (s.abs) text = "|" + text + "|";
    if (s.neg) text = "-" + text;
    return text;
}

const char* export_target(const ExportInstruction& e, char* buffer, size_t size) {
    switch (e.type) {
    case cf::kPixel: std::snprintf(buffer, size, "PIX%u", e.array_base); break;
    case cf::kPosition: std::snprintf(buffer, size, "POS%u", e.array_base - 60); break;
    case cf::kParameter: std::snprintf(buffer, size, "PARAM%u", e.array_base); break;
    default: std::snprintf(buffer, size, "?%u", e.array_base); break;
    }
    return buffer;
}

void vtx_text(std::string& out, const VtxInstruction& v) {
    append(out, ".%c%c%c%c, R%u.%c, b%u, fmt %u num %u %s off %u endian %u%s%s",
           kSel[v.dst_sel[0]], kSel[v.dst_sel[1]], kSel[v.dst_sel[2]], kSel[v.dst_sel[3]],
           v.src_gpr, kChan[v.src_sel_x], v.buffer_id, v.data_format, v.num_format,
           v.format_signed ? "signed" : "unsigned", v.offset, v.endian,
           v.fetch_type == vtx::kInstanceData ? " INSTANCE" : "",
           v.use_const_fields ? " CONST_FIELDS" : "");
}

} // namespace

std::string disassemble(const Program& program) {
    std::string out;
    for (const CfInstruction& cf : program.cf) {
        switch (cf.kind) {
        case CfInstruction::kAlu: {
            append(out, "%02u %s: ADDR(%u) CNT(%u)", cf.index, cf_alu_name(cf.inst), cf.addr, cf.count);
            for (int k = 0; k < 2; ++k) {
                if (cf.kcache[k].mode != 0) {
                    const uint32_t lines = cf.kcache[k].mode == 2 ? 2 : 1;
                    append(out, " KCACHE%d(CB%u:%u-%u)", k, cf.kcache[k].bank, cf.kcache[k].addr * 16,
                           (cf.kcache[k].addr + lines) * 16 - 1);
                }
            }
            out += "\n";
            int group_index = 0;
            for (const AluGroup& group : program.alu_clauses[cf.clause]) {
                for (size_t i = 0; i < group.instructions.size(); ++i) {
                    const AluInstruction& in = group.instructions[i];
                    if (i == 0) append(out, "     %3d ", group_index);
                    else out += "         ";
                    std::string dst = in.write ? gpr(in.dst_gpr, in.dst_rel, in.index_mode) + "." + kChan[in.dst_chan]
                                               : std::string("____");
                    append(out, "%c: %-18s %s", kChan[in.slot], in.info->name, dst.c_str());
                    for (int s = 0; s < in.info->sources; ++s) {
                        append(out, ", %s", source(in, in.src[s], group, cf).c_str());
                    }
                    static constexpr const char* kOmod[] = {"", " *2", " *4", " /2"};
                    if (in.omod) out += kOmod[in.omod & 3];
                    if (in.clamp) out += " CLAMP";
                    if (in.pred_sel) append(out, " PRED_SEL_%s", in.pred_sel == alu::kPredZero ? "ZERO" : "ONE");
                    if (in.update_exec_mask) out += " UPDATE_EXEC_MASK";
                    if (in.update_pred) out += " UPDATE_PRED";
                    out += "\n";
                }
                ++group_index;
            }
            break;
        }
        case CfInstruction::kExport: {
            char target[32];
            const ExportInstruction& e = cf.exp;
            if (cf.inst == cf::kExp || cf.inst == cf::kExpDone) {
                append(out, "%02u %s: %s", cf.index, cf_export_name(cf.inst), export_target(e, target, sizeof(target)));
            } else {
                static constexpr const char* kTypes[] = {"WRITE", "WRITE_IND", "WRITE_ACK", "WRITE_IND_ACK"};
                append(out, "%02u %s: %s %u", cf.index, cf_export_name(cf.inst), kTypes[e.type & 3], e.array_base);
            }
            if (e.burst_count) append(out, " (+%u)", e.burst_count);
            if (cf.inst == cf::kExp || cf.inst == cf::kExpDone) {
                append(out, ", R%u.%c%c%c%c", e.rw_gpr, kSel[e.sel[0]], kSel[e.sel[1]], kSel[e.sel[2]], kSel[e.sel[3]]);
            } else {
                append(out, ", R%u mask 0x%X size %u elem %u", e.rw_gpr, e.comp_mask, e.array_size, e.elem_size);
            }
            if (cf.end_of_program) out += " END_OF_PROGRAM";
            out += "\n";
            break;
        }
        case CfInstruction::kNormal: {
            append(out, "%02u %s", cf.index, cf_name(cf.inst));
            switch (cf.inst) {
            case cf::kTex:
            case cf::kVtx:
            case cf::kVtxTc: append(out, ": ADDR(%u) CNT(%u)", cf.addr, cf.count); break;
            case cf::kJump:
            case cf::kElse:
            case cf::kPopJump:
            case cf::kLoopStart:
            case cf::kLoopStartDx10:
            case cf::kLoopStartNoAl:
            case cf::kLoopEnd:
            case cf::kLoopContinue:
            case cf::kLoopBreak:
            case cf::kCall: append(out, " ADDR(%u)", cf.addr); break;
            default: break;
            }
            if (cf.pop_count) append(out, " POP_CNT(%u)", cf.pop_count);
            if (cf.cond) append(out, " COND(%u) CF_CONST(%u)", cf.cond, cf.cf_const);
            if (cf.end_of_program) out += " END_OF_PROGRAM";
            out += "\n";
            if (cf.inst == cf::kTex) {
                for (const TexInstruction& t : program.tex_clauses[cf.clause]) {
                    const char* name = tex::name(t.inst);
                    if (t.vfetch) {
                        append(out, "         %-14s R%u", name, t.vtx.semantic_or_gpr);
                        vtx_text(out, t.vtx);
                        out += "\n";
                        continue;
                    }
                    append(out, "         %-14s R%u.%c%c%c%c, R%u.%c%c%c%c, t%u, s%u", name ? name : "?", t.dst_gpr,
                           kSel[t.dst_sel[0]], kSel[t.dst_sel[1]], kSel[t.dst_sel[2]], kSel[t.dst_sel[3]], t.src_gpr,
                           kSel[t.src_sel[0]], kSel[t.src_sel[1]], kSel[t.src_sel[2]], kSel[t.src_sel[3]],
                           t.resource_id, t.sampler_id);
                    if (t.lod_bias) append(out, " LOD_BIAS(%g)", t.lod_bias / 8.0);
                    if (t.offset[0] || t.offset[1] || t.offset[2]) {
                        append(out, " OFFSET(%g,%g,%g)", t.offset[0] / 2.0, t.offset[1] / 2.0, t.offset[2] / 2.0);
                    }
                    if (t.coord_normalized != 0xF) append(out, " UNNORM(0x%X)", ~t.coord_normalized & 0xF);
                    out += "\n";
                }
            } else if (cf.inst == cf::kVtx || cf.inst == cf::kVtxTc) {
                for (const VtxInstruction& v : program.vtx_clauses[cf.clause]) {
                    if (v.inst == vtx::kSemantic) {
                        append(out, "         SEMANTIC       SEM%u", v.semantic_or_gpr);
                    } else {
                        append(out, "         FETCH          R%u", v.semantic_or_gpr);
                    }
                    vtx_text(out, v);
                    out += "\n";
                }
            }
            break;
        }
        }
    }
    return out;
}

} // namespace cafe::latte
