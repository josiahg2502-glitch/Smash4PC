#pragma once

// Shared by the GX2 implementation files: command writing, command buffer
// management, surface layout and the register encodings GX2 composes.
//
// GX2 writes PM4 packets, big-endian, into command buffers in guest memory:
// the main ring (a pool GX2Init receives or allocates) or a display list
// the title is recording. The command processor (gpu/) executes them.

#include "gpu/latte.h"
#include "gx2/types.h"

#include "cafe/ppc_context.h"

#include <initializer_list>
#include <mutex>
#include <span>
#include <vector>

namespace cafe::gx2 {

// Titles call GX2 from several threads, which the console serialises per
// core. Guest threads run in parallel here, so one recursive lock covers
// all GX2 state and command writing.
std::recursive_mutex& api_mutex();
struct ApiLock {
    ApiLock() { api_mutex().lock(); }
    ~ApiLock() { api_mutex().unlock(); }
    ApiLock(const ApiLock&) = delete;
    ApiLock& operator=(const ApiLock&) = delete;
};

// --------------------------------------------------------- command buffers
// cbpool.cpp. Display lists are recorded per guest thread (the console keeps
// one per core); everything else goes to the main buffer.
void init_command_buffers(PPCContext& ctx, uint32_t pool_base, uint32_t pool_size);
bool initialized();
// Writes host-order dwords as big-endian into the active command buffer.
void write(std::span<const uint32_t> dwords);
void flush();
void begin_display_list(uint32_t buffer, uint32_t bytes);
uint32_t end_display_list(); // returns the recorded size in bytes
bool recording_display_list();
void call_display_list(uint32_t buffer, uint32_t bytes);
void direct_call_display_list(uint32_t buffer, uint32_t bytes);
uint64_t last_submitted_timestamp();
uint32_t gpu_timeout_ms();

// ------------------------------------------------------------- PM4 helpers
void write_packet(uint32_t opcode, std::initializer_list<uint32_t> data);
void set_config_reg(uint32_t address, uint32_t value);
void set_config_regs(uint32_t address, std::span<const uint32_t> values);
void set_context_reg(uint32_t address, uint32_t value);
void set_context_regs(uint32_t address, std::span<const uint32_t> values);
void set_all_contexts_reg(uint32_t address, uint32_t value);
void set_loop_const(uint32_t address, uint32_t value);
void set_ctl_const(uint32_t address, uint32_t value);
void set_resource(uint32_t slot, const uint32_t (&words)[latte::resource::kWords]);
void set_sampler(uint32_t slot, uint32_t word0, uint32_t word1, uint32_t word2);

// An HLE packet (latte::pm4::kHle*) whose payload mixes dwords and GX2
// structures copied verbatim from guest memory.
class HlePacket {
public:
    explicit HlePacket(uint32_t opcode) : opcode_(opcode) {}
    HlePacket& u32(uint32_t value) {
        payload_.push_back(value);
        return *this;
    }
    HlePacket& f32(float value);
    template <typename T>
    HlePacket& guest_struct(const T& object) {
        static_assert(sizeof(T) % 4 == 0);
        const auto* words = reinterpret_cast<const be<uint32_t>*>(&object);
        for (size_t i = 0; i < sizeof(T) / 4; ++i) payload_.push_back(words[i]);
        return *this;
    }
    void write();

private:
    uint32_t opcode_;
    std::vector<uint32_t> payload_;
};

// ---------------------------------------------------------------- surfaces
// surface.cpp, over AMD's addrlib.
struct SurfaceInfo {
    uint32_t pitch;       // in elements
    uint32_t height;      // in elements
    uint32_t depth;
    uint64_t size;        // bytes
    uint32_t base_align;
    uint32_t pitch_align;
    uint32_t height_align;
    uint32_t tile_mode;   // the tile mode addrlib chose for this level
    uint32_t bpp;         // bits per element
};
SurfaceInfo surface_info(const Surface& surface, uint32_t level);
// AddrLib's handle (ADDR_HANDLE), configured as the console's GPU.
void* address_library();
void calc_surface_size_and_alignment(Surface& surface);
void init_color_buffer_regs(ColorBuffer& buffer);

// ----------------------------------------------------------------- formats
// format.cpp
uint32_t surface_format_bits_per_element(uint32_t format);
uint32_t surface_format_bytes_per_element(uint32_t format);
uint32_t color_buffer_format(uint32_t format);        // CB_FORMAT
uint32_t color_buffer_number_type(uint32_t format);   // CB_NUMBER_TYPE
uint32_t color_buffer_source_format(uint32_t format); // CB_SOURCE_FORMAT
uint32_t attrib_format_bits(uint32_t format);
uint32_t attrib_format_data_format(uint32_t format);  // SQ_DATA_FORMAT
uint32_t attrib_format_endian(uint32_t format);       // SQ_ENDIAN
uint32_t swap_mode_endian(uint32_t mode);             // SQ_ENDIAN

// -------------------------------------------------------------- registers
// registers.cpp: the register values GX2's Init*Reg functions compute, for
// GX2SetDefaultState and the convenience Set* functions.
uint32_t make_db_depth_control(bool depth_test, bool depth_write, uint32_t depth_compare, bool stencil_test,
                               bool backface_stencil, uint32_t front_func, uint32_t front_zpass,
                               uint32_t front_zfail, uint32_t front_fail, uint32_t back_func,
                               uint32_t back_zpass, uint32_t back_zfail, uint32_t back_fail);
uint32_t make_pa_su_sc_mode_cntl(uint32_t front_face, bool cull_front, bool cull_back, uint32_t poly_mode,
                                 uint32_t poly_mode_front, uint32_t poly_mode_back, bool offset_front,
                                 bool offset_back, bool offset_para);
uint32_t make_cb_color_control(uint32_t rop3, uint32_t target_blend_enable, bool multi_write, bool color_write);
uint32_t make_cb_blend_control(uint32_t color_src, uint32_t color_dst, uint32_t color_combine, bool separate_alpha,
                               uint32_t alpha_src, uint32_t alpha_dst, uint32_t alpha_combine);
uint32_t make_db_alpha_to_mask(bool enable, uint32_t mode);
void set_rasterizer_clip_control(bool rasterizer, bool z_clip, bool half_z);

// ------------------------------------------------------------------ state
// state.cpp
void init_registers();
void set_default_state();
void enable_state_shadowing();
void disable_state_shadowing();
void set_shader_mode(uint32_t mode, uint32_t vs_gprs, uint32_t vs_stack, uint32_t gs_gprs, uint32_t gs_stack,
                     uint32_t ps_gprs, uint32_t ps_stack);
void set_stream_out_enable(bool enable);
void set_stream_out_buffer(uint32_t index, const OutputStream& stream);
void draw_indexed(uint32_t mode, uint32_t count, uint32_t index_type, uint32_t indices, uint32_t base_vertex,
                  uint32_t instances);
void invalidate_caches(uint32_t mode, uint32_t address, uint32_t size);

// ---------------------------------------------------------------- display
// display.cpp
void init_display();

} // namespace cafe::gx2
