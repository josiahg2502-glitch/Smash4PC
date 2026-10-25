#pragma once

// The Wii U GPU ("Latte", an AMD R7xx derivative) as GX2 programs it:
// register addresses, PM4 command packets, and the register fields GX2
// composes. Register addresses are byte offsets in the GPU register space.
// PM4 SET_* packets address registers in dwords relative to the base of
// their space.
//
// Sources: AMD's R6xx/R7xx register reference and PM4 packet documentation;
// the values match the register maps in Mesa's r600 driver, Cemu and
// decaf-emu.

#include <cstdint>

namespace cafe::latte {

// ------------------------------------------------------------ register spaces
constexpr uint32_t kConfigBase = 0x08000;
constexpr uint32_t kConfigEnd = 0x0AC00;
constexpr uint32_t kContextBase = 0x28000;
constexpr uint32_t kContextEnd = 0x29000;
constexpr uint32_t kAluConstBase = 0x30000;
constexpr uint32_t kAluConstEnd = 0x32000;
constexpr uint32_t kResourceBase = 0x38000;
constexpr uint32_t kResourceEnd = 0x3B678;
constexpr uint32_t kSamplerBase = 0x3C000;
constexpr uint32_t kSamplerEnd = 0x3C288;
constexpr uint32_t kCtlConstBase = 0x3CFF0;
constexpr uint32_t kCtlConstEnd = 0x3CFF8;
constexpr uint32_t kLoopConstBase = 0x3E200;
constexpr uint32_t kLoopConstEnd = 0x3E380;
constexpr uint32_t kBoolConstBase = 0x3E380;
constexpr uint32_t kBoolConstEnd = 0x3E38C;
constexpr uint32_t kRegisterSpaceSize = 0x40000;

// ------------------------------------------------------------------ registers
namespace reg {
// config
constexpr uint32_t VGT_GS_PER_ES = 0x088C8;
constexpr uint32_t VGT_ES_PER_GS = 0x088CC;
constexpr uint32_t VGT_GS_VERTEX_REUSE = 0x088D4;
constexpr uint32_t VGT_GS_PER_VS = 0x088E8;
constexpr uint32_t VGT_PRIMITIVE_TYPE = 0x08958;
constexpr uint32_t SQ_CONFIG = 0x08C00;
constexpr uint32_t SQ_ESGS_RING_BASE = 0x08C40;
constexpr uint32_t SQ_ESGS_RING_SIZE = 0x08C44;
constexpr uint32_t SQ_GSVS_RING_BASE = 0x08C48;
constexpr uint32_t SQ_GSVS_RING_SIZE = 0x08C4C;
constexpr uint32_t SQ_ESTMP_RING_BASE = 0x08C50;
constexpr uint32_t SPI_CONFIG_CNTL_1 = 0x0913C;
constexpr uint32_t TA_CNTL_AUX = 0x09508;
constexpr uint32_t TD_PS_SAMPLER_BORDER0_RED = 0x0A400;
constexpr uint32_t TD_VS_SAMPLER_BORDER0_RED = 0x0A600;
constexpr uint32_t TD_GS_SAMPLER_BORDER0_RED = 0x0A900;
constexpr uint32_t CP_INT_STATUS = 0x0C128; // written with type-0 packets
// context
constexpr uint32_t DB_DEPTH_SIZE = 0x28000;
constexpr uint32_t DB_DEPTH_VIEW = 0x28004;
constexpr uint32_t DB_DEPTH_BASE = 0x2800C;
constexpr uint32_t DB_DEPTH_INFO = 0x28010;
constexpr uint32_t DB_DEPTH_HTILE_DATA_BASE = 0x28014;
constexpr uint32_t DB_STENCIL_CLEAR = 0x28028;
constexpr uint32_t DB_DEPTH_CLEAR = 0x2802C;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x28030;
constexpr uint32_t CB_COLOR0_BASE = 0x28040; // +4 per target
constexpr uint32_t CB_COLOR0_SIZE = 0x28060;
constexpr uint32_t CB_COLOR0_VIEW = 0x28080;
constexpr uint32_t CB_COLOR0_INFO = 0x280A0;
constexpr uint32_t CB_COLOR0_TILE = 0x280C0;
constexpr uint32_t CB_COLOR0_FRAG = 0x280E0;
constexpr uint32_t CB_COLOR0_MASK = 0x28100;
constexpr uint32_t SQ_ALU_CONST_BUFFER_SIZE_PS_0 = 0x28140;
constexpr uint32_t SQ_ALU_CONST_BUFFER_SIZE_VS_0 = 0x28180;
constexpr uint32_t SQ_ALU_CONST_BUFFER_SIZE_GS_0 = 0x281C0;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x28200;
constexpr uint32_t PA_SC_CLIPRECT_RULE = 0x2820C;
constexpr uint32_t PA_SC_EDGERULE = 0x28230;
constexpr uint32_t CB_TARGET_MASK = 0x28238;
constexpr uint32_t CB_SHADER_MASK = 0x2823C;
constexpr uint32_t PA_SC_GENERIC_SCISSOR_TL = 0x28240;
constexpr uint32_t PA_SC_VPORT_SCISSOR_0_TL = 0x28250;
constexpr uint32_t PA_SC_VPORT_ZMIN_0 = 0x282D0;
constexpr uint32_t SQ_VTX_SEMANTIC_0 = 0x28380;
constexpr uint32_t VGT_MAX_VTX_INDX = 0x28400;
constexpr uint32_t VGT_INDX_OFFSET = 0x28408;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2840C;
constexpr uint32_t SX_ALPHA_TEST_CONTROL = 0x28410;
constexpr uint32_t CB_BLEND_RED = 0x28414;
constexpr uint32_t DB_STENCILREFMASK = 0x28430;
constexpr uint32_t DB_STENCILREFMASK_BF = 0x28434;
constexpr uint32_t SX_ALPHA_REF = 0x28438;
constexpr uint32_t PA_CL_VPORT_XSCALE_0 = 0x2843C;
constexpr uint32_t SPI_VS_OUT_ID_0 = 0x28614;
constexpr uint32_t SPI_PS_INPUT_CNTL_0 = 0x28644;
constexpr uint32_t SPI_VS_OUT_CONFIG = 0x286C4;
constexpr uint32_t SPI_PS_IN_CONTROL_0 = 0x286CC;
constexpr uint32_t SPI_INTERP_CONTROL_0 = 0x286D4;
constexpr uint32_t SPI_INPUT_Z = 0x286D8;
constexpr uint32_t SPI_FOG_CNTL = 0x286DC;
constexpr uint32_t SPI_UNKNOWN_286C8 = 0x286C8; // written by GX2 with SET_ALL_CONTEXTS
constexpr uint32_t CB_BLEND0_CONTROL = 0x28780;
constexpr uint32_t CB_SHADER_CONTROL = 0x287A0;
constexpr uint32_t DB_DEPTH_CONTROL = 0x28800;
constexpr uint32_t CB_COLOR_CONTROL = 0x28808;
constexpr uint32_t DB_SHADER_CONTROL = 0x2880C;
constexpr uint32_t PA_CL_CLIP_CNTL = 0x28810;
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x28814;
constexpr uint32_t PA_CL_VTE_CNTL = 0x28818;
constexpr uint32_t PA_CL_VS_OUT_CNTL = 0x2881C;
constexpr uint32_t PA_CL_NANINF_CNTL = 0x28820;
constexpr uint32_t SQ_PGM_START_PS = 0x28840;
constexpr uint32_t SQ_PGM_RESOURCES_PS = 0x28850;
constexpr uint32_t SQ_PGM_RESOURCES_VS = 0x28868;
constexpr uint32_t SQ_PGM_START_VS = 0x28858;
constexpr uint32_t SQ_PGM_START_GS = 0x2886C;
constexpr uint32_t SQ_PGM_START_ES = 0x28880;
constexpr uint32_t SQ_PGM_START_FS = 0x28894;
constexpr uint32_t SQ_ESGS_RING_ITEMSIZE = 0x288A8;
constexpr uint32_t SQ_GSVS_RING_ITEMSIZE = 0x288AC;
constexpr uint32_t SQ_ESTMP_RING_ITEMSIZE = 0x288B0;
constexpr uint32_t SQ_GS_VERT_ITEMSIZE = 0x288C8;
constexpr uint32_t SQ_PGM_CF_OFFSET_VS = 0x288D0;
constexpr uint32_t SQ_PGM_CF_OFFSET_GS = 0x288D4;
constexpr uint32_t SQ_VTX_SEMANTIC_CLEAR = 0x288E0;
constexpr uint32_t SQ_ALU_CONST_CACHE_PS_0 = 0x28940;
constexpr uint32_t SQ_ALU_CONST_CACHE_VS_0 = 0x28980;
constexpr uint32_t SQ_ALU_CONST_CACHE_GS_0 = 0x289C0;
constexpr uint32_t PA_SU_POINT_SIZE = 0x28A00;
constexpr uint32_t PA_SU_POINT_MINMAX = 0x28A04;
constexpr uint32_t PA_SU_LINE_CNTL = 0x28A08;
constexpr uint32_t PA_SC_LINE_STIPPLE = 0x28A0C;
constexpr uint32_t VGT_OUTPUT_PATH_CNTL = 0x28A10;
constexpr uint32_t VGT_HOS_CNTL = 0x28A14;
constexpr uint32_t VGT_HOS_MAX_TESS_LEVEL = 0x28A18;
constexpr uint32_t VGT_HOS_MIN_TESS_LEVEL = 0x28A1C;
constexpr uint32_t VGT_HOS_REUSE_DEPTH = 0x28A20;
constexpr uint32_t VGT_GROUP_PRIM_TYPE = 0x28A24;
constexpr uint32_t VGT_GS_MODE = 0x28A40;
constexpr uint32_t PA_SC_MPASS_PS_CNTL = 0x28A48;
constexpr uint32_t PA_SC_MODE_CNTL = 0x28A4C;
constexpr uint32_t VGT_GS_OUT_PRIM_TYPE = 0x28A6C;
constexpr uint32_t VGT_DMA_INDEX_TYPE = 0x28A7C;
constexpr uint32_t VGT_PRIMITIVEID_EN = 0x28A84;
constexpr uint32_t VGT_DMA_NUM_INSTANCES = 0x28A88;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_EN = 0x28A94;
constexpr uint32_t VGT_INSTANCE_STEP_RATE_0 = 0x28AA0;
constexpr uint32_t VGT_STRMOUT_EN = 0x28AB0;
constexpr uint32_t VGT_REUSE_OFF = 0x28AB4;
constexpr uint32_t VGT_VTX_CNT_EN = 0x28AB8;
constexpr uint32_t VGT_STRMOUT_BUFFER_SIZE_0 = 0x28AD0; // +16 per buffer
constexpr uint32_t VGT_STRMOUT_VTX_STRIDE_0 = 0x28AD4;
constexpr uint32_t VGT_STRMOUT_BUFFER_BASE_0 = 0x28AD8;
constexpr uint32_t VGT_STRMOUT_BUFFER_OFFSET_0 = 0x28ADC;
constexpr uint32_t VGT_STRMOUT_BUFFER_EN = 0x28B20;
constexpr uint32_t VGT_STRMOUT_DRAW_OPAQUE_OFFSET = 0x28B28;
constexpr uint32_t PA_SC_LINE_CNTL = 0x28C00;
constexpr uint32_t PA_SU_VTX_CNTL = 0x28C08;
constexpr uint32_t PA_CL_GB_VERT_CLIP_ADJ = 0x28C0C;
constexpr uint32_t CB_CLRCMP_CONTROL = 0x28C30;
constexpr uint32_t PA_SC_AA_MASK = 0x28C48;
constexpr uint32_t VGT_VERTEX_REUSE_BLOCK_CNTL = 0x28C58;
constexpr uint32_t DB_RENDER_CONTROL = 0x28D0C;
constexpr uint32_t DB_RENDER_OVERRIDE = 0x28D10;
constexpr uint32_t DB_HTILE_SURFACE = 0x28D24;
constexpr uint32_t DB_SRESULTS_COMPARE_STATE0 = 0x28D28;
constexpr uint32_t DB_PRELOAD_CONTROL = 0x28D30;
constexpr uint32_t DB_PREFETCH_LIMIT = 0x28D34;
constexpr uint32_t DB_ALPHA_TO_MASK = 0x28D44;
constexpr uint32_t PA_SU_POLY_OFFSET_DB_FMT_CNTL = 0x28DF8;
constexpr uint32_t PA_SU_POLY_OFFSET_CLAMP = 0x28DFC;
constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x28E00;
constexpr uint32_t PA_CL_POINT_X_RAD = 0x28E10;
constexpr uint32_t PA_CL_UCP_0_X = 0x28E20;
// constants
constexpr uint32_t SQ_ALU_CONSTANT0_0 = 0x30000;   // pixel shader uniform registers
constexpr uint32_t SQ_ALU_CONSTANT0_256 = 0x31000; // vertex shader uniform registers
constexpr uint32_t SQ_VTX_BASE_VTX_LOC = 0x3CFF0;
constexpr uint32_t SQ_VTX_START_INST_LOC = 0x3CFF4;
constexpr uint32_t SQ_LOOP_CONST_PS_0 = 0x3E200;
constexpr uint32_t SQ_LOOP_CONST_VS_0 = 0x3E280;
constexpr uint32_t SQ_LOOP_CONST_GS_0 = 0x3E300;
} // namespace reg

// Resource slots (7 dwords each in the resource space) and sampler slots
// (3 dwords each).
namespace resource {
constexpr uint32_t kPsTexture = 0x00;
constexpr uint32_t kPsBuffer = 0x80;
constexpr uint32_t kVsTexture = 0xA0;
constexpr uint32_t kVsBuffer = 0x120;
constexpr uint32_t kVsGsOut = 0x13F;
constexpr uint32_t kVsAttrib = 0x140;
constexpr uint32_t kGsTexture = 0x150;
constexpr uint32_t kGsBuffer = 0x1D0;
constexpr uint32_t kGsGsIn = 0x1EF;
constexpr uint32_t kWords = 7;
constexpr uint32_t kTypeValidTexture = 2u << 30; // SQ_TEX_RESOURCE_WORD6.TYPE
constexpr uint32_t kTypeValidBuffer = 3u << 30;
} // namespace resource
namespace sampler {
constexpr uint32_t kPs = 0;
constexpr uint32_t kVs = 18;
constexpr uint32_t kGs = 36;
constexpr uint32_t kWords = 3;
} // namespace sampler

// ------------------------------------------------------------ field helpers
// Places `value` in a `width`-bit field at bit `shift`.
constexpr uint32_t field(uint32_t value, int shift, int width) {
    const uint32_t mask = width >= 32 ? ~0u : ((1u << width) - 1);
    return (value & mask) << shift;
}
constexpr uint32_t get_field(uint32_t reg, int shift, int width) {
    const uint32_t mask = width >= 32 ? ~0u : ((1u << width) - 1);
    return (reg >> shift) & mask;
}

// Hardware data formats (SQ_DATA_FORMAT), the low six bits of a GX2 format.
namespace fmt {
constexpr uint32_t k8 = 0x01, k4_4 = 0x02, k3_3_2 = 0x03, k16 = 0x05, k16Float = 0x06, k8_8 = 0x07,
                   k5_6_5 = 0x08, k6_5_5 = 0x09, k1_5_5_5 = 0x0A, k4_4_4_4 = 0x0B, k5_5_5_1 = 0x0C,
                   k32 = 0x0D, k32Float = 0x0E, k16_16 = 0x0F, k16_16Float = 0x10, k8_24 = 0x11,
                   k8_24Float = 0x12, k24_8 = 0x13, k24_8Float = 0x14, k10_11_11 = 0x15,
                   k10_11_11Float = 0x16, k11_11_10 = 0x17, k11_11_10Float = 0x18,
                   k2_10_10_10 = 0x19, k8_8_8_8 = 0x1A, k10_10_10_2 = 0x1B, kX24_8_32Float = 0x1C,
                   k32_32 = 0x1D, k32_32Float = 0x1E, k16_16_16_16 = 0x1F, k16_16_16_16Float = 0x20,
                   k32_32_32_32 = 0x22, k32_32_32_32Float = 0x23, k32_32_32 = 0x2F,
                   k32_32_32Float = 0x30, kBc1 = 0x31, kBc2 = 0x32, kBc3 = 0x33, kBc4 = 0x34,
                   kBc5 = 0x35;
constexpr bool is_compressed(uint32_t hw) { return hw >= kBc1 && hw <= kBc5; }
} // namespace fmt

// Endian swap selectors (SQ_ENDIAN / CB_ENDIAN).
namespace endian {
constexpr uint32_t kNone = 0, k8In16 = 1, k8In32 = 2, k8In64 = 3;
}

// ----------------------------------------------------------------------- PM4
namespace pm4 {
enum Opcode : uint32_t {
    // Packets the runtime defines for operations GX2 performs with internal
    // shaders or display hardware on the console. 0x01-0x0F are not used by
    // the command processor.
    kHleCopyColorToScan = 0x01,
    kHleSwapBuffers = 0x02,
    kHleClearColor = 0x03,
    kHleClearDepthStencil = 0x04,
    kHleCopySurface = 0x05,
    kHleResolveColor = 0x06,
    kHleExpandDepth = 0x07,
    kHleConvertDepth = 0x08,

    kNop = 0x10,
    kSetPredication = 0x20,
    kDrawIndex2 = 0x27,
    kContextControl = 0x28,
    kIndexType = 0x2A,
    kDrawIndexAuto = 0x2D,
    kDrawIndexImmd = 0x2E,
    kNumInstances = 0x2F,
    kIndirectBufferPriv = 0x32,
    kStrmoutBufferUpdate = 0x34,
    kMemSemaphore = 0x39,
    kCopyDw = 0x3B,
    kWaitRegMem = 0x3C,
    kMemWrite = 0x3D,
    kIndirectBuffer = 0x3F,
    kSurfaceSync = 0x43,
    kEventWrite = 0x46,
    kEventWriteEop = 0x47,
    kLoadConfigReg = 0x60,
    kLoadContextReg = 0x61,
    kLoadAluConst = 0x62,
    kLoadBoolConst = 0x63,
    kLoadLoopConst = 0x64,
    kLoadResource = 0x65,
    kLoadSampler = 0x66,
    kLoadCtlConst = 0x67,
    kSetConfigReg = 0x68,
    kSetContextReg = 0x69,
    kSetAluConst = 0x6A,
    kSetBoolConst = 0x6B,
    kSetLoopConst = 0x6C,
    kSetResource = 0x6D,
    kSetSampler = 0x6E,
    kSetCtlConst = 0x6F,
    kStrmoutBaseUpdate = 0x72,
    kSetAllContexts = 0x74,
};

constexpr uint32_t type0(uint32_t reg_address, uint32_t count) {
    return (0u << 30) | ((count - 1) << 16) | (reg_address >> 2);
}
// `count` data dwords follow the header.
constexpr uint32_t type3(uint32_t opcode, uint32_t count) {
    return (3u << 30) | ((count - 1) << 16) | (opcode << 8);
}
constexpr uint32_t kFiller = 0x80000000; // type-2 packet: one dword, no effect

// CONTEXT_CONTROL enable bits: which register types LOAD_* packets load and
// which SET_* packets shadow to memory.
namespace context {
constexpr uint32_t kConfig = 1u << 0, kContext = 1u << 1, kAluConst = 1u << 2,
                   kBoolConst = 1u << 3, kLoopConst = 1u << 4, kResource = 1u << 5,
                   kSampler = 1u << 6, kCtlConst = 1u << 7, kOrdinal = 1u << 31;
constexpr uint32_t kAll = kConfig | kContext | kAluConst | kBoolConst | kLoopConst | kResource |
                          kSampler | kCtlConst | kOrdinal;
} // namespace context

// EVENT_WRITE_EOP / MEM_WRITE fields.
constexpr uint32_t kEventCacheFlushAndInvTs = 20;   // VGT_EVENT_TYPE
constexpr uint32_t kEventBottomOfPipeTs = 40;
constexpr uint32_t kEventIndexTs = 5u << 8;          // EVENT_INDEX(TS)
constexpr uint32_t kEopData32 = 1u << 29;            // EWP_ADDR_HI.DATA_SEL
constexpr uint32_t kEopData64 = 2u << 29;
constexpr uint32_t kEopDataClock = 3u << 29;
constexpr uint32_t kEopIntWriteConfirm = 2u << 24;   // EWP_ADDR_HI.INT_SEL
constexpr uint32_t kMemWriteData32 = 1u << 18;       // MW_ADDR_HI.DATA32
constexpr uint32_t kMemWriteClock = 1u << 16;        // MW_ADDR_HI.CNTR_SEL
} // namespace pm4

} // namespace cafe::latte
