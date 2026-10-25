#include "latte/translate.h"

#include "latte/shader_abi.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string_view>
#include <vector>

namespace cafe::latte {
namespace {

using namespace isa;

constexpr char kChan[] = "xyzw";

std::string format(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

// Bytes per element of a vertex/buffer fetch data format; 0 if unsupported.
uint32_t fetch_format_bytes(uint32_t data_format) {
    switch (data_format) {
    case 0x01: case 0x02: case 0x03: return 1;                   // 8, 4_4, 3_3_2
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:       // 16, 16F, 8_8, 5_6_5, 6_5_5
    case 0x0A: case 0x0B: case 0x0C: return 2;                   // 1_5_5_5, 4_4_4_4, 5_5_5_1
    case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: return 4;
    case 0x1D: case 0x1E: case 0x1F: case 0x20: return 8;        // 32_32, 32_32F, 16x4, 16x4F
    case 0x2F: case 0x30: return 12;                             // 32_32_32, 32_32_32F
    case 0x22: case 0x23: return 16;                             // 32x4, 32x4F
    default: return 0;
    }
}

class Translator {
public:
    Translator(const Program& program, const Program* fetch, const ShaderEnvironment& env, TranslatedShader& out)
        : p_(program), fetch_(fetch), env_(env), out_(out) {}

    bool run(std::string& error);

private:
    // ------------------------------------------------------------ output
    void line(const std::string& text) {
        body_.append(indent_ * 4, ' ');
        body_ += text;
        body_ += '\n';
    }
    void open(const std::string& text) {
        line(text + " {");
        ++indent_;
    }
    void close(const std::string& suffix = "") {
        --indent_;
        line("}" + suffix);
    }
    void fail(const std::string& message) {
        if (error_.empty()) error_ = message;
    }

    // ---------------------------------------------------------- analysis
    void note_gpr(uint32_t gpr, bool rel) {
        if (rel) relative_ = true;
        max_gpr_ = std::max<int>(max_gpr_, static_cast<int>(gpr));
        if (gpr < kClauseTemps) max_register_ = std::max<int>(max_register_, static_cast<int>(gpr));
    }
    void analyze(const Program& program);

    // ------------------------------------------------------- registers
    // A register read. Relative ones go through rel_load, which reads the
    // register file at constant indices only: indexing the array itself
    // would make the driver keep all of it in (slow) memory instead of
    // registers, 16 ms for one full-screen blur at 1440p.
    std::string gpr(uint32_t index, bool rel, uint32_t index_mode) const {
        if (!rel) return format("R[%u]", index);
        return format("rel_load(%u + %s)", index, index_register(index_mode).c_str());
    }
    // A register write of one channel.
    std::string gpr_store(uint32_t index, bool rel, uint32_t index_mode, char chan, const std::string& value) const {
        if (!rel) return format("R[%u].%c = %s;", index, chan, value.c_str());
        return format("rel_store(%u + %s, %d, %s);", index, index_register(index_mode).c_str(),
                      static_cast<int>(std::string_view("xyzw").find(chan)), value.c_str());
    }
    // TEX/VTX/export relative registers add the loop index aL (ALU
    // operands choose with their INDEX_MODE).
    static constexpr uint32_t kLoopIndex = 4;
    std::string index_register(uint32_t index_mode) const {
        if (index_mode < 4) return format("AR.%c", kChan[index_mode]);
        return "AL";
    }

    // --------------------------------------------------------- control flow
    void emit_program(const Program& program, uint32_t start, bool subroutine);
    void emit_cf(const Program& program, const CfInstruction& cf);
    void emit_alu_clause(const Program& program, const CfInstruction& cf);
    void emit_group(const AluGroup& group, const CfInstruction& cf);
    void emit_tex_clause(const Program& program, const CfInstruction& cf);
    void emit_vtx_clause(const std::vector<VtxInstruction>& clause, bool fetch_shader);
    void emit_export(const CfInstruction& cf);
    void emit_stream_out(const CfInstruction& cf);

    // ---------------------------------------------------------------- ALU
    std::string raw_source(const AluInstruction& in, int s, const AluGroup& group, const CfInstruction& cf);
    std::string float_source(const AluInstruction& in, int s, const AluGroup& group, const CfInstruction& cf);
    std::string int_source(const AluInstruction& in, int s, const AluGroup& group, const CfInstruction& cf) {
        return raw_source(in, s, group, cf);
    }
    std::string uint_source(const AluInstruction& in, int s, const AluGroup& group, const CfInstruction& cf) {
        return "uint(" + raw_source(in, s, group, cf) + ")";
    }
    // GLSL int expression with the result bits of a non-reduction instruction;
    // empty if it has no result.
    std::string alu_result(const AluInstruction& in, const AluGroup& group, const CfInstruction& cf);

    // ------------------------------------------------------------ fetches
    std::string fetch_value(const std::string& buffer, const std::string& offset, uint32_t data_format,
                            uint32_t num_format, bool is_signed, uint32_t endian, bool raw32);
    void emit_vertex_fetch(const VtxInstruction& v);
    void emit_buffer_fetch(const TexInstruction& t);
    void emit_texture(const TexInstruction& t);
    std::string tex_coord(const TexInstruction& t, int i) const;

    // ------------------------------------------------------------- header
    std::string header() const;

    const Program& p_;
    const Program* fetch_;
    const ShaderEnvironment& env_;
    TranslatedShader& out_;
    bool vs_ = false;

    std::string body_;
    int indent_ = 1;
    std::string error_;

    // Registers 123-127 are the clause temporaries (T0-T4), never relative.
    static constexpr uint32_t kClauseTemps = 123;
    int max_gpr_ = 0;
    int max_register_ = 0; // highest below the clause temporaries
    int gpr_count_ = 1;
    bool relative_ = false;
    bool uses_kill_ = false;
    bool uses_gradients_ = false;
    bool uses_cube_ = false;
    bool writes_depth_ = false;
    int max_depth_ = 0;
    int loop_counter_ = 0;
    uint32_t texture_mask_ = 0;
    uint32_t constant_bank_mask_ = 0; // kcache banks read
    uint32_t constant_bank_extent_[16] = {}; // bytes of each bank read (UINT32_MAX: any)
    uint32_t buffer_mask_ = 0;        // buffer resources fetched from
    uint32_t shadow_mask_ = 0;
    bool uses_registers_ = false;

    // The static push depth while emitting, and the depth each jump expects
    // at its target.
    int depth_ = 0;
    std::map<uint32_t, int> expected_depth_;
    struct Loop {
        int depth;
        int id;
    };
    std::vector<Loop> loops_;
    // VS parameter export -> pixel shader input locations it feeds.
    std::vector<int> param_locations_[32];
    std::vector<int> stack_vars_; // per function: deepest stack variable used
};

// ---------------------------------------------------------------- analysis

void Translator::analyze(const Program& program) {
    for (const CfInstruction& cf : program.cf) {
        if (cf.kind == CfInstruction::kExport) {
            note_gpr(cf.exp.rw_gpr + cf.exp.burst_count, cf.exp.rw_rel);
            if (cf.exp.type == cf::kPixel && cf.exp.array_base == 61 &&
                (cf.inst == cf::kExp || cf.inst == cf::kExpDone)) {
                writes_depth_ = true;
            }
        }
    }
    for (const auto& clause : program.alu_clauses) {
        for (const AluGroup& group : clause) {
            for (const AluInstruction& in : group.instructions) {
                for (int s = 0; s < (in.op3 ? 3 : 2); ++s) {
                    if (in.src[s].sel <= alu::kGprLast) note_gpr(in.src[s].sel, in.src[s].rel);
                }
                note_gpr(in.dst_gpr, in.dst_rel);
                if (in.info->flags & alu::kKill) uses_kill_ = true;
                if (!in.op3 && in.opcode == alu::op2_code::kCube) uses_cube_ = true;
            }
        }
    }
    for (const auto& clause : program.tex_clauses) {
        for (const TexInstruction& t : clause) {
            if (t.vfetch) {
                note_gpr(t.vtx.src_gpr, t.vtx.src_rel);
                note_gpr(t.vtx.semantic_or_gpr, t.vtx.dst_rel);
                continue;
            }
            note_gpr(t.src_gpr, t.src_rel);
            note_gpr(t.dst_gpr, t.dst_rel);
            if (t.inst >= tex::kSampleC && t.inst <= tex::kSampleCGLz && t.resource_id < 32) {
                shadow_mask_ |= 1u << t.resource_id;
            }
            if (t.inst >= tex::kGetGradientsH && t.inst <= tex::kSetGradientsV) uses_gradients_ = true;
            if (t.inst == tex::kSampleG || t.inst == tex::kSampleCG) uses_gradients_ = true;
        }
    }
    for (const auto& clause : program.vtx_clauses) {
        for (const VtxInstruction& v : clause) {
            note_gpr(v.src_gpr, v.src_rel);
            if (v.inst != vtx::kSemantic) note_gpr(v.semantic_or_gpr, v.dst_rel);
        }
    }
}

// ---------------------------------------------------------------- ALU code

std::string Translator::raw_source(const AluInstruction& in, int s, const AluGroup& group, const CfInstruction& cf) {
    const AluSource& src = in.src[s];
    const uint32_t sel = src.sel;
    const char c = kChan[src.chan];
    if (sel <= alu::kGprLast) return gpr(sel, src.rel, in.index_mode) + "." + c;
    if (sel >= alu::kKcacheBank0 && sel < alu::kKcacheBank1 + 32) {
        const int bank = sel >= alu::kKcacheBank1 ? 1 : 0;
        const KcacheLock& lock = cf.kcache[bank];
        const uint32_t index = lock.addr * 16 + (sel - (bank ? alu::kKcacheBank1 : alu::kKcacheBank0));
        if (lock.mode == 0 || lock.mode == 3) {
            fail(format("kcache bank %d used with lock mode %u", bank, lock.mode));
            return "0";
        }
        if (env_.uniform_registers) {
            fail("kcache constants in uniform register mode");
            return "0";
        }
        std::string i = format("%uu", index);
        if (src.rel) i = format("uint(%u + %s)", index, index_register(in.index_mode).c_str());
        constant_bank_mask_ |= 1u << (lock.bank & 15);
        // The bytes of the block the shader can read: up to this constant,
        // or anywhere when the index is relative.
        uint32_t& extent = constant_bank_extent_[lock.bank & 15];
        extent = src.rel ? UINT32_MAX : std::max(extent, (index + 1) * 16);
        return format("kc(%uu, %s).%c", lock.bank, i.c_str(), c);
    }
    if (sel >= alu::kConstFile) {
        if (!env_.uniform_registers) {
            fail("constant file read in uniform block mode");
            return "0";
        }
        uses_registers_ = true;
        const uint32_t index = sel - alu::kConstFile;
        if (src.rel) {
            return format("int(regs.c[(%u + %s) & 255].%c)", index, index_register(in.index_mode).c_str(), c);
        }
        return format("int(regs.c[%u].%c)", index, c);
    }
    switch (sel) {
    case alu::kImm0: return "0";
    case alu::kImm1: return "0x3F800000";
    case alu::kImm1Int: return "1";
    case alu::kImmMinus1Int: return "-1";
    case alu::kImmHalf: return "0x3F000000";
    case alu::kLiteral: return format("int(0x%08Xu)", group.literals[src.chan]);
    case alu::kPv: return format("PV.%c", c);
    case alu::kPs: return "PSv";
    default:
        fail(format("unsupported ALU source select %u", sel));
        return "0";
    }
}

std::string Translator::float_source(const AluInstruction& in, int s, const AluGroup& group,
                                     const CfInstruction& cf) {
    const AluSource& src = in.src[s];
    std::string text;
    switch (src.sel) {
    case alu::kImm0: text = "0.0"; break;
    case alu::kImm1: text = "1.0"; break;
    case alu::kImmHalf: text = "0.5"; break;
    default: text = "intBitsToFloat(" + raw_source(in, s, group, cf) + ")"; break;
    }
    if (src.abs) text = "abs(" + text + ")";
    if (src.neg) text = "(-" + text + ")";
    return text;
}

std::string Translator::alu_result(const AluInstruction& in, const AluGroup& g, const CfInstruction& cf) {
    using namespace alu::op2_code;
    using namespace alu::op3_code;
    const auto F = [&](int s) { return float_source(in, s, g, cf); };
    const auto I = [&](int s) { return int_source(in, s, g, cf); };
    const auto U = [&](int s) { return uint_source(in, s, g, cf); };
    const auto fb = [](const std::string& e) { return "floatBitsToInt(" + e + ")"; };
    // A source passed through unchanged unless it has modifiers.
    const auto pass = [&](int s) {
        return (in.src[s].abs || in.src[s].neg) ? fb(F(s)) : I(s);
    };
    std::string f; // float result expression, if the instruction has one
    if (in.op3) {
        switch (in.opcode) {
        case kMulAdd: f = "mul_ni(" + F(0) + ", " + F(1) + ") + " + F(2); break;
        case kMulAddM2: f = "(mul_ni(" + F(0) + ", " + F(1) + ") + " + F(2) + ") * 2.0"; break;
        case kMulAddM4: f = "(mul_ni(" + F(0) + ", " + F(1) + ") + " + F(2) + ") * 4.0"; break;
        case kMulAddD2: f = "(mul_ni(" + F(0) + ", " + F(1) + ") + " + F(2) + ") * 0.5"; break;
        case kMulAddIeee: f = F(0) + " * " + F(1) + " + " + F(2); break;
        case kMulAddIeeeM2: f = "(" + F(0) + " * " + F(1) + " + " + F(2) + ") * 2.0"; break;
        case kMulAddIeeeM4: f = "(" + F(0) + " * " + F(1) + " + " + F(2) + ") * 4.0"; break;
        case kMulAddIeeeD2: f = "(" + F(0) + " * " + F(1) + " + " + F(2) + ") * 0.5"; break;
        case kFma: f = "fma(" + F(0) + ", " + F(1) + ", " + F(2) + ")"; break;
        case kCndE:
        case kCndGt:
        case kCndGe: {
            const char* op = in.opcode == kCndE ? "==" : in.opcode == kCndGt ? ">" : ">=";
            std::string r = "((" + F(0) + " " + op + " 0.0) ? " + pass(1) + " : " + pass(2) + ")";
            if (in.clamp) r = fb("clamp(intBitsToFloat(" + r + "), 0.0, 1.0)");
            return r;
        }
        case kCndEInt: return "((" + I(0) + " == 0) ? " + I(1) + " : " + I(2) + ")";
        case kCndGtInt: return "((" + I(0) + " > 0) ? " + I(1) + " : " + I(2) + ")";
        case kCndGeInt: return "((" + I(0) + " >= 0) ? " + I(1) + " : " + I(2) + ")";
        case kBfeUint: return "int(bitfieldExtract(" + U(0) + ", " + I(1) + " & 31, " + I(2) + " & 31))";
        case kBfeInt: return "bitfieldExtract(" + I(0) + ", " + I(1) + " & 31, " + I(2) + " & 31)";
        case kBfiInt: return "((" + I(0) + " & " + I(1) + ") | (~" + I(0) + " & " + I(2) + "))";
        default:
            fail(format("unsupported OP3 instruction %s", in.info->name));
            return "0";
        }
    } else {
        switch (in.opcode) {
        case kAdd: f = F(0) + " + " + F(1); break;
        case kMul: f = "mul_ni(" + F(0) + ", " + F(1) + ")"; break;
        case kMulIeee: f = F(0) + " * " + F(1); break;
        case kMax: case kMaxDx10: f = "max(" + F(0) + ", " + F(1) + ")"; break;
        case kMin: case kMinDx10: f = "min(" + F(0) + ", " + F(1) + ")"; break;
        case kSetE: f = "((" + F(0) + " == " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kSetGt: f = "((" + F(0) + " > " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kSetGe: f = "((" + F(0) + " >= " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kSetNe: f = "((" + F(0) + " != " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kSetEDx10: return "((" + F(0) + " == " + F(1) + ") ? -1 : 0)";
        case kSetGtDx10: return "((" + F(0) + " > " + F(1) + ") ? -1 : 0)";
        case kSetGeDx10: return "((" + F(0) + " >= " + F(1) + ") ? -1 : 0)";
        case kSetNeDx10: return "((" + F(0) + " != " + F(1) + ") ? -1 : 0)";
        case kFract: f = "fract(" + F(0) + ")"; break;
        case kTrunc: f = "trunc(" + F(0) + ")"; break;
        case kCeil: f = "ceil(" + F(0) + ")"; break;
        case kRndne: f = "roundEven(" + F(0) + ")"; break;
        case kFloor: f = "floor(" + F(0) + ")"; break;
        case kMova: case kMovaFloor: case kMovaInt: return pass(0); // AR is set by the group
        case kMov:
            if (!in.src[0].abs && !in.src[0].neg && !in.clamp && in.omod == 0) return I(0);
            f = F(0);
            break;
        case kNop: return "";
        case kPredSetE: case kPredSetEPush: f = "((" + F(0) + " == " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kPredSetGt: case kPredSetGtPush: f = "((" + F(0) + " > " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kPredSetGe: case kPredSetGePush: f = "((" + F(0) + " >= " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kPredSetNe: case kPredSetNePush: f = "((" + F(0) + " != " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kPredSetEInt: case kPredSetEPushInt: return "((" + I(0) + " == " + I(1) + ") ? 1 : 0)";
        case kPredSetGtInt: case kPredSetGtPushInt: return "((" + I(0) + " > " + I(1) + ") ? 1 : 0)";
        case kPredSetGeInt: case kPredSetGePushInt: return "((" + I(0) + " >= " + I(1) + ") ? 1 : 0)";
        case kPredSetNeInt: case kPredSetNePushInt: return "((" + I(0) + " != " + I(1) + ") ? 1 : 0)";
        case kPredSetLtPushInt: return "((" + I(0) + " < " + I(1) + ") ? 1 : 0)";
        case kPredSetLePushInt: return "((" + I(0) + " <= " + I(1) + ") ? 1 : 0)";
        case kPredSetGtUint: return "((" + U(0) + " > " + U(1) + ") ? 1 : 0)";
        case kPredSetGeUint: return "((" + U(0) + " >= " + U(1) + ") ? 1 : 0)";
        case kKillE: f = "((" + F(0) + " == " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kKillGt: f = "((" + F(0) + " > " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kKillGe: f = "((" + F(0) + " >= " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kKillNe: f = "((" + F(0) + " != " + F(1) + ") ? 1.0 : 0.0)"; break;
        case kKillEInt: return "((" + I(0) + " == " + I(1) + ") ? 1 : 0)";
        case kKillGtInt: return "((" + I(0) + " > " + I(1) + ") ? 1 : 0)";
        case kKillGeInt: return "((" + I(0) + " >= " + I(1) + ") ? 1 : 0)";
        case kKillNeInt: return "((" + I(0) + " != " + I(1) + ") ? 1 : 0)";
        case kKillGtUint: return "((" + U(0) + " > " + U(1) + ") ? 1 : 0)";
        case kKillGeUint: return "((" + U(0) + " >= " + U(1) + ") ? 1 : 0)";
        case kAndInt: return "(" + I(0) + " & " + I(1) + ")";
        case kOrInt: return "(" + I(0) + " | " + I(1) + ")";
        case kXorInt: return "(" + I(0) + " ^ " + I(1) + ")";
        case kNotInt: return "(~" + I(0) + ")";
        case kAddInt: return "(" + I(0) + " + " + I(1) + ")";
        case kSubInt: return "(" + I(0) + " - " + I(1) + ")";
        case kMaxInt: return "max(" + I(0) + ", " + I(1) + ")";
        case kMinInt: return "min(" + I(0) + ", " + I(1) + ")";
        case kMaxUint: return "int(max(" + U(0) + ", " + U(1) + "))";
        case kMinUint: return "int(min(" + U(0) + ", " + U(1) + "))";
        case kSetEInt: return "((" + I(0) + " == " + I(1) + ") ? -1 : 0)";
        case kSetGtInt: return "((" + I(0) + " > " + I(1) + ") ? -1 : 0)";
        case kSetGeInt: return "((" + I(0) + " >= " + I(1) + ") ? -1 : 0)";
        case kSetNeInt: return "((" + I(0) + " != " + I(1) + ") ? -1 : 0)";
        case kSetGtUint: return "((" + U(0) + " > " + U(1) + ") ? -1 : 0)";
        case kSetGeUint: return "((" + U(0) + " >= " + U(1) + ") ? -1 : 0)";
        case kExpIeee: f = "exp2(" + F(0) + ")"; break;
        case kLogClamped: f = "max(log2(" + F(0) + "), -3.40282347e38)"; break;
        case kLogIeee: f = "log2(" + F(0) + ")"; break;
        case kRecipClamped: case kRecipFf: f = "clamp(1.0 / " + F(0) + ", -3.40282347e38, 3.40282347e38)"; break;
        case kRecipIeee: f = "1.0 / " + F(0); break;
        case kRecipSqrtClamped: case kRecipSqrtFf: f = "min(inversesqrt(" + F(0) + "), 3.40282347e38)"; break;
        case kRecipSqrtIeee: f = "inversesqrt(" + F(0) + ")"; break;
        case kSqrtIeee: f = "sqrt(" + F(0) + ")"; break;
        case kFltToInt: return "int(" + F(0) + ")";
        case kFltToUint: return "int(uint(max(" + F(0) + ", 0.0)))";
        case kIntToFlt: return fb("float(" + I(0) + ")");
        case kUintToFlt: return fb("float(" + U(0) + ")");
        case kSin: f = "sin(" + F(0) + " * 6.28318530718)"; break;
        case kCos: f = "cos(" + F(0) + " * 6.28318530718)"; break;
        case kAshrInt: return "(" + I(0) + " >> (" + I(1) + " & 31))";
        case kLshrInt: return "int(" + U(0) + " >> (" + U(1) + " & 31u))";
        case kLshlInt: return "(" + I(0) + " << (" + I(1) + " & 31))";
        case kMulloInt: return "(" + I(0) + " * " + I(1) + ")";
        case kMulloUint: return "int(" + U(0) + " * " + U(1) + ")";
        case kMulhiInt: return "mulhi_i(" + I(0) + ", " + I(1) + ")";
        case kMulhiUint: return "int(mulhi_u(" + U(0) + ", " + U(1) + "))";
        default:
            fail(format("unsupported OP2 instruction %s", in.info->name));
            return "0";
        }
        static constexpr const char* kOmod[] = {"", " * 2.0", " * 4.0", " * 0.5"};
        if (in.omod != 0) f = "(" + f + ")" + kOmod[in.omod & 3];
    }
    if (in.clamp) f = "clamp(" + f + ", 0.0, 1.0)";
    return fb(f);
}

void Translator::emit_group(const AluGroup& group, const CfInstruction& cf) {
    using namespace alu::op2_code;
    open("");
    std::string results[5];
    bool has[5] = {};
    // Reductions use the four vector slots together.
    const AluInstruction* reduction[4] = {};
    int reduction_count = 0;
    for (const AluInstruction& in : group.instructions) {
        if (!in.op3 && (in.info->flags & alu::kReduction) && in.slot < 4) {
            reduction[in.slot] = &in;
            ++reduction_count;
        }
    }
    if (reduction_count != 0) {
        if (reduction_count != 4 || !reduction[0] || !reduction[1] || !reduction[2] || !reduction[3]) {
            fail("reduction instruction without all four vector slots");
        } else {
            const uint32_t op = reduction[0]->opcode;
            std::string a[4], b[4];
            for (int i = 0; i < 4; ++i) {
                a[i] = float_source(*reduction[i], 0, group, cf);
                b[i] = float_source(*reduction[i], 1, group, cf);
            }
            if (op == kDot4) {
                line("float red = mul_ni(" + a[0] + ", " + b[0] + ") + mul_ni(" + a[1] + ", " + b[1] + ") + mul_ni(" +
                     a[2] + ", " + b[2] + ") + mul_ni(" + a[3] + ", " + b[3] + ");");
                for (int i = 0; i < 4; ++i) results[i] = "red";
            } else if (op == kDot4Ieee) {
                line("float red = dot(vec4(" + a[0] + ", " + a[1] + ", " + a[2] + ", " + a[3] + "), vec4(" + b[0] +
                     ", " + b[1] + ", " + b[2] + ", " + b[3] + "));");
                for (int i = 0; i < 4; ++i) results[i] = "red";
            } else if (op == kMax4) {
                line("float red = max(max(" + a[0] + ", " + a[1] + "), max(" + a[2] + ", " + a[3] + "));");
                for (int i = 0; i < 4; ++i) results[i] = "red";
            } else if (op == kCube) {
                // Direction (x, y, z) = (src1.y, src1.x, src0.x) of slots x/y.
                line("vec4 red = cube(vec3(" + b[1] + ", " + b[0] + ", " + a[0] + "));");
                for (int i = 0; i < 4; ++i) results[i] = format("red.%c", kChan[i]);
            } else {
                fail("unsupported reduction instruction");
            }
            for (int i = 0; i < 4; ++i) {
                const AluInstruction& in = *reduction[i];
                std::string r = results[i];
                static constexpr const char* kOmod[] = {"", " * 2.0", " * 4.0", " * 0.5"};
                if (in.omod != 0) r = "(" + r + kOmod[in.omod & 3] + ")";
                if (in.clamp) r = "clamp(" + r + ", 0.0, 1.0)";
                line(format("int t%c = floatBitsToInt(%s);", kChan[i], r.c_str()));
                has[i] = true;
            }
        }
    }
    for (const AluInstruction& in : group.instructions) {
        if (!in.op3 && (in.info->flags & alu::kReduction) && in.slot < 4) continue;
        const std::string value = alu_result(in, group, cf);
        if (value.empty()) continue;
        const char name = in.slot < 4 ? kChan[in.slot] : 't';
        line(format("int t%c = %s;", name, value.c_str()));
        has[in.slot] = true;
    }
    // Side effects in slot order: predicates, kills, AR, then GPR writes.
    for (const AluInstruction& in : group.instructions) {
        if (!has[in.slot]) continue;
        const char name = in.slot < 4 ? kChan[in.slot] : 't';
        const uint32_t flags = in.info->flags;
        if (!in.op3 && (flags & alu::kPredSet)) {
            if (in.opcode == kPredSetInv || in.opcode == kPredSetPop || in.opcode == kPredSetClr ||
                in.opcode == kPredSetRestore) {
                fail(format("unsupported predicate instruction %s", in.info->name));
            }
            // Float compares produce 1.0, integer ones 1, when true.
            if (in.update_pred) line(format("pred = t%c != 0;", name));
            if (in.update_exec_mask) line(format("act = t%c != 0;", name));
        }
        if (!in.op3 && (flags & alu::kKill)) {
            if (env_.stage != Stage::kPixel) fail("KILL outside a pixel shader");
            line(format("if (t%c != 0) demote;", name));
        }
        if (!in.op3 && (in.opcode == kMova || in.opcode == kMovaFloor || in.opcode == kMovaInt)) {
            const std::string v = in.opcode == kMovaInt  ? format("t%c", name)
                                  : in.opcode == kMova   ? format("int(round(intBitsToFloat(t%c)))", name)
                                                         : format("int(floor(intBitsToFloat(t%c)))", name);
            line(format("ARn.%c = clamp(%s, -256, 255);", kChan[in.dst_chan], v.c_str()));
        }
    }
    for (const AluInstruction& in : group.instructions) {
        if (!has[in.slot] || !in.write) continue;
        const char name = in.slot < 4 ? kChan[in.slot] : 't';
        std::string write = gpr_store(in.dst_gpr, in.dst_rel, in.index_mode, kChan[in.dst_chan], format("t%c", name));
        if (in.pred_sel == alu::kPredZero) write = "if (!pred) " + write;
        if (in.pred_sel == alu::kPredOne) write = "if (pred) " + write;
        line(write);
    }
    for (int slot = 0; slot < 4; ++slot) {
        if (has[slot]) line(format("PV.%c = t%c;", kChan[slot], kChan[slot]));
    }
    if (has[4]) line("PSv = tt;");
    for (const AluInstruction& in : group.instructions) {
        if (!in.op3 && (in.opcode == kMova || in.opcode == kMovaFloor || in.opcode == kMovaInt)) {
            line("AR = ARn;");
            break;
        }
    }
    close();
}

void Translator::emit_alu_clause(const Program& program, const CfInstruction& cf) {
    for (const AluGroup& group : program.alu_clauses[cf.clause]) emit_group(group, cf);
}

// ----------------------------------------------------------------- fetches

// Emits `uvec4 w`, the raw words of an element at `offset` (a uint byte
// offset) in `buffer` (a uvec4 buffer descriptor), into the current block
// and returns an ivec4 expression for the element's four components.
std::string Translator::fetch_value(const std::string& buffer, const std::string& offset, uint32_t data_format,
                                    uint32_t num_format, bool is_signed, uint32_t endian, bool raw32) {
    const uint32_t bytes = fetch_format_bytes(data_format);
    if (bytes == 0) {
        fail(format("unsupported fetch data format 0x%X", data_format));
        return "ivec4(0)";
    }
    line("uint off = " + offset + ";");
    std::string words;
    const uint32_t count = (bytes + 3) / 4;
    for (uint32_t i = 0; i < 4; ++i) {
        if (i) words += ", ";
        words += i < count ? format("gload(%s, off + %uu, %uu)", buffer.c_str(), i * 4, std::min(4u, bytes - i * 4))
                           : std::string("0u");
    }
    std::string w = "uvec4(" + words + ")";
    switch (endian) { // SQ_ENDIAN
    case 1: w = "swap16(" + w + ")"; break;
    case 2: w = "swap32(" + w + ")"; break;
    case 3: w = "swap64(" + w + ")"; break;
    default: break;
    }
    line("uvec4 w = " + w + ";");
    // Components (uvec4), their count and bit width.
    std::string c;
    int bits = 32;
    uint32_t comps = 4;
    bool is_float = false;
    switch (data_format) {
    case 0x01: c = "uvec4(w.x & 0xFFu, 0u, 0u, 0u)"; bits = 8; comps = 1; break;
    case 0x05: c = "uvec4(w.x & 0xFFFFu, 0u, 0u, 0u)"; bits = 16; comps = 1; break;
    case 0x06: c = "uvec4(floatBitsToUint(unpackHalf2x16(w.x).x), 0u, 0u, 0u)"; is_float = true; comps = 1; break;
    case 0x07: c = "uvec4(w.x & 0xFFu, (w.x >> 8) & 0xFFu, 0u, 0u)"; bits = 8; comps = 2; break;
    case 0x0D: c = "uvec4(w.x, 0u, 0u, 0u)"; comps = 1; break;
    case 0x0E: c = "uvec4(w.x, 0u, 0u, 0u)"; is_float = true; comps = 1; break;
    case 0x0F: c = "uvec4(w.x & 0xFFFFu, w.x >> 16, 0u, 0u)"; bits = 16; comps = 2; break;
    case 0x10: c = "uvec4(floatBitsToUint(unpackHalf2x16(w.x)), 0u, 0u)"; is_float = true; comps = 2; break;
    case 0x1A: c = "uvec4(w.x & 0xFFu, (w.x >> 8) & 0xFFu, (w.x >> 16) & 0xFFu, w.x >> 24)"; bits = 8; break;
    case 0x1B: // 10_10_10_2: the 2-bit W is normalized/converted with 2 bits
        c = "uvec4(w.x & 0x3FFu, (w.x >> 10) & 0x3FFu, (w.x >> 20) & 0x3FFu, w.x >> 30)";
        bits = 10;
        break;
    case 0x1D: c = "uvec4(w.x, w.y, 0u, 0u)"; comps = 2; break;
    case 0x1E: c = "uvec4(w.x, w.y, 0u, 0u)"; is_float = true; comps = 2; break;
    case 0x1F: c = "uvec4(w.x & 0xFFFFu, w.x >> 16, w.y & 0xFFFFu, w.y >> 16)"; bits = 16; break;
    case 0x20:
        c = "uvec4(floatBitsToUint(unpackHalf2x16(w.x)), floatBitsToUint(unpackHalf2x16(w.y)))";
        is_float = true;
        break;
    case 0x2F: c = "uvec4(w.xyz, 0u)"; comps = 3; break;
    case 0x30: c = "uvec4(w.xyz, 0u)"; is_float = true; comps = 3; break;
    case 0x22: c = "w"; break;
    case 0x23: c = "w"; is_float = true; break;
    default:
        fail(format("unsupported fetch data format 0x%X", data_format));
        return "ivec4(0)";
    }
    if (data_format == 0x1B && num_format != 1 && !raw32) {
        fail("10_10_10_2 fetch conversion"); // W has its own width
    }
    std::string value;
    if (is_float || raw32 || (bits == 32 && num_format != 2)) {
        value = "ivec4(" + c + ")";
    } else if (num_format == 1) { // integer
        value = is_signed ? format("bitfieldExtract(ivec4(%s), 0, %d)", c.c_str(), bits) : "ivec4(" + c + ")";
    } else if (num_format == 2) { // scaled
        value = is_signed ? format("floatBitsToInt(vec4(bitfieldExtract(ivec4(%s), 0, %d)))", c.c_str(), bits)
                          : "floatBitsToInt(vec4(" + c + "))";
    } else { // normalized
        const double max_unsigned = static_cast<double>((uint64_t{1} << bits) - 1);
        const double max_signed = static_cast<double>((uint64_t{1} << (bits - 1)) - 1);
        value = is_signed ? format("floatBitsToInt(max(vec4(bitfieldExtract(ivec4(%s), 0, %d)) / %.1f, -1.0))",
                                   c.c_str(), bits, max_signed)
                          : format("floatBitsToInt(vec4(%s) / %.1f)", c.c_str(), max_unsigned);
    }
    // Missing components read as 0, and W as 1.
    if (comps < 4 && !raw32) {
        static constexpr const char* kMasks[] = {"", "x", "xy", "xyz"};
        const char* one = (num_format == 1 && !is_float) ? "1" : "0x3F800000";
        std::string parts;
        for (uint32_t i = comps; i < 4; ++i) parts += std::string(", ") + (i == 3 ? one : "0");
        value = format("ivec4((%s).%s%s)", value.c_str(), kMasks[comps], parts.c_str());
    }
    return value;
}

void Translator::emit_vertex_fetch(const VtxInstruction& v) {
    if (v.inst != vtx::kSemantic && v.inst != vtx::kFetch) {
        fail(format("unsupported vertex fetch instruction %u", v.inst));
        return;
    }
    int dst = -1;
    if (v.inst == vtx::kSemantic) {
        for (int n = 0; n < 32; ++n) {
            if (env_.vtx_semantic[n] == v.semantic_or_gpr) {
                dst = n + 1;
                break;
            }
        }
        if (dst < 0) return; // not an input of this vertex shader
    } else {
        dst = v.semantic_or_gpr;
    }
    if (v.use_const_fields) {
        fail("vertex fetch with formats from the resource");
        return;
    }
    const int buffer = static_cast<int>(v.buffer_id) - 160;
    const std::string b = (buffer >= 0 && buffer < 16) ? format("dc.vb[%d]", buffer) : std::string("uvec4(0)");
    std::string index;
    if (v.fetch_type == vtx::kInstanceData) {
        switch (v.src_sel_x) {
        case 1: index = "uint(gl_InstanceIndex) / max(dc.vtx.z, 1u)"; break;
        case 2: index = "uint(gl_InstanceIndex) / max(dc.vtx.w, 1u)"; break;
        default: index = "uint(gl_InstanceIndex)"; break;
        }
        index = "(" + index + " + dc.vtx.y)";
    } else {
        index = format("uint(%s.%c)", gpr(v.src_gpr, v.src_rel, kLoopIndex).c_str(), kChan[v.src_sel_x & 3]);
        if (v.fetch_type != vtx::kNoIndexOffset) index = "(" + index + " + dc.vtx.x)";
    }
    open("");
    const std::string value = fetch_value(b, index + " * " + b + ".w + " + format("%uu", v.offset), v.data_format,
                                          v.num_format, v.format_signed, v.endian, false);
    line("ivec4 f = " + value + ";");
    const char* one = v.num_format == 1 ? "1" : "0x3F800000";
    for (int i = 0; i < 4; ++i) {
        const uint8_t sel = v.dst_sel[i];
        if (sel == kSelMask) continue;
        const std::string value = sel < 4 ? format("f.%c", kChan[sel]) : sel == kSel1 ? std::string(one) : "0";
        line(gpr_store(dst, v.dst_rel, kLoopIndex, kChan[i], value));
    }
    close();
}

void Translator::emit_buffer_fetch(const TexInstruction& t) {
    const VtxInstruction& v = t.vtx;
    const int slot = static_cast<int>(v.buffer_id) - 128;
    const char* stage = vs_ ? "vs" : "ps";
    const std::string b = (slot >= 0 && slot < 16) ? format("dc.%s_buf[%d]", stage, slot) : "uvec4(0)";
    if (slot >= 0 && slot < 16) buffer_mask_ |= 1u << slot;
    const std::string index = format("uint(%s.%c)", gpr(v.src_gpr, v.src_rel, kLoopIndex).c_str(), kChan[v.src_sel_x & 3]);
    // Uniform blocks GX2 binds as buffers hold raw 32-bit words (32_32_32_32).
    const uint32_t data_format = v.use_const_fields ? 0x22 : v.data_format;
    open("");
    const std::string value = fetch_value(b, index + " * " + b + ".w + " + format("%uu", v.offset), data_format,
                                          v.num_format, v.format_signed, v.use_const_fields ? 0 : v.endian,
                                          v.use_const_fields);
    line("ivec4 f = " + value + ";");
    const int dst = v.semantic_or_gpr;
    for (int i = 0; i < 4; ++i) {
        const uint8_t sel = v.dst_sel[i];
        if (sel == kSelMask) continue;
        const std::string value = sel < 4 ? format("f.%c", kChan[sel]) : sel == kSel1 ? "0x3F800000" : "0";
        line(gpr_store(dst, v.dst_rel, kLoopIndex, kChan[i], value));
    }
    close();
}

// --------------------------------------------------------------- textures

std::string Translator::tex_coord(const TexInstruction& t, int i) const {
    const uint8_t sel = t.src_sel[i];
    if (sel < 4) return format("intBitsToFloat(%s.%c)", gpr(t.src_gpr, t.src_rel, kLoopIndex).c_str(), kChan[sel]);
    return sel == kSel1 ? "1.0" : "0.0";
}

void Translator::emit_texture(const TexInstruction& t) {
    using namespace tex;
    const uint32_t slot = t.resource_id;
    if (slot >= abi::kTextureSlots) {
        fail(format("texture resource %u out of range", slot));
        return;
    }
    const ShaderEnvironment::Texture& tx = env_.textures[slot];
    const std::string sampler = format("tex%u", slot);
    const auto store = [&](int i, const std::string& value) { line(gpr_store(t.dst_gpr, t.dst_rel, kLoopIndex, kChan[i], value)); };
    const bool integer = tx.kind != TextureKind::kFloat;
    switch (t.inst) {
    case kSetGradientsH:
    case kSetGradientsV:
        line(format("%s = vec4(%s, %s, %s, %s);", t.inst == kSetGradientsH ? "gradH" : "gradV",
                    tex_coord(t, 0).c_str(), tex_coord(t, 1).c_str(), tex_coord(t, 2).c_str(), tex_coord(t, 3).c_str()));
        return;
    case kGetGradientsH:
    case kGetGradientsV: {
        const char* fn = t.inst == kGetGradientsH ? "dFdx" : "dFdy";
        open("");
        line(format("vec4 r = %s(vec4(%s, %s, %s, %s));", fn, tex_coord(t, 0).c_str(), tex_coord(t, 1).c_str(),
                    tex_coord(t, 2).c_str(), tex_coord(t, 3).c_str()));
        for (int i = 0; i < 4; ++i) {
            const uint8_t sel = t.dst_sel[i];
            if (sel == kSelMask) continue;
            const std::string v = sel < 4 ? format("floatBitsToInt(r.%c)", kChan[sel]) : sel == kSel1 ? "0x3F800000" : "0";
            store(i, v);
        }
        close();
        return;
    }
    default: break;
    }
    texture_mask_ |= 1u << slot;
    const uint32_t dim = tx.dim;
    // Coordinates by dimension: count of coordinate components, whether the
    // last is an array layer.
    int coords = 2;
    bool array = false, cube = false;
    switch (dim) {
    case 0: coords = 1; break;                // 1D
    case 1: case 6: coords = 2; break;        // 2D, 2D MSAA
    case 2: coords = 3; break;                // 3D
    case 3: coords = 3; cube = true; break;   // cube
    case 4: coords = 2; array = true; break;  // 1D array
    case 5: case 7: coords = 3; array = true; break; // 2D array
    default: fail(format("texture dimension %u", dim)); return;
    }
    open("");
    // Unnormalized coordinates are in texels.
    const uint32_t normalized = t.coord_normalized;
    const int spatial = array ? coords - 1 : coords;
    bool unnormalized = false;
    if (!cube) {
        for (int i = 0; i < spatial; ++i) unnormalized |= !(normalized & (1u << i));
    }
    std::string c[4];
    for (int i = 0; i < 4; ++i) c[i] = tex_coord(t, i);
    std::string coord;
    if (cube) {
        uses_cube_ = true;
        coord = "cube_dir(vec2(" + c[0] + ", " + c[1] + "), " + c[2] + ")";
    } else {
        static constexpr const char* kVec[] = {"", "float", "vec2", "vec3", "vec4"};
        coord = std::string(kVec[coords]) + "(";
        for (int i = 0; i < coords; ++i) {
            if (i) coord += ", ";
            coord += c[i];
        }
        coord += ")";
        if (unnormalized) {
            line(format("vec%d size = vec%d(textureSize(%s, 0)) / tex_scale(%uu);", std::max(coords, 2), std::max(coords, 2),
                        sampler.c_str(), slot));
            std::string scaled = std::string(kVec[coords]) + "(";
            for (int i = 0; i < coords; ++i) {
                if (i) scaled += ", ";
                const bool scale = i < spatial && !(normalized & (1u << i));
                scaled += scale ? format("%s / float(size.%c)", c[i].c_str(), kChan[i]) : c[i];
            }
            coord = scaled + ")";
        }
    }
    // Texel offsets in half texels; whole texels become textureOffset.
    bool has_offset = false, whole = true;
    for (int i = 0; i < 3; ++i) {
        if (t.offset[i] != 0) has_offset = true;
        if (t.offset[i] & 1) whole = false;
    }
    std::string offset;
    std::string scaled_coord; // upscaled textures: the offset in the title's texels, as coordinates
    if (has_offset && !cube) {
        if (whole) {
            static constexpr const char* kIvec[] = {"", "", "ivec2", "ivec3"};
            static constexpr const char* kVec[] = {"", "float", "vec2", "vec3"};
            std::string o;
            if (spatial == 1) o = format("%d", t.offset[0] / 2);
            else if (spatial == 2) o = format("%s(%d, %d)", kIvec[2], t.offset[0] / 2, t.offset[1] / 2);
            else o = format("%s(%d, %d, %d)", kIvec[3], t.offset[0] / 2, t.offset[1] / 2, t.offset[2] / 2);
            offset = ", " + o;
            if (coords == spatial) {
                scaled_coord = format("(%s + %s(%s) * tex_scale(%uu) / %s(textureSize(%s, 0)))", coord.c_str(),
                                      kVec[spatial], o.c_str(), slot, kVec[spatial], sampler.c_str());
            } else {
                // Arrays: the layer is the last coordinate and takes no offset.
                const char* sw = spatial == 1 ? "x" : "xy";
                scaled_coord = format("vec%d(%s.%s + %s(%s) * tex_scale(%uu) / %s(textureSize(%s, 0).%s), %s.%c)", coords,
                                      coord.c_str(), sw, kVec[spatial], o.c_str(), slot, kVec[spatial], sampler.c_str(),
                                      sw, coord.c_str(), kChan[spatial]);
            }
        } else {
            fail("texture offset in half texels");
        }
    }
    const bool shadow = t.inst >= kSampleC && t.inst <= kSampleCGLz;
    if (shadow != ((shadow_mask_ >> slot) & 1) && t.inst != kGetTextureInfo) {
        fail(format("texture %u sampled both with and without a depth compare", slot));
    }
    const std::string bias = t.lod_bias != 0 ? format(", %.4f", t.lod_bias / 8.0) : std::string();
    const bool vertex_lod = vs_; // no derivatives outside pixel shaders
    // The sampling call for coordinates and an offset (", ivec2(...)" or empty).
    const auto sample_call = [&](const std::string& coord, const std::string& offset) -> std::string {
        std::string call;
        switch (t.inst) {
        case kSample:
            if (vertex_lod) call = format("textureLod%s(%s, %s, 0.0%s)", offset.empty() ? "" : "Offset", sampler.c_str(), coord.c_str(), offset.c_str());
            else call = format("texture%s(%s, %s%s%s)", offset.empty() ? "" : "Offset", sampler.c_str(), coord.c_str(), offset.c_str(), bias.c_str());
            break;
        case kSampleL:
            call = format("textureLod%s(%s, %s, %s%s)", offset.empty() ? "" : "Offset", sampler.c_str(), coord.c_str(), c[3].c_str(), offset.c_str());
            break;
        case kSampleLz:
            call = format("textureLod%s(%s, %s, 0.0%s)", offset.empty() ? "" : "Offset", sampler.c_str(), coord.c_str(), offset.c_str());
            break;
        case kSampleLb:
            if (vertex_lod) call = format("textureLod%s(%s, %s, %s%s)", offset.empty() ? "" : "Offset", sampler.c_str(), coord.c_str(), c[3].c_str(), offset.c_str());
            else call = format("texture%s(%s, %s%s, %s)", offset.empty() ? "" : "Offset", sampler.c_str(), coord.c_str(), offset.c_str(), c[3].c_str());
            break;
        case kSampleG:
            call = format("textureGrad%s(%s, %s, gradH.%s, gradV.%s%s)", offset.empty() ? "" : "Offset", sampler.c_str(), coord.c_str(),
                          spatial == 1 ? "x" : spatial == 2 ? "xy" : "xyz", spatial == 1 ? "x" : spatial == 2 ? "xy" : "xyz", offset.c_str());
            break;
        case kSampleC:
        case kSampleCLz:
        case kSampleCL: {
            // The reference value follows the coordinates (w; z for C_L).
            const std::string ref = t.inst == kSampleCL ? c[2] : c[3];
            std::string sc;
            if (cube) sc = "vec4(" + coord + ", " + ref + ")";
            else if (coords == 1) sc = "vec3(" + coord + ", 0.0, " + ref + ")";
            else if (coords == 2) sc = "vec3(" + coord + ", " + ref + ")";
            else sc = "vec4(" + coord + ", " + ref + ")";
            if (t.inst == kSampleC && !vertex_lod) {
                call = format("vec4(texture%s(%s, %s%s))", offset.empty() ? "" : "Offset", sampler.c_str(), sc.c_str(), offset.c_str());
            } else if (cube || coords == 3) {
                // Array and cube shadow samplers have no explicit-LOD variant: use gradients of 0.
                const char* g = cube ? "vec3(0.0)" : "vec2(0.0)";
                call = format("vec4(textureGrad%s(%s, %s, %s, %s%s))", offset.empty() ? "" : "Offset", sampler.c_str(), sc.c_str(), g, g, offset.c_str());
            } else {
                const std::string lod = t.inst == kSampleCL ? c[3] : std::string("0.0");
                call = format("vec4(textureLod%s(%s, %s, %s%s))", offset.empty() ? "" : "Offset", sampler.c_str(), sc.c_str(), lod.c_str(), offset.c_str());
            }
            break;
        }
        case kFetch4:
            call = format("textureGather%s(%s, %s%s).zxyw", offset.empty() ? "" : "Offset", sampler.c_str(), coord.c_str(), offset.c_str());
            break;
        default: break;
        }
        return call;
    };
    std::string call;
    switch (t.inst) {
    case kSample:
    case kSampleL:
    case kSampleLz:
    case kSampleLb:
    case kSampleG:
    case kSampleC:
    case kSampleCLz:
    case kSampleCL:
    case kFetch4:
        call = sample_call(coord, offset);
        if (!scaled_coord.empty()) {
            call = format("(tex_scale(%uu) == 1.0 ? %s : %s)", slot, call.c_str(), sample_call(scaled_coord, "").c_str());
        }
        break;
    case kLd: {
        const std::string ic = format("ivec%d(", std::max(coords, 2));
        std::string args;
        // Texel coordinates scale with the texture; an array layer does not.
        // floor(c * scale + 0.5) is c * scale at the whole scales, and at
        // 1.5 an image texel inside the title's texel c.
        constexpr const char* kScaled = "int(floor(float(%s.%c) * tex_scale(%uu) + 0.5))";
        for (int i = 0; i < coords; ++i) {
            const bool texel = i < spatial;
            args += (i ? ", " : "") + format(texel ? kScaled : "%s.%c", gpr(t.src_gpr, t.src_rel, kLoopIndex).c_str(),
                                            kChan[t.src_sel[i] & 3], slot);
        }
        if (coords == 1) call = format("texelFetch(%s, %s, %s.%c)", sampler.c_str(),
                                       format(kScaled, gpr(t.src_gpr, t.src_rel, kLoopIndex).c_str(),
                                              kChan[t.src_sel[0] & 3], slot).c_str(),
                                       gpr(t.src_gpr, t.src_rel, kLoopIndex).c_str(), kChan[t.src_sel[3] & 3]);
        else call = format("texelFetch(%s, %s%s), %s.%c)", sampler.c_str(), ic.c_str(), args.c_str(), gpr(t.src_gpr, t.src_rel, kLoopIndex).c_str(), kChan[t.src_sel[3] & 3]);
        break;
    }
    case kGetTextureInfo: {
        const std::string lod = format("%s.%c", gpr(t.src_gpr, t.src_rel, kLoopIndex).c_str(), kChan[t.src_sel[0] & 3]);
        const std::string ts = format("int(tex_scale(%uu))", slot);
        if (coords == 1) call = format("ivec4(textureSize(%s, %s) / %s, 0, 0, textureQueryLevels(%s))", sampler.c_str(), lod.c_str(), ts.c_str(), sampler.c_str());
        else if (coords == 2) call = format("ivec4(textureSize(%s, %s) / %s, 0, textureQueryLevels(%s))", sampler.c_str(), lod.c_str(), ts.c_str(), sampler.c_str());
        else if (array) call = format("ivec4(textureSize(%s, %s) / ivec3(%s, %s, 1), textureQueryLevels(%s))", sampler.c_str(), lod.c_str(), ts.c_str(), ts.c_str(), sampler.c_str());
        else call = format("ivec4(textureSize(%s, %s) / %s, textureQueryLevels(%s))", sampler.c_str(), lod.c_str(), ts.c_str(), sampler.c_str());
        // Queried once: the destination may be the LOD's register.
        line("ivec4 q = " + call + ";");
        // An upscaled texture's size divides exactly at the whole scales.
        // At 1.5 the image's levels halve its rounded-up size, which can
        // give a texel more than the title's level: the title's size is
        // then level 0's divided, halved down to the level. (Only the texel
        // dimensions: layers and a 3D texture's depth are not upscaled.)
        if (!cube) {
            const std::string size0 = format("textureSize(%s, 0)", sampler.c_str());
            const std::string fractional = format("if (fract(tex_scale(%uu)) != 0.0) ", slot);
            if (spatial == 1) {
                line(fractional + format("q.x = max(1, int(round(float(%s) / tex_scale(%uu))) >> %s);",
                                         (array ? size0 + ".x" : size0).c_str(), slot, lod.c_str()));
            } else {
                line(fractional + format("q.xy = max(ivec2(1), ivec2(round(vec2(%s.xy) / tex_scale(%uu))) >> %s);",
                                         size0.c_str(), slot, lod.c_str()));
            }
        }
        for (int i = 0; i < 4; ++i) {
            const uint8_t sel = t.dst_sel[i];
            if (sel == kSelMask) continue;
            store(i, sel < 4 ? format("q.%c", kChan[sel]) : sel == kSel1 ? "1" : "0");
        }
        close();
        return;
    }
    default:
        fail(format("unsupported texture instruction %s", tex::name(t.inst) ? tex::name(t.inst) : "?"));
        close();
        return;
    }
    if (integer) line(format("ivec4 r = ivec4(%s);", call.c_str()));
    else line(format("ivec4 r = floatBitsToInt(%s);", call.c_str()));
    const char* one = integer ? "1" : "0x3F800000";
    for (int i = 0; i < 4; ++i) {
        const uint8_t sel = t.dst_sel[i];
        if (sel == kSelMask) continue;
        const std::string v = sel < 4 ? format("r.%c", kChan[sel]) : sel == kSel1 ? std::string(one) : "0";
        store(i, v);
    }
    close();
}

void Translator::emit_tex_clause(const Program& program, const CfInstruction& cf) {
    for (const TexInstruction& t : program.tex_clauses[cf.clause]) {
        if (t.vfetch) emit_buffer_fetch(t);
        else emit_texture(t);
    }
}

void Translator::emit_vtx_clause(const std::vector<VtxInstruction>& clause, bool) {
    for (const VtxInstruction& v : clause) emit_vertex_fetch(v);
}

// ----------------------------------------------------------------- exports

void Translator::emit_export(const CfInstruction& cf) {
    const ExportInstruction& e = cf.exp;
    const auto component = [&](int burst, int i, bool as_float) -> std::string {
        const uint8_t sel = e.sel[i];
        const std::string reg = gpr(e.rw_gpr + burst, e.rw_rel, kLoopIndex);
        if (sel < 4) return as_float ? format("intBitsToFloat(%s.%c)", reg.c_str(), kChan[sel]) : format("%s.%c", reg.c_str(), kChan[sel]);
        if (sel == kSel1) return as_float ? "1.0" : "1";
        return as_float ? "0.0" : "0";
    };
    const auto vec = [&](int burst) {
        return format("vec4(%s, %s, %s, %s)", component(burst, 0, true).c_str(), component(burst, 1, true).c_str(),
                      component(burst, 2, true).c_str(), component(burst, 3, true).c_str());
    };
    if (env_.stage == Stage::kVertex) {
        for (int b = 0; b <= e.burst_count; ++b) {
            const uint32_t base = e.array_base + b;
            if (e.type == cf::kPosition) {
                if (base == 60) {
                    line("gl_Position = " + vec(b) + ";");
                    if (!env_.clip_space_dx) line("gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;");
                } else if (base == 61) {
                    if (env_.point_size_export) line("gl_PointSize = " + component(b, 0, true) + " * dc.params.z;"); // the title's pixels, upscaled
                } else if (base > 63) {
                    fail(format("position export %u", base));
                }
            } else if (e.type == cf::kParameter) {
                if (base >= 32) {
                    fail(format("parameter export %u", base));
                    continue;
                }
                for (int location : param_locations_[base]) line(format("v%d = %s;", location, vec(b).c_str()));
            } else {
                fail("pixel export from a vertex shader");
            }
        }
        return;
    }
    if (e.type != cf::kPixel) {
        fail("non-pixel export from a pixel shader");
        return;
    }
    if (e.array_base == 61) {
        line("gl_FragDepth = " + component(0, 0, true) + ";");
        return;
    }
    for (int b = 0; b <= e.burst_count; ++b) {
        const uint32_t index = e.array_base + b;
        // The n-th export goes to the n-th render target the shader mask enables.
        std::vector<int> targets;
        int n = 0;
        for (int target = 0; target < 8; ++target) {
            if (((env_.cb_shader_mask >> (target * 4)) & 0xF) == 0) continue;
            if (static_cast<uint32_t>(n) == index || (env_.multiwrite && index == 0)) targets.push_back(target);
            ++n;
        }
        if (targets.empty()) continue;
        if (index == 0 && env_.alpha_func != 7) {
            static constexpr const char* kCompare[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
            if (env_.alpha_func == 0) {
                line("demote;");
            } else {
                line(format("if (!(%s %s dc.params.x)) demote;", component(b, 3, true).c_str(), kCompare[env_.alpha_func]));
            }
            uses_kill_ = true;
        }
        for (int target : targets) {
            switch (env_.color_kind[target]) {
            case TextureKind::kFloat: line(format("o%d = %s;", target, vec(b).c_str())); break;
            case TextureKind::kSint:
                line(format("o%d = ivec4(%s, %s, %s, %s);", target, component(b, 0, false).c_str(), component(b, 1, false).c_str(),
                            component(b, 2, false).c_str(), component(b, 3, false).c_str()));
                break;
            case TextureKind::kUint:
                line(format("o%d = uvec4(ivec4(%s, %s, %s, %s));", target, component(b, 0, false).c_str(),
                            component(b, 1, false).c_str(), component(b, 2, false).c_str(), component(b, 3, false).c_str()));
                break;
            }
        }
    }
}

void Translator::emit_stream_out(const CfInstruction& cf) {
    const ExportInstruction& e = cf.exp;
    const uint32_t buffer = cf.inst - cf::kMemStream0;
    if (!vs_) {
        fail("stream-out from a pixel shader");
        return;
    }
    if (!(env_.stream_out_mask & (1u << buffer))) return; // buffer not enabled: no write
    if (e.type != 0) fail("indexed stream-out write");
    const uint32_t stride = env_.stream_out_stride[buffer];
    const std::string reg = gpr(e.rw_gpr, e.rw_rel, kLoopIndex);
    for (int i = 0; i < 4; ++i) {
        if (!(e.comp_mask & (1u << i))) continue;
        line(format("gstore(dc.so[%u], uint(gl_VertexIndex) * %uu + %uu, uint(%s.%c));", buffer, stride,
                    (e.array_base + i) * 4, reg.c_str(), kChan[i]));
    }
}

// ------------------------------------------------------------ control flow

void Translator::emit_cf(const Program& program, const CfInstruction& cf) {
    if (auto it = expected_depth_.find(cf.index); it != expected_depth_.end() && it->second != depth_) {
        fail(format("CF %u: control flow stack depth %d where a jump expects %d", cf.index, depth_, it->second));
    }
    const auto push = [&] {
        line(format("st%d = act;", depth_));
        ++depth_;
        max_depth_ = std::max(max_depth_, depth_);
    };
    const auto pop = [&](int count) {
        depth_ -= count;
        if (depth_ < 0) {
            fail(format("CF %u: control flow stack underflow", cf.index));
            depth_ = 0;
            return;
        }
        line(format("act = st%d;", depth_));
    };
    const auto do_else = [&] {
        if (depth_ < 1) {
            fail(format("CF %u: ELSE without a pushed state", cf.index));
            return;
        }
        line(format("act = !act && st%d;", depth_ - 1));
    };
    if (cf.kind == CfInstruction::kAlu) {
        switch (cf.inst) {
        case cf::kAluClause:
            open("if (act)");
            emit_alu_clause(program, cf);
            close();
            break;
        case cf::kAluPushBefore:
            push();
            open("if (act)");
            emit_alu_clause(program, cf);
            close();
            break;
        case cf::kAluPopAfter:
        case cf::kAluPop2After:
            open("if (act)");
            emit_alu_clause(program, cf);
            close();
            pop(cf.inst == cf::kAluPopAfter ? 1 : 2);
            break;
        case cf::kAluElseAfter:
            open("if (act)");
            emit_alu_clause(program, cf);
            close();
            do_else();
            break;
        case cf::kAluBreak:
        case cf::kAluContinue:
            if (loops_.empty()) fail(format("CF %u: ALU_BREAK outside a loop", cf.index));
            open("if (act)");
            emit_alu_clause(program, cf);
            line(cf.inst == cf::kAluBreak ? "if (!act) break;" : "if (!act) { act = true; continue; }");
            close();
            break;
        default: fail(format("CF %u: unsupported ALU clause %s", cf.index, cf_alu_name(cf.inst))); break;
        }
        return;
    }
    if (cf.kind == CfInstruction::kExport) {
        if (cf.inst == cf::kExp || cf.inst == cf::kExpDone) emit_export(cf);
        else if (cf.inst >= cf::kMemStream0 && cf.inst <= cf::kMemStream3) emit_stream_out(cf);
        else fail(format("CF %u: unsupported export %s", cf.index, cf_export_name(cf.inst)));
        return;
    }
    if (cf.cond != cf::kActive && cf.inst != cf::kNop) {
        fail(format("CF %u: conditional %s", cf.index, cf_name(cf.inst)));
    }
    switch (cf.inst) {
    case cf::kNop:
    case cf::kWaitAck:
    case cf::kTexAck:
    case cf::kVtxAck:
    case cf::kVtxTcAck:
        break;
    case cf::kTex:
        open("if (act)");
        emit_tex_clause(program, cf);
        close();
        break;
    case cf::kVtx:
    case cf::kVtxTc:
        open("if (act)");
        emit_vtx_clause(program.vtx_clauses[cf.clause], false);
        close();
        break;
    case cf::kCallFs:
        if (!vs_ || fetch_ == nullptr) {
            fail("CALL_FS without a fetch shader");
            break;
        }
        for (const CfInstruction& f : fetch_->cf) {
            if (f.kind != CfInstruction::kNormal) {
                fail("fetch shader with ALU or export instructions");
                continue;
            }
            if (f.inst == cf::kVtx || f.inst == cf::kVtxTc) emit_vtx_clause(fetch_->vtx_clauses[f.clause], true);
            else if (f.inst != cf::kReturn && f.inst != cf::kNop) fail(format("fetch shader instruction %s", cf_name(f.inst)));
        }
        break;
    case cf::kPush:
        push();
        break;
    case cf::kPop:
        pop(static_cast<int>(cf.pop_count));
        break;
    case cf::kElse:
        do_else();
        expected_depth_[cf.addr] = depth_ - static_cast<int>(cf.pop_count);
        break;
    case cf::kJump:
        expected_depth_[cf.addr] = depth_ - static_cast<int>(cf.pop_count);
        break;
    case cf::kLoopStartDx10:
    case cf::kLoopStartNoAl: {
        const int id = loop_counter_++;
        open("");
        line(format("bool loop%d = act;", id));
        open("while (act)");
        loops_.push_back({depth_, id});
        break;
    }
    case cf::kLoopEnd: {
        if (loops_.empty()) {
            fail(format("CF %u: LOOP_END without a loop", cf.index));
            break;
        }
        const Loop loop = loops_.back();
        loops_.pop_back();
        if (depth_ != loop.depth) fail(format("CF %u: loop body changes the stack depth", cf.index));
        depth_ = loop.depth;
        close();
        line(format("act = loop%d;", loop.id));
        close();
        break;
    }
    case cf::kLoopBreak:
        if (loops_.empty()) fail(format("CF %u: LOOP_BREAK outside a loop", cf.index));
        line("if (act) break;");
        break;
    case cf::kLoopContinue:
        if (loops_.empty()) fail(format("CF %u: LOOP_CONTINUE outside a loop", cf.index));
        line("if (act) continue;");
        break;
    case cf::kCall:
        line(format("act = sub%u(act);", cf.addr));
        break;
    case cf::kReturn:
        break; // handled by emit_program
    default:
        fail(format("CF %u: unsupported control flow instruction %s", cf.index, cf_name(cf.inst)));
        break;
    }
}

void Translator::emit_program(const Program& program, uint32_t start, bool subroutine) {
    depth_ = 0;
    max_depth_ = 0;
    expected_depth_.clear();
    for (const CfInstruction& cf : program.cf) {
        if (cf.index < start) continue;
        emit_cf(program, cf);
        if (cf.end_of_program) break;
        if (cf.kind == CfInstruction::kNormal && cf.inst == cf::kReturn) {
            if (!subroutine) fail(format("CF %u: RETURN in the main program", cf.index));
            break;
        }
    }
    if (!loops_.empty()) fail("loop without LOOP_END");
}

// ------------------------------------------------------------------ header

std::string Translator::header() const {
    std::string h;
    h += "#version 460\n";
    h += "#extension GL_EXT_buffer_reference : require\n";
    h += "#extension GL_EXT_buffer_reference_uvec2 : require\n";
    if (uses_kill_) h += "#extension GL_EXT_demote_to_helper_invocation : require\n";
    h += "\n";
    h += "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GuestWords { uint w[]; };\n";
    h += "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GuestVec4s { uvec4 v[]; };\n";
    if (env_.stream_out_mask) {
        h += "layout(buffer_reference, std430, buffer_reference_align = 4) writeonly buffer GuestOut { uint w[]; };\n";
    }
    h += format("layout(set = 0, binding = %u, std140) uniform DrawConstants {\n", abi::kDrawConstantsBinding);
    h += "    uvec4 vs_cb[16];\n    uvec4 ps_cb[16];\n    uvec4 vs_buf[16];\n    uvec4 ps_buf[16];\n";
    h += "    uvec4 vb[16];\n    uvec4 so[4];\n    uvec4 vtx;\n    vec4 params;\n    uvec4 scaled;\n} dc;\n";
    if (out_.uses_registers) {
        h += format("layout(set = 0, binding = %u, std140) uniform Registers { uvec4 c[256]; } regs;\n",
                    vs_ ? abi::kVsRegistersBinding : abi::kPsRegistersBinding);
    }
    // Textures.
    for (uint32_t slot = 0; slot < abi::kTextureSlots; ++slot) {
        if (!(texture_mask_ & (1u << slot))) continue;
        const ShaderEnvironment::Texture& t = env_.textures[slot];
        const char* prefix = t.kind == TextureKind::kUint ? "u" : t.kind == TextureKind::kSint ? "i" : "";
        const char* type = "sampler2D";
        switch (t.dim) {
        case 0: type = "sampler1D"; break;
        case 1: type = "sampler2D"; break;
        case 2: type = "sampler3D"; break;
        case 3: type = "samplerCube"; break;
        case 4: type = "sampler1DArray"; break;
        case 5: type = "sampler2DArray"; break;
        case 6: type = "sampler2DMS"; break;
        case 7: type = "sampler2DMSArray"; break;
        default: break;
        }
        h += format("layout(set = 0, binding = %u) uniform %s%s%s tex%u;\n",
                    (vs_ ? abi::kVsTextureBinding : abi::kPsTextureBinding) + slot, prefix, type,
                    (shadow_mask_ >> slot) & 1 ? "Shadow" : "", slot);
    }
    // Interface between the stages.
    for (int k = 0; k < env_.ps_input_count; ++k) {
        if (k == env_.ps_position_input) continue;
        const ShaderEnvironment::PsInput& in = env_.ps_inputs[k];
        bool exported = false;
        for (uint8_t s : env_.vs_out_semantic) exported |= s == in.semantic && s != 0xFF;
        if (!exported) continue;
        const char* q = in.flat ? "flat " : in.linear ? "noperspective " : "";
        const char* centroid = in.centroid ? "centroid " : "";
        h += format("layout(location = %d) %s%s%s vec4 v%d;\n", k, q, centroid, vs_ ? "out" : "in", k);
    }
    if (!vs_) {
        for (int target = 0; target < 8; ++target) {
            if (((env_.cb_shader_mask >> (target * 4)) & 0xF) == 0) continue;
            const char* type = env_.color_kind[target] == TextureKind::kUint   ? "uvec4"
                               : env_.color_kind[target] == TextureKind::kSint ? "ivec4"
                                                                                : "vec4";
            h += format("layout(location = %d) out %s o%d;\n", target, type, target);
        }
    }
    h += "\n";
    // Helpers.
    // Textures copied from upscaled render targets have more texels than the
    // title knows of: texel coordinates, sizes and offsets are scaled.
    h += format("float tex_scale(uint slot) { return ((dc.scaled.%c >> slot) & 1u) != 0u ? dc.params.w : 1.0; }\n",
                vs_ ? 'x' : 'y');
    h += "uint bswap(uint x) { return (x << 24) | ((x << 8) & 0xFF0000u) | ((x >> 8) & 0xFF00u) | (x >> 24); }\n";
    h += "uvec4 swap16(uvec4 x) { return ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu); }\n";
    h += "uvec4 swap32(uvec4 x) { return uvec4(bswap(x.x), bswap(x.y), bswap(x.z), bswap(x.w)); }\n";
    h += "uvec4 swap64(uvec4 x) { return uvec4(bswap(x.y), bswap(x.x), bswap(x.w), bswap(x.z)); }\n";
    // A little-endian load of `n` (1-4) bytes at byte `off` of buffer `b`.
    h += "uint gload(uvec4 b, uint off, uint n) {\n"
         "    if (off >= b.z || n > b.z - off) return 0u;\n" // no wrap-around: a stray offset faults the GPU
         "    uint a = off + (b.x & 3u);\n"
         "    GuestWords p = GuestWords(uvec2(b.x & ~3u, b.y));\n"
         "    uint i = a >> 2u, s = (a & 3u) * 8u;\n"
         "    uint v = p.w[i];\n"
         "    if (s != 0u) v = (v >> s) | (p.w[i + 1u] << (32u - s));\n"
         "    return v;\n"
         "}\n";
    if (env_.stream_out_mask) {
        h += "void gstore(uvec4 b, uint off, uint v) {\n"
             "    if (off >= b.z || 4u > b.z - off) return;\n"
             "    GuestOut(uvec2(b.x & ~3u, b.y)).w[(off + (b.x & 3u)) >> 2u] = v;\n"
             "}\n";
    }
    h += format("ivec4 kc(uint bank, uint index) {\n"
                "    uvec4 b = dc.%s_cb[bank];\n"
                "    if (index >= (b.z >> 4u)) return ivec4(0);\n" // index * 16 wraps for negative relative indices
                "    return ivec4(GuestVec4s(uvec2(b.x & ~15u, b.y)).v[index]);\n"
                "}\n",
                vs_ ? "vs" : "ps");
    h += "float mul_ni(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n";
    h += "int mulhi_i(int a, int b) { int hi, lo; imulExtended(a, b, hi, lo); return hi; }\n";
    h += "uint mulhi_u(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }\n";
    if (uses_cube_) {
        // CUBE: (T, S, 2 * major axis, face) for a direction.
        h += "vec4 cube(vec3 d) {\n"
             "    vec3 a = abs(d);\n"
             "    if (a.z >= a.x && a.z >= a.y) return vec4(-d.y, d.z < 0.0 ? -d.x : d.x, 2.0 * d.z, d.z < 0.0 ? 5.0 : 4.0);\n"
             "    if (a.y >= a.x) return vec4(d.y < 0.0 ? -d.z : d.z, d.x, 2.0 * d.y, d.y < 0.0 ? 3.0 : 2.0);\n"
             "    return vec4(-d.y, d.x < 0.0 ? d.z : -d.z, 2.0 * d.x, d.x < 0.0 ? 1.0 : 0.0);\n"
             "}\n";
        // Back from the face coordinates (S, T biased by 1.5) a cube sample takes.
        h += "vec3 cube_dir(vec2 st, float face) {\n"
             "    vec2 c = (st - vec2(1.5)) * 2.0;\n"
             "    int f = int(face);\n"
             "    if (f == 0) return vec3(1.0, -c.y, -c.x);\n"
             "    if (f == 1) return vec3(-1.0, -c.y, c.x);\n"
             "    if (f == 2) return vec3(c.x, 1.0, c.y);\n"
             "    if (f == 3) return vec3(c.x, -1.0, -c.y);\n"
             "    if (f == 4) return vec3(c.x, -c.y, 1.0);\n"
             "    return vec3(-c.x, -c.y, -1.0);\n"
             "}\n";
    }
    h += "\n";
    return h;
}

bool Translator::run(std::string& error) {
    vs_ = env_.stage == Stage::kVertex;
    analyze(p_);
    if (fetch_) analyze(*fetch_);
    if (vs_) {
        for (int n = 0; n < 32; ++n) {
            if (env_.vtx_semantic[n] != 0xFF) note_gpr(n + 1, false);
        }
        // Parameter exports feed the pixel shader inputs with the same semantic.
        for (int p = 0; p < 32; ++p) {
            const uint8_t s = env_.vs_out_semantic[p];
            if (s == 0xFF) continue;
            for (int k = 0; k < env_.ps_input_count; ++k) {
                if (k != env_.ps_position_input && env_.ps_inputs[k].semantic == s) param_locations_[p].push_back(k);
            }
        }
    } else {
        note_gpr(std::max(env_.ps_input_count, uint8_t{1}) - 1, false);
        if (env_.ps_front_face) note_gpr(env_.ps_front_face_gpr, false);
        if (env_.ps_param_gen_gpr >= 0) note_gpr(env_.ps_param_gen_gpr, false);
    }
    gpr_count_ = relative_ ? 128 : max_gpr_ + 1;

    // Subroutines first: GLSL functions over the shared state.
    std::string functions;
    for (uint32_t start : p_.subroutines) {
        body_.clear();
        indent_ = 1;
        emit_program(p_, start, true);
        std::string vars;
        for (int d = 0; d < max_depth_; ++d) vars += format("    bool st%d = false;\n", d);
        functions += format("bool sub%u(bool act) {\n", start) + vars + body_ + "    return act;\n}\n\n";
    }

    body_.clear();
    indent_ = 1;
    // Inputs.
    if (vs_) {
        line("R[0] = ivec4(gl_VertexIndex, 0, 0, gl_InstanceIndex);");
        line("gl_PointSize = dc.params.y;"); // PA_SU_POINT_SIZE unless the shader exports a size
        for (int k = 0; k < env_.ps_input_count; ++k) {
            bool exported = false;
            for (int p = 0; p < 32; ++p) {
                for (int l : param_locations_[p]) exported |= l == k;
            }
            if (exported) line(format("v%d = vec4(0.0);", k));
        }
    } else {
        for (int k = 0; k < env_.ps_input_count; ++k) {
            const ShaderEnvironment::PsInput& in = env_.ps_inputs[k];
            if (k == env_.ps_position_input) {
                // In the title's pixels when the pass renders upscaled.
                line(format("R[%d] = floatBitsToInt(vec4(gl_FragCoord.xy / dc.params.z, gl_FragCoord.z, 1.0 / gl_FragCoord.w));", k));
                continue;
            }
            bool exported = false;
            for (uint8_t s : env_.vs_out_semantic) exported |= s == in.semantic && s != 0xFF;
            if (exported) {
                line(format("R[%d] = floatBitsToInt(v%d);", k, k));
            } else {
                static constexpr const char* kDefaults[] = {"vec4(0.0)", "vec4(0.0, 0.0, 0.0, 1.0)",
                                                            "vec4(1.0, 1.0, 1.0, 0.0)", "vec4(1.0)"};
                line(format("R[%d] = floatBitsToInt(%s);", k, kDefaults[in.default_value & 3]));
            }
        }
        // Point sprite coordinates, T running up when PNT_SPRITE_TOP_1.
        const char* sprite = env_.point_sprite_top_1 ? "vec2(gl_PointCoord.x, 1.0 - gl_PointCoord.y)" : "gl_PointCoord";
        if (env_.ps_param_gen_gpr >= 0) {
            line(format("R[%d] = floatBitsToInt(vec4(%s, %s));", env_.ps_param_gen_gpr, sprite, sprite));
        }
        for (int k = 0; k < env_.ps_input_count; ++k) {
            if (!env_.ps_inputs[k].point_sprite || k == env_.ps_position_input) continue;
            static constexpr const char* kComponent[] = {"0.0", "1.0", "s.x", "s.y", "0.0", "0.0", "0.0", "0.0"};
            const uint8_t* o = env_.point_sprite_override;
            line(format("{ vec2 s = %s; R[%d] = floatBitsToInt(vec4(%s, %s, %s, %s)); }", sprite, k, kComponent[o[0] & 7],
                        kComponent[o[1] & 7], kComponent[o[2] & 7], kComponent[o[3] & 7]));
        }
        if (env_.ps_front_face) {
            line(format("R[%u].%c = %s;", env_.ps_front_face_gpr, kChan[env_.ps_front_face_chan & 3],
                        env_.ps_front_face_all_bits ? "gl_FrontFacing ? -1 : 0"
                                                    : "floatBitsToInt(gl_FrontFacing ? 1.0 : 0.0)"));
        }
    }
    emit_program(p_, 0, false);
    std::string main_body = body_;

    std::string main_vars;
    main_vars += format("    bool act = true;\n");
    for (int d = 0; d < max_depth_; ++d) main_vars += format("    bool st%d = false;\n", d);

    out_.texture_mask = texture_mask_;
    out_.shadow_mask = shadow_mask_ & texture_mask_;
    out_.uses_registers = uses_registers_;
    out_.constant_bank_mask = constant_bank_mask_;
    std::copy(std::begin(constant_bank_extent_), std::end(constant_bank_extent_), std::begin(out_.constant_bank_extent));
    out_.buffer_mask = buffer_mask_;
    std::string globals = format("ivec4 R[%d];\nivec4 PV = ivec4(0);\nint PSv = 0;\nbool pred = false;\n"
                                 "ivec4 AR = ivec4(0);\nivec4 ARn = ivec4(0);\nint AL = 0;\n",
                                 gpr_count_);
    if (uses_gradients_) globals += "vec4 gradH = vec4(0.0);\nvec4 gradV = vec4(0.0);\n";
    std::string init = format("    for (int i = 0; i < %d; ++i) R[i] = ivec4(0);\n", gpr_count_);
    if (relative_) {
        // Relative addressing ranges over the program's registers; every
        // access to the register file has a constant index (see gpr()).
        const int count = std::clamp(std::max<int>(env_.num_gprs, max_register_ + 1), 1, static_cast<int>(kClauseTemps));
        std::string load = "ivec4 rel_load(int i) {\n    switch (clamp(i, 0, " + std::to_string(count - 1) + ")) {\n";
        std::string store = "void rel_store(int i, int c, int v) {\n    switch (clamp(i, 0, " + std::to_string(count - 1) + ")) {\n";
        init.clear();
        for (int k = 0; k < count; ++k) {
            load += format("    case %d: return R[%d];\n", k, k);
            store += format("    case %d: R[%d][c] = v; break;\n", k, k);
            init += format("    R[%d] = ivec4(0);\n", k);
        }
        for (int k = kClauseTemps; k < gpr_count_; ++k) init += format("    R[%d] = ivec4(0);\n", k);
        load += "    }\n    return ivec4(0);\n}\n";
        store += "    }\n}\n";
        globals += load + store;
    }
    out_.glsl = header() + globals + "\n" + functions + "void main() {\n" + init + main_vars + main_body + "}\n";
    if (!error_.empty()) {
        error = error_;
        return false;
    }
    return true;
}

} // namespace

bool translate(const Program& program, const Program* fetch, const ShaderEnvironment& env, TranslatedShader& out,
               std::string& error) {
    out = TranslatedShader{};
    Translator translator(program, fetch, env, out);
    return translator.run(error);
}

} // namespace cafe::latte
