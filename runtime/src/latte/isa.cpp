#include "latte/isa.h"

#include <array>
#include <cstddef>

namespace cafe::latte::isa {
namespace alu {
namespace {

constexpr uint32_t V = kVector, T = kTrans, VT = kVector | kTrans, R = kReduction, P = kPredSet;
constexpr uint32_t I = kIntIn | kIntOut, U = kUintIn | kUintOut, K = kKill;

struct Entry {
    uint32_t opcode;
    OpInfo info;
};

constexpr Entry kOp2[] = {
    {0x00, {"ADD", 2, VT}}, {0x01, {"MUL", 2, VT}}, {0x02, {"MUL_IEEE", 2, VT}}, {0x03, {"MAX", 2, VT}},
    {0x04, {"MIN", 2, VT}}, {0x05, {"MAX_DX10", 2, VT}}, {0x06, {"MIN_DX10", 2, VT}},
    {0x08, {"SETE", 2, VT}}, {0x09, {"SETGT", 2, VT}}, {0x0A, {"SETGE", 2, VT}}, {0x0B, {"SETNE", 2, VT}},
    {0x0C, {"SETE_DX10", 2, VT | kIntOut}}, {0x0D, {"SETGT_DX10", 2, VT | kIntOut}},
    {0x0E, {"SETGE_DX10", 2, VT | kIntOut}}, {0x0F, {"SETNE_DX10", 2, VT | kIntOut}},
    {0x10, {"FRACT", 1, VT}}, {0x11, {"TRUNC", 1, VT}}, {0x12, {"CEIL", 1, VT}}, {0x13, {"RNDNE", 1, VT}},
    {0x14, {"FLOOR", 1, VT}}, {0x15, {"MOVA", 1, V | kIntOut}}, {0x16, {"MOVA_FLOOR", 1, V | kIntOut}},
    {0x18, {"MOVA_INT", 1, V | I}}, {0x19, {"MOV", 1, VT}}, {0x1A, {"NOP", 0, VT}},
    {0x1E, {"PRED_SETGT_UINT", 2, VT | P | kUintIn}}, {0x1F, {"PRED_SETGE_UINT", 2, VT | P | kUintIn}},
    {0x20, {"PRED_SETE", 2, VT | P}}, {0x21, {"PRED_SETGT", 2, VT | P}}, {0x22, {"PRED_SETGE", 2, VT | P}},
    {0x23, {"PRED_SETNE", 2, VT | P}}, {0x24, {"PRED_SET_INV", 2, VT | P}}, {0x25, {"PRED_SET_POP", 2, VT | P}},
    {0x26, {"PRED_SET_CLR", 2, VT | P}}, {0x27, {"PRED_SET_RESTORE", 2, VT | P}},
    {0x28, {"PRED_SETE_PUSH", 2, VT | P}}, {0x29, {"PRED_SETGT_PUSH", 2, VT | P}},
    {0x2A, {"PRED_SETGE_PUSH", 2, VT | P}}, {0x2B, {"PRED_SETNE_PUSH", 2, VT | P}},
    {0x2C, {"KILLE", 2, VT | K}}, {0x2D, {"KILLGT", 2, VT | K}}, {0x2E, {"KILLGE", 2, VT | K}},
    {0x2F, {"KILLNE", 2, VT | K}}, {0x30, {"AND_INT", 2, VT | I}}, {0x31, {"OR_INT", 2, VT | I}},
    {0x32, {"XOR_INT", 2, VT | I}}, {0x33, {"NOT_INT", 1, VT | I}}, {0x34, {"ADD_INT", 2, VT | I}},
    {0x35, {"SUB_INT", 2, VT | I}}, {0x36, {"MAX_INT", 2, VT | I}}, {0x37, {"MIN_INT", 2, VT | I}},
    {0x38, {"MAX_UINT", 2, VT | U}}, {0x39, {"MIN_UINT", 2, VT | U}}, {0x3A, {"SETE_INT", 2, VT | I}},
    {0x3B, {"SETGT_INT", 2, VT | I}}, {0x3C, {"SETGE_INT", 2, VT | I}}, {0x3D, {"SETNE_INT", 2, VT | I}},
    {0x3E, {"SETGT_UINT", 2, VT | kUintIn | kIntOut}}, {0x3F, {"SETGE_UINT", 2, VT | kUintIn | kIntOut}},
    {0x40, {"KILLGT_UINT", 2, VT | K | kUintIn}}, {0x41, {"KILLGE_UINT", 2, VT | K | kUintIn}},
    {0x42, {"PRED_SETE_INT", 2, VT | P | kIntIn}}, {0x43, {"PRED_SETGT_INT", 2, VT | P | kIntIn}},
    {0x44, {"PRED_SETGE_INT", 2, VT | P | kIntIn}}, {0x45, {"PRED_SETNE_INT", 2, VT | P | kIntIn}},
    {0x46, {"KILLE_INT", 2, VT | K | kIntIn}}, {0x47, {"KILLGT_INT", 2, VT | K | kIntIn}},
    {0x48, {"KILLGE_INT", 2, VT | K | kIntIn}}, {0x49, {"KILLNE_INT", 2, VT | K | kIntIn}},
    {0x4A, {"PRED_SETE_PUSH_INT", 2, VT | P | kIntIn}}, {0x4B, {"PRED_SETGT_PUSH_INT", 2, VT | P | kIntIn}},
    {0x4C, {"PRED_SETGE_PUSH_INT", 2, VT | P | kIntIn}}, {0x4D, {"PRED_SETNE_PUSH_INT", 2, VT | P | kIntIn}},
    {0x4E, {"PRED_SETLT_PUSH_INT", 2, VT | P | kIntIn}}, {0x4F, {"PRED_SETLE_PUSH_INT", 2, VT | P | kIntIn}},
    {0x50, {"DOT4", 2, V | R}}, {0x51, {"DOT4_IEEE", 2, V | R}}, {0x52, {"CUBE", 2, V | R}},
    {0x53, {"MAX4", 2, V | R}}, {0x60, {"MOVA_GPR_INT", 1, V | I}}, {0x61, {"EXP_IEEE", 1, T}},
    {0x62, {"LOG_CLAMPED", 1, T}}, {0x63, {"LOG_IEEE", 1, T}}, {0x64, {"RECIP_CLAMPED", 1, T}},
    {0x65, {"RECIP_FF", 1, T}}, {0x66, {"RECIP_IEEE", 1, T}}, {0x67, {"RECIPSQRT_CLAMPED", 1, T}},
    {0x68, {"RECIPSQRT_FF", 1, T}}, {0x69, {"RECIPSQRT_IEEE", 1, T}}, {0x6A, {"SQRT_IEEE", 1, T}},
    {0x6B, {"FLT_TO_INT", 1, T | kIntOut}}, {0x6C, {"INT_TO_FLT", 1, T | kIntIn}},
    {0x6D, {"UINT_TO_FLT", 1, T | kUintIn}}, {0x6E, {"SIN", 1, T}}, {0x6F, {"COS", 1, T}},
    {0x70, {"ASHR_INT", 2, VT | I}}, {0x71, {"LSHR_INT", 2, VT | U}}, {0x72, {"LSHL_INT", 2, VT | I}},
    {0x73, {"MULLO_INT", 2, T | I}}, {0x74, {"MULHI_INT", 2, T | I}}, {0x75, {"MULLO_UINT", 2, T | U}},
    {0x76, {"MULHI_UINT", 2, T | U}}, {0x77, {"RECIP_INT", 1, T | I}}, {0x78, {"RECIP_UINT", 1, T | U}},
    {0x79, {"FLT_TO_UINT", 1, T | kUintOut}},
};

constexpr Entry kOp3[] = {
    {0x04, {"BFE_UINT", 3, VT | U}}, {0x05, {"BFE_INT", 3, VT | I}}, {0x06, {"BFI_INT", 3, VT | I}},
    {0x07, {"FMA", 3, VT}}, {0x0C, {"MUL_LIT", 3, T}}, {0x0D, {"MUL_LIT_M2", 3, T}},
    {0x0E, {"MUL_LIT_M4", 3, T}}, {0x0F, {"MUL_LIT_D2", 3, T}}, {0x10, {"MULADD", 3, VT}},
    {0x11, {"MULADD_M2", 3, VT}}, {0x12, {"MULADD_M4", 3, VT}}, {0x13, {"MULADD_D2", 3, VT}},
    {0x14, {"MULADD_IEEE", 3, VT}}, {0x15, {"MULADD_IEEE_M2", 3, VT}}, {0x16, {"MULADD_IEEE_M4", 3, VT}},
    {0x17, {"MULADD_IEEE_D2", 3, VT}}, {0x18, {"CNDE", 3, VT}}, {0x19, {"CNDGT", 3, VT}},
    {0x1A, {"CNDGE", 3, VT}}, {0x1C, {"CNDE_INT", 3, VT | I}}, {0x1D, {"CNDGT_INT", 3, VT | I}},
    {0x1E, {"CNDGE_INT", 3, VT | I}},
};

template <size_t N, size_t M>
constexpr std::array<OpInfo, N> build(const Entry (&entries)[M]) {
    std::array<OpInfo, N> table{};
    for (const Entry& e : entries) table[e.opcode] = e.info;
    return table;
}

constexpr auto kOp2Table = build<0x80>(kOp2);
constexpr auto kOp3Table = build<0x20>(kOp3);
constexpr OpInfo kUnknown{nullptr, 0, 0};

} // namespace

const OpInfo& op2(uint32_t opcode) { return opcode < kOp2Table.size() ? kOp2Table[opcode] : kUnknown; }
const OpInfo& op3(uint32_t opcode) { return opcode < kOp3Table.size() ? kOp3Table[opcode] : kUnknown; }

} // namespace alu

namespace tex {
const char* name(uint32_t inst) {
    static constexpr const char* kNames[] = {
        "VTX_FETCH", "VTX_SEMANTIC", "MEM", "LD", "GET_TEXTURE_INFO", "GET_SAMPLE_INFO", "GET_COMP_TEX_LOD",
        "GET_GRADIENTS_H", "GET_GRADIENTS_V", "GET_LERP", "KEEP_GRADIENTS", "SET_GRADIENTS_H",
        "SET_GRADIENTS_V", "PASS", "SET_CUBEMAP_INDEX", "FETCH4", "SAMPLE", "SAMPLE_L", "SAMPLE_LB",
        "SAMPLE_LZ", "SAMPLE_G", "SAMPLE_G_L", "SAMPLE_G_LB", "SAMPLE_G_LZ", "SAMPLE_C", "SAMPLE_C_L",
        "SAMPLE_C_LB", "SAMPLE_C_LZ", "SAMPLE_C_G", "SAMPLE_C_G_L", "SAMPLE_C_G_LB", "SAMPLE_C_G_LZ",
        "SET_TEXTURE_OFFSETS", "GATHER4", "GATHER4_O", "GATHER4_C", "GATHER4_C_O", "GET_BUFFER_RESINFO",
    };
    return inst < std::size(kNames) ? kNames[inst] : nullptr;
}
} // namespace tex

const char* cf_name(uint32_t inst) {
    static constexpr const char* kNames[] = {
        "NOP", "TEX", "VTX", "VTX_TC", "LOOP_START", "LOOP_END", "LOOP_START_DX10", "LOOP_START_NO_AL",
        "LOOP_CONTINUE", "LOOP_BREAK", "JUMP", "PUSH", "PUSH_ELSE", "ELSE", "POP", "POP_JUMP", "POP_PUSH",
        "POP_PUSH_ELSE", "CALL", "CALL_FS", "RETURN", "EMIT_VERTEX", "EMIT_CUT_VERTEX", "CUT_VERTEX", "KILL",
        "END_PROGRAM", "WAIT_ACK", "TEX_ACK", "VTX_ACK", "VTX_TC_ACK",
    };
    return inst < std::size(kNames) ? kNames[inst] : nullptr;
}

const char* cf_alu_name(uint32_t inst) {
    static constexpr const char* kNames[] = {"ALU", "ALU_PUSH_BEFORE", "ALU_POP_AFTER", "ALU_POP2_AFTER",
                                             "ALU_EXT", "ALU_CONTINUE", "ALU_BREAK", "ALU_ELSE_AFTER"};
    return inst >= 8 && inst < 16 ? kNames[inst - 8] : nullptr;
}

const char* cf_export_name(uint32_t inst) {
    switch (inst) {
    case cf::kMemStream0: return "MEM_STREAM0";
    case cf::kMemStream1: return "MEM_STREAM1";
    case cf::kMemStream2: return "MEM_STREAM2";
    case cf::kMemStream3: return "MEM_STREAM3";
    case cf::kMemScratch: return "MEM_SCRATCH";
    case cf::kMemReduction: return "MEM_REDUCTION";
    case cf::kMemRing: return "MEM_RING";
    case cf::kExp: return "EXP";
    case cf::kExpDone: return "EXP_DONE";
    case cf::kMemExport: return "MEM_EXPORT";
    default: return nullptr;
    }
}

} // namespace cafe::latte::isa
