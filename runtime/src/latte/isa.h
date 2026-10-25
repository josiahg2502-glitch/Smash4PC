#pragma once

// The Latte shader instruction set (AMD R7xx family): control flow, ALU
// (VLIW5: four vector slots x, y, z, w and a transcendental slot t),
// texture and vertex fetch instructions. Encodings and opcode numbers are
// those of AMD's R700-family ISA documentation.

#include <cstdint>

namespace cafe::latte::isa {

// ------------------------------------------------------------ control flow
namespace cf {
enum Inst : uint32_t {
    kNop = 0x00, kTex = 0x01, kVtx = 0x02, kVtxTc = 0x03, kLoopStart = 0x04, kLoopEnd = 0x05,
    kLoopStartDx10 = 0x06, kLoopStartNoAl = 0x07, kLoopContinue = 0x08, kLoopBreak = 0x09,
    kJump = 0x0A, kPush = 0x0B, kPushElse = 0x0C, kElse = 0x0D, kPop = 0x0E, kPopJump = 0x0F,
    kPopPush = 0x10, kPopPushElse = 0x11, kCall = 0x12, kCallFs = 0x13, kReturn = 0x14,
    kEmitVertex = 0x15, kEmitCutVertex = 0x16, kCutVertex = 0x17, kKill = 0x18,
    kWaitAck = 0x1A, kTexAck = 0x1B, kVtxAck = 0x1C, kVtxTcAck = 0x1D,
};
// CF_INST_TYPE in bits 28-29 of word 1.
enum Type : uint32_t { kNormal = 0, kExport = 1, kAlu = 2, kAluExtended = 3 };
// ALU clause instructions (bits 26-29 of an ALU CF word 1).
enum AluInst : uint32_t {
    kAluClause = 0x08, kAluPushBefore = 0x09, kAluPopAfter = 0x0A, kAluPop2After = 0x0B,
    kAluExt = 0x0C, kAluContinue = 0x0D, kAluBreak = 0x0E, kAluElseAfter = 0x0F,
};
// Export instructions (bits 23-29 of an export word 1).
enum ExportInst : uint32_t {
    kMemStream0 = 0x20, kMemStream1 = 0x21, kMemStream2 = 0x22, kMemStream3 = 0x23,
    kMemScratch = 0x24, kMemReduction = 0x25, kMemRing = 0x26, kExp = 0x27, kExpDone = 0x28,
    kMemExport = 0x3A,
};
enum ExportType : uint32_t { kPixel = 0, kPosition = 1, kParameter = 2 };
enum Cond : uint32_t { kActive = 0, kFalse = 1, kBool = 2, kNotBool = 3 };
} // namespace cf

// --------------------------------------------------------------------- ALU
namespace alu {
// Source operand selects.
constexpr uint32_t kGprLast = 127;
constexpr uint32_t kKcacheBank0 = 128; // 128-159
constexpr uint32_t kKcacheBank1 = 160; // 160-191
constexpr uint32_t kImm0 = 248, kImm1 = 249, kImm1Int = 250, kImmMinus1Int = 251, kImmHalf = 252;
constexpr uint32_t kLiteral = 253, kPv = 254, kPs = 255;
constexpr uint32_t kConstFile = 256; // 256-511

enum OMod : uint32_t { kOmodOff = 0, kOmodM2 = 1, kOmodM4 = 2, kOmodD2 = 3 };
enum IndexMode : uint32_t { kArX = 0, kArY = 1, kArZ = 2, kArW = 3, kLoopIndex = 4 };
enum PredSel : uint32_t { kPredOff = 0, kPredZero = 2, kPredOne = 3 };

// Operand/result interpretation and unit constraints.
enum Flags : uint32_t {
    kVector = 1u << 0,    // may run in x, y, z, w
    kTrans = 1u << 1,     // may run in t
    kReduction = 1u << 2, // occupies all four vector slots (dot4, cube, max4)
    kPredSet = 1u << 3,   // sets the predicate / execute mask
    kIntIn = 1u << 4,     // sources are signed integers
    kUintIn = 1u << 5,    // sources are unsigned integers
    kIntOut = 1u << 6,    // result is a signed integer
    kUintOut = 1u << 7,   // result is an unsigned integer
    kKill = 1u << 8,      // pixel kill
};

struct OpInfo {
    const char* name; // null for unused opcodes
    uint8_t sources;
    uint32_t flags;
};

// OP2 opcodes, 0x00-0x7F (11-bit field; the game uses the low range).
const OpInfo& op2(uint32_t opcode);
// OP3 opcodes, 0x00-0x1F (encoded in the 5-bit field at word 1 bit 13).
const OpInfo& op3(uint32_t opcode);

namespace op2_code {
enum : uint32_t {
    kAdd = 0x00, kMul = 0x01, kMulIeee = 0x02, kMax = 0x03, kMin = 0x04, kMaxDx10 = 0x05, kMinDx10 = 0x06,
    kSetE = 0x08, kSetGt = 0x09, kSetGe = 0x0A, kSetNe = 0x0B, kSetEDx10 = 0x0C, kSetGtDx10 = 0x0D,
    kSetGeDx10 = 0x0E, kSetNeDx10 = 0x0F, kFract = 0x10, kTrunc = 0x11, kCeil = 0x12, kRndne = 0x13,
    kFloor = 0x14, kMova = 0x15, kMovaFloor = 0x16, kMovaInt = 0x18, kMov = 0x19, kNop = 0x1A,
    kPredSetGtUint = 0x1E, kPredSetGeUint = 0x1F, kPredSetE = 0x20, kPredSetGt = 0x21, kPredSetGe = 0x22,
    kPredSetNe = 0x23, kPredSetInv = 0x24, kPredSetPop = 0x25, kPredSetClr = 0x26, kPredSetRestore = 0x27,
    kPredSetEPush = 0x28, kPredSetGtPush = 0x29, kPredSetGePush = 0x2A, kPredSetNePush = 0x2B,
    kKillE = 0x2C, kKillGt = 0x2D, kKillGe = 0x2E, kKillNe = 0x2F, kAndInt = 0x30, kOrInt = 0x31,
    kXorInt = 0x32, kNotInt = 0x33, kAddInt = 0x34, kSubInt = 0x35, kMaxInt = 0x36, kMinInt = 0x37,
    kMaxUint = 0x38, kMinUint = 0x39, kSetEInt = 0x3A, kSetGtInt = 0x3B, kSetGeInt = 0x3C,
    kSetNeInt = 0x3D, kSetGtUint = 0x3E, kSetGeUint = 0x3F, kKillGtUint = 0x40, kKillGeUint = 0x41,
    kPredSetEInt = 0x42, kPredSetGtInt = 0x43, kPredSetGeInt = 0x44, kPredSetNeInt = 0x45,
    kKillEInt = 0x46, kKillGtInt = 0x47, kKillGeInt = 0x48, kKillNeInt = 0x49,
    kPredSetEPushInt = 0x4A, kPredSetGtPushInt = 0x4B, kPredSetGePushInt = 0x4C,
    kPredSetNePushInt = 0x4D, kPredSetLtPushInt = 0x4E, kPredSetLePushInt = 0x4F, kDot4 = 0x50,
    kDot4Ieee = 0x51, kCube = 0x52, kMax4 = 0x53, kMovaGprInt = 0x60, kExpIeee = 0x61,
    kLogClamped = 0x62, kLogIeee = 0x63, kRecipClamped = 0x64, kRecipFf = 0x65, kRecipIeee = 0x66,
    kRecipSqrtClamped = 0x67, kRecipSqrtFf = 0x68, kRecipSqrtIeee = 0x69, kSqrtIeee = 0x6A,
    kFltToInt = 0x6B, kIntToFlt = 0x6C, kUintToFlt = 0x6D, kSin = 0x6E, kCos = 0x6F, kAshrInt = 0x70,
    kLshrInt = 0x71, kLshlInt = 0x72, kMulloInt = 0x73, kMulhiInt = 0x74, kMulloUint = 0x75,
    kMulhiUint = 0x76, kRecipInt = 0x77, kRecipUint = 0x78, kFltToUint = 0x79,
};
} // namespace op2_code

namespace op3_code {
enum : uint32_t {
    kBfeUint = 0x04, kBfeInt = 0x05, kBfiInt = 0x06, kFma = 0x07, kMulLit = 0x0C, kMulLitM2 = 0x0D,
    kMulLitM4 = 0x0E, kMulLitD2 = 0x0F, kMulAdd = 0x10, kMulAddM2 = 0x11, kMulAddM4 = 0x12,
    kMulAddD2 = 0x13, kMulAddIeee = 0x14, kMulAddIeeeM2 = 0x15, kMulAddIeeeM4 = 0x16,
    kMulAddIeeeD2 = 0x17, kCndE = 0x18, kCndGt = 0x19, kCndGe = 0x1A, kCndEInt = 0x1C,
    kCndGtInt = 0x1D, kCndGeInt = 0x1E,
};
} // namespace op3_code
} // namespace alu

// ----------------------------------------------------------- tex / vertex
namespace tex {
enum Inst : uint32_t {
    kVtxFetch = 0x00, kVtxSemantic = 0x01, kMem = 0x02, kLd = 0x03, kGetTextureInfo = 0x04,
    kGetSampleInfo = 0x05, kGetCompTexLod = 0x06, kGetGradientsH = 0x07, kGetGradientsV = 0x08,
    kGetLerp = 0x09, kKeepGradients = 0x0A, kSetGradientsH = 0x0B, kSetGradientsV = 0x0C, kPass = 0x0D,
    kSetCubemapIndex = 0x0E, kFetch4 = 0x0F, kSample = 0x10, kSampleL = 0x11, kSampleLb = 0x12,
    kSampleLz = 0x13, kSampleG = 0x14, kSampleGL = 0x15, kSampleGLb = 0x16, kSampleGLz = 0x17,
    kSampleC = 0x18, kSampleCL = 0x19, kSampleCLb = 0x1A, kSampleCLz = 0x1B, kSampleCG = 0x1C,
    kSampleCGL = 0x1D, kSampleCGLb = 0x1E, kSampleCGLz = 0x1F, kSetTextureOffsets = 0x20,
    kGather4 = 0x21, kGather4O = 0x22, kGather4C = 0x23, kGather4CO = 0x24, kGetBufferResinfo = 0x25,
};
const char* name(uint32_t inst);
} // namespace tex

namespace vtx {
enum Inst : uint32_t { kFetch = 0x00, kSemantic = 0x01, kBufInfo = 0x0E };
enum FetchType : uint32_t { kVertexData = 0, kInstanceData = 1, kNoIndexOffset = 2 };
} // namespace vtx

// Destination/source component selects (SQ_SEL).
enum Sel : uint32_t { kSelX = 0, kSelY = 1, kSelZ = 2, kSelW = 3, kSel0 = 4, kSel1 = 5, kSelMask = 7 };

const char* cf_name(uint32_t inst);
const char* cf_alu_name(uint32_t inst);
const char* cf_export_name(uint32_t inst);

} // namespace cafe::latte::isa
