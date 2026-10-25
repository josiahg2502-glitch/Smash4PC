// GX2 initialisation, the default GPU state, context states and cache
// invalidation.
//
// A GX2ContextState is a block of memory the GPU shadows register writes
// into while it is current. Switching to it loads the recorded registers
// back (LOAD_* packets over the ranges GX2 restores), through a small
// display list built once by GX2SetupContextStateEx.

#include "gx2/internal.h"

#include "gpu/gpu.h"
#include "os/kernel.h"

#include "cafe/export.h"
#include "cafe/runtime.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace cafe::gx2 {

using namespace latte;

namespace {

int32_t g_main_core = -1;

// Register ranges ([dword offset, count] within each space) that a context
// switch restores; the same ranges the console's GX2 uses (as decaf-emu's
// reverse engineering of GX2 documents them).
constexpr std::pair<uint32_t, uint32_t> kConfigRanges[] = {
    {0x300, 6}, {0x900, 0x48}, {0x980, 0x48}, {0xA00, 0x48}, {0x310, 0xC}, {0x542, 1}, {0x235, 1}, {0x232, 2},
    {0x23A, 1}, {0x256, 1},    {0x60C, 1},    {0x5C5, 1},    {0x2C8, 1},   {0x363, 1}, {0x404, 2},
};
constexpr std::pair<uint32_t, uint32_t> kContextRanges[] = {
    {0, 2},     {3, 3},      {0xA, 4},     {0x10, 0x38}, {0x50, 0x34}, {0x8E, 4},    {0x94, 0x40}, {0x100, 9},
    {0x10C, 3}, {0x10F, 0x60}, {0x185, 0xA}, {0x191, 0x27}, {0x1E0, 9},  {0x200, 1},   {0x202, 7},   {0xE0, 0x20},
    {0x210, 0x29}, {0x250, 0x34}, {0x290, 1}, {0x292, 2},  {0x2A1, 1},   {0x2A5, 1},   {0x2A8, 2},   {0x2AC, 3},
    {0x2CA, 1}, {0x2CC, 1},  {0x2CE, 1},   {0x300, 9},   {0x30C, 1},   {0x312, 1},   {0x316, 2},   {0x343, 2},
    {0x349, 3}, {0x34C, 2},  {0x351, 1},   {0x37E, 6},   {0x2B4, 3},   {0x2B8, 3},   {0x2BC, 3},   {0x2C0, 3},
    {0x2C8, 1}, {0x29B, 1},  {0x8C, 1},    {0xD5, 1},    {0x284, 0xC},
};
constexpr std::pair<uint32_t, uint32_t> kAluRanges[] = {{0, 0x800}};
constexpr std::pair<uint32_t, uint32_t> kLoopRanges[] = {{0, 0x60}};
constexpr std::pair<uint32_t, uint32_t> kResourceRanges[] = {
    {0, 0x70}, {0x380, 0x70}, {0x460, 0x70}, {0x7E0, 0x70}, {0x8B9, 7}, {0x8C0, 0x70}, {0x930, 0x70}, {0xCB0, 0x70},
    {0xD89, 7},
};
constexpr std::pair<uint32_t, uint32_t> kSamplerRanges[] = {{0, 0x36}, {0x36, 0x36}, {0x6C, 0x36}};

void load_packet(uint32_t opcode, uint32_t shadow, std::span<const std::pair<uint32_t, uint32_t>> ranges) {
    std::vector<uint32_t> packet{pm4::type3(opcode, static_cast<uint32_t>(2 + ranges.size() * 2)), shadow, 0};
    for (const auto& [offset, count] : ranges) {
        packet.push_back(offset);
        packet.push_back(count);
    }
    write(packet);
}

// Names `state` as the shadow of every register type and, unless
// `names_only`, loads the context-switched ranges from it.
void load_state(uint32_t state, bool names_only) {
    enable_state_shadowing();
    using R = std::span<const std::pair<uint32_t, uint32_t>>;
    const auto ranges = [&](R r) { return names_only ? R{} : r; };
    load_packet(pm4::kLoadConfigReg, state + context_state::kConfig, ranges(kConfigRanges));
    load_packet(pm4::kLoadContextReg, state + context_state::kContext, ranges(kContextRanges));
    load_packet(pm4::kLoadAluConst, state + context_state::kAlu, ranges(kAluRanges));
    load_packet(pm4::kLoadLoopConst, state + context_state::kLoop, ranges(kLoopRanges));
    load_packet(pm4::kLoadResource, state + context_state::kResource, ranges(kResourceRanges));
    load_packet(pm4::kLoadSampler, state + context_state::kSampler, ranges(kSamplerRanges));
    if (!names_only) invalidate_caches(invalidate::kShader, 0, 0xFFFFFFFF);
}

} // namespace

void enable_state_shadowing() {
    write_packet(pm4::kContextControl, {pm4::context::kAll, pm4::context::kAll});
}

void disable_state_shadowing() {
    write_packet(pm4::kContextControl, {pm4::context::kOrdinal, pm4::context::kOrdinal});
}

// The registers GX2 programs once at initialisation and for every new
// context state, before the default state (the console's values, as
// documented by decaf-emu).
void init_registers() {
    const uint32_t zeroes[24] = {};
    const uint32_t screen_scissor[] = {0, field(8192, 0, 14) | field(8192, 16, 14)};
    set_context_regs(reg::PA_SC_SCREEN_SCISSOR_TL, screen_scissor);
    set_context_reg(reg::PA_SC_LINE_CNTL, 0);
    // PIX_CENTER OGL, ROUND_MODE truncate, QUANT_MODE 1/256.
    set_context_reg(reg::PA_SU_VTX_CNTL, field(1, 0, 1) | field(0, 1, 2) | field(5, 3, 3));
    set_context_regs(reg::PA_CL_POINT_X_RAD, {zeroes, 4});
    set_context_regs(reg::PA_CL_UCP_0_X, {zeroes, 24});
    set_context_reg(reg::PA_CL_VTE_CNTL, 0x3F | field(1, 10, 1)); // viewport transform on, W0 format
    set_context_reg(reg::PA_CL_NANINF_CNTL, 0);
    const uint32_t window[] = {0, field(1, 31, 1), field(8192, 0, 14) | field(8192, 16, 14)};
    set_context_regs(reg::PA_SC_WINDOW_OFFSET, window);
    set_context_reg(reg::PA_SC_LINE_STIPPLE, 0);
    // PA_SC_MPASS_PS_CNTL, PA_SC_MODE_CNTL: MSAA, FORCE_EOV_CNTDWN, FORCE_EOV_REZ.
    const uint32_t mode_cntl[] = {0, field(1, 0, 1) | field(1, 14, 1) | field(1, 16, 1)};
    set_context_regs(reg::PA_SC_MPASS_PS_CNTL, mode_cntl);
    const uint32_t vport_scissor[] = {field(1, 31, 1), field(8192, 0, 14) | field(8192, 16, 14)};
    set_context_regs(reg::PA_SC_VPORT_SCISSOR_0_TL, vport_scissor);
    set_config_reg(0x8B24, 0xFF3FFF);
    set_context_reg(reg::PA_SC_CLIPRECT_RULE, 0xFFFF);
    set_config_reg(reg::VGT_GS_VERTEX_REUSE, 16);
    set_context_reg(reg::VGT_OUTPUT_PATH_CNTL, 1); // PATH_SELECT tessellation
    set_config_reg(reg::VGT_ES_PER_GS, 16);
    set_config_reg(reg::VGT_GS_PER_ES, 256);
    set_config_reg(reg::VGT_GS_PER_VS, 4);
    set_context_reg(reg::VGT_INDX_OFFSET, 0);
    set_context_reg(reg::VGT_REUSE_OFF, 0);
    set_context_reg(reg::VGT_MULTI_PRIM_IB_RESET_EN, 1);
    const uint32_t reuse[] = {14, 16}; // VGT_VERTEX_REUSE_BLOCK_CNTL, VGT_OUT_DEALLOC_CNTL
    set_context_regs(reg::VGT_VERTEX_REUSE_BLOCK_CNTL, reuse);
    set_context_reg(reg::VGT_HOS_REUSE_DEPTH, 16);
    set_context_reg(reg::VGT_STRMOUT_DRAW_OPAQUE_OFFSET, 0);
    set_context_reg(reg::VGT_VTX_CNT_EN, 0);
    const uint32_t index_range[] = {0xFFFFFFFF, 0}; // VGT_MAX_VTX_INDX, VGT_MIN_VTX_INDX
    set_context_regs(reg::VGT_MAX_VTX_INDX, index_range);
    set_config_reg(reg::TA_CNTL_AUX, field(1, 1, 1) | field(1, 24, 1) | field(1, 25, 1) | field(1, 26, 1));
    set_config_reg(0x9714, 1);
    set_config_reg(0x8D8C, 0x4000);
    set_config_regs(reg::SQ_ESTMP_RING_BASE, {zeroes, 12});
    set_context_regs(reg::SQ_ESTMP_RING_ITEMSIZE, {zeroes, 6});
    set_ctl_const(reg::SQ_VTX_START_INST_LOC, 0);
    set_context_regs(reg::SPI_FOG_CNTL, {zeroes, 3});
    // FLAT_SHADE_ENA, point sprite overrides S, T, 0, 1, PNT_SPRITE_TOP_1.
    set_context_reg(reg::SPI_INTERP_CONTROL_0, field(1, 0, 1) | field(2, 2, 3) | field(3, 5, 3) | field(0, 8, 3) |
                                                   field(1, 11, 3) | field(1, 14, 1));
    set_config_reg(reg::SPI_CONFIG_CNTL_1, 0);
    set_all_contexts_reg(reg::SPI_UNKNOWN_286C8, 1);
    set_context_reg(0x28354, 0x1FF);
    const uint32_t compare_state[] = {0, 0};
    set_context_regs(reg::DB_SRESULTS_COMPARE_STATE0, compare_state);
    set_context_reg(reg::DB_RENDER_OVERRIDE, 0);
    set_config_reg(0x9830, 0);
    set_config_reg(0x983C, 0x1000000);
    // CB_CLRCMP_CONTROL (select source), CB_CLRCMP_SRC, _DST, _MSK.
    const uint32_t color_compare[] = {field(1, 24, 2), 0, 0, 0xFFFFFFFF};
    set_context_regs(reg::CB_CLRCMP_CONTROL, color_compare);
    set_config_reg(0x9A1C, 0);
    set_context_reg(reg::PA_SC_AA_MASK, 0xFFFFFFFF);
    set_context_reg(reg::PA_SC_EDGERULE, 0xAAAAAAAA);
}

void set_default_state() {
    set_shader_mode(kModeUniformRegister, 48, 64, 0, 0, 200, 192);
    // Depth test less, depth writes on; stencil off, always / replace.
    set_context_reg(reg::DB_DEPTH_CONTROL, make_db_depth_control(true, true, 1, false, false, 7, 2, 2, 2, 7, 2, 2, 2));
    // Counter-clockwise front faces, no culling, filled triangles.
    set_context_reg(reg::PA_SU_SC_MODE_CNTL, make_pa_su_sc_mode_cntl(0, false, false, 0, 2, 2, false, false, false));
    set_context_reg(reg::DB_STENCILREFMASK, field(1, 0, 8) | field(0xFF, 8, 8) | field(0xFF, 16, 8));
    set_context_reg(reg::DB_STENCILREFMASK_BF, field(1, 0, 8) | field(0xFF, 8, 8) | field(0xFF, 16, 8));
    const uint32_t zero_offsets[] = {0, 0, 0, 0};
    set_context_regs(reg::PA_SU_POLY_OFFSET_FRONT_SCALE, zero_offsets);
    set_context_reg(reg::PA_SU_POLY_OFFSET_CLAMP, 0);
    set_context_reg(reg::PA_SU_POINT_SIZE, field(8, 0, 16) | field(8, 16, 16));   // 1.0 x 1.0
    set_context_reg(reg::PA_SU_POINT_MINMAX, field(8, 0, 16) | field(8, 16, 16)); // 1.0 .. 1.0
    set_context_reg(reg::PA_SU_LINE_CNTL, field(8, 0, 16));                       // 1.0
    set_context_reg(reg::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    set_context_reg(reg::SX_ALPHA_TEST_CONTROL, field(1, 0, 3)); // less, disabled
    set_context_reg(reg::SX_ALPHA_REF, 0);
    set_context_reg(reg::DB_ALPHA_TO_MASK, make_db_alpha_to_mask(false, 0));
    set_context_reg(reg::CB_TARGET_MASK, 0xFFFFFFFF);
    set_context_reg(reg::CB_COLOR_CONTROL, make_cb_color_control(0xCC, 0, false, true));
    for (uint32_t target = 0; target < 8; ++target) {
        // src alpha, 1 - src alpha, add, for color and alpha.
        set_context_reg(reg::CB_BLEND0_CONTROL + target * 4, make_cb_blend_control(4, 5, 0, true, 4, 5, 0));
    }
    const uint32_t blend_color[] = {0, 0, 0, 0};
    set_context_regs(reg::CB_BLEND_RED, blend_color);
    set_stream_out_enable(false);
    set_rasterizer_clip_control(true, true, false);
    set_context_reg(reg::VGT_HOS_MAX_TESS_LEVEL, std::bit_cast<uint32_t>(1.0f));
    set_context_reg(reg::VGT_HOS_MIN_TESS_LEVEL, std::bit_cast<uint32_t>(1.0f));
    set_context_reg(reg::DB_RENDER_CONTROL, 0);
}

void invalidate_caches(uint32_t mode, uint32_t address, uint32_t size) {
    // CPU caches are coherent here, but the renderer keeps GPU copies of what
    // the CPU wrote (textures, shaders).
    if (mode & invalidate::kCpu) gpu::cpu_wrote(address, size);
    if (mode == 0 || mode == invalidate::kCpu) return;
    if (size != 0xFFFFFFFF) size = (size + 0xFF) & ~0xFFu;
    uint32_t control = 1u << 31; // ENGINE_ME
    if (address == 0 && size == 0xFFFFFFFF && (mode & 0xF)) control |= 1u << 20;               // FULL_CACHE
    if (mode & (invalidate::kTexture | invalidate::kAttributeBuffer)) control |= 1u << 23;   // TC
    if (mode & invalidate::kUniformBlock) control |= (1u << 23) | (1u << 27);                // TC, SH
    if (mode & invalidate::kShader) control |= 1u << 27;                                     // SH
    if (mode & invalidate::kColorBuffer) control |= (0xFFu << 6) | (1u << 25);               // CB0-7, CB
    if (mode & invalidate::kDepthBuffer) control |= (1u << 14) | (1u << 26);                 // DB
    if (mode & invalidate::kStreamOutBuffer) control |= (0xFu << 2) | (1u << 28);            // SO0-3, SX
    if (mode & invalidate::kExportBuffer) control |= 1u | (1u << 23) | (3u << 25) | (1u << 28);
    write_packet(pm4::kSurfaceSync, {control, size >> 8, address >> 8, 4});
}

namespace {

// GX2InitAttrib identifiers.
enum : uint32_t {
    kAttribEnd = 0,
    kAttribPoolBase = 1,
    kAttribPoolSize = 2,
    kAttribArgc = 7,
    kAttribArgv = 8,
    kAttribProfileMode = 9,
    kAttribTossStage = 10,
    kAttribAppIoStackSize = 11,
};

void GX2Init(PPCContext& ctx, be<uint32_t>* attributes) {
    ApiLock lock;
    if (initialized()) return;
    uint32_t pool_base = 0, pool_size = 0;
    for (be<uint32_t>* a = attributes; a != nullptr && *a != kAttribEnd; a += 2) {
        switch (uint32_t{a[0]}) {
        case kAttribPoolBase: pool_base = a[1]; break;
        case kAttribPoolSize: pool_size = a[1]; break;
        case kAttribArgc: case kAttribArgv: case kAttribProfileMode: case kAttribTossStage:
        case kAttribAppIoStackSize: break;
        default: std::fprintf(stderr, "ttt2: GX2Init: unknown attribute %u\n", uint32_t{a[0]}); break;
        }
    }
    g_main_core = os::current_thread()->core;
    gpu::start();
    init_command_buffers(ctx, pool_base, pool_size);
    init_display();
    disable_state_shadowing();
    init_registers();
    set_default_state();
    flush();
}

void GX2Shutdown() {
    {
        ApiLock lock;
        if (!initialized()) return;
        flush();
    }
    gpu::wait_timestamp(gpu::last_submitted_timestamp(), uint64_t{gpu_timeout_ms()} * 1000000);
}

int32_t GX2GetMainCoreId() { return initialized() ? g_main_core : -1; }

void GX2SetDefaultState() {
    ApiLock lock;
    set_default_state();
}

void GX2SetupContextStateEx(uint32_t state, uint32_t flags) {
    ApiLock lock;
    std::memset(guest<uint8_t>(state), 0, context_state::kSize);
    *guest<be<uint32_t>>(state + context_state::kProfilingEnabled) = (flags & 1) ? 1u : 0u;
    load_state(state, true);
    init_registers();
    set_default_state();
    uint32_t list_size = 0;
    if (!(flags & 2)) { // GX2_CONTEXT_STATE_NO_SHADOW_DISPLAY_LIST
        begin_display_list(state + context_state::kDisplayList, context_state::kDisplayListCapacity);
        load_state(state, false);
        list_size = end_display_list();
    }
    *guest<be<uint32_t>>(state + context_state::kDisplayListSize) = list_size;
}

void GX2SetContextState(uint32_t state) {
    ApiLock lock;
    if (state == 0) {
        disable_state_shadowing();
        return;
    }
    const uint32_t list_size = *guest<be<uint32_t>>(state + context_state::kDisplayListSize);
    if (list_size == 0) {
        load_state(state, false);
    } else {
        call_display_list(state + context_state::kDisplayList, list_size);
    }
}

void GX2GetContextStateDisplayList(uint32_t state, be<uint32_t>* out_list, be<uint32_t>* out_size) {
    if (out_list) *out_list = state + context_state::kDisplayList;
    if (out_size) *out_size = *guest<be<uint32_t>>(state + context_state::kDisplayListSize);
}

void GX2Invalidate(uint32_t mode, uint32_t address, uint32_t size) {
    ApiLock lock;
    invalidate_caches(mode, address, size);
}

uint32_t GX2TempGetGPUVersion() { return 2; }

void GX2PrintGPUStatus() {
    const uint64_t submitted = gpu::last_submitted_timestamp();
    const uint64_t retired = gpu::retired_timestamp();
    std::fprintf(stderr, "ttt2: GX2PrintGPUStatus: submitted %llu, retired %llu\n",
                 static_cast<unsigned long long>(submitted), static_cast<unsigned long long>(retired));
}

} // namespace

CAFE_EXPORT(gx2, GX2Init, GX2Init);
CAFE_EXPORT(gx2, GX2Shutdown, GX2Shutdown);
CAFE_EXPORT(gx2, GX2GetMainCoreId, GX2GetMainCoreId);
CAFE_EXPORT(gx2, GX2SetDefaultState, GX2SetDefaultState);
CAFE_EXPORT(gx2, GX2SetupContextStateEx, GX2SetupContextStateEx);
CAFE_EXPORT(gx2, GX2SetContextState, GX2SetContextState);
CAFE_EXPORT(gx2, GX2GetContextStateDisplayList, GX2GetContextStateDisplayList);
CAFE_EXPORT(gx2, GX2Invalidate, GX2Invalidate);
CAFE_EXPORT(gx2, GX2TempGetGPUVersion, GX2TempGetGPUVersion);
CAFE_EXPORT(gx2, GX2PrintGPUStatus, GX2PrintGPUStatus);

} // namespace cafe::gx2
