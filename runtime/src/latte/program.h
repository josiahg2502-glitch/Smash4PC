#pragma once

// A decoded Latte shader program: the control flow program and the clauses
// it runs. Microcode is little-endian dwords; CF instructions and ALU slots
// are 64 bits, texture and vertex fetch instructions 128 bits, and clause
// addresses count 64-bit units from the start of the program.

#include "latte/isa.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cafe::latte {

struct AluSource {
    uint16_t sel = 0; // isa::alu source select (GPR, kcache, constant, inline, literal, PV/PS)
    uint8_t chan = 0;
    bool rel = false;
    bool neg = false;
    bool abs = false;
};

struct AluInstruction {
    uint8_t slot = 0; // 0-3 vector x..w, 4 transcendental
    bool op3 = false;
    uint32_t opcode = 0;
    const isa::alu::OpInfo* info = nullptr;
    AluSource src[3];
    uint8_t dst_gpr = 0;
    uint8_t dst_chan = 0;
    bool dst_rel = false;
    bool write = true; // OP3 always writes; OP2 has WRITE_MASK
    bool clamp = false;
    uint8_t omod = 0;
    uint8_t index_mode = 0;
    uint8_t pred_sel = 0;
    bool update_exec_mask = false;
    bool update_pred = false;
    uint8_t bank_swizzle = 0;
};

// Up to five instructions issued together; sources see the GPRs as they
// were before the group, PV/PS are the previous group's results.
struct AluGroup {
    std::vector<AluInstruction> instructions;
    uint32_t literals[4] = {};
    uint8_t literal_count = 0;
};

struct KcacheLock {
    uint8_t bank = 0;
    uint8_t mode = 0; // 0 none, 1 lock 1 line (16 constants), 2 lock 2 lines, 3 lock loop index
    uint16_t addr = 0; // in 16-constant lines
};

struct VtxInstruction {
    uint32_t inst = 0;
    uint32_t fetch_type = 0;
    bool fetch_whole_quad = false;
    uint32_t buffer_id = 0;
    uint8_t src_gpr = 0;
    bool src_rel = false;
    uint8_t src_sel_x = 0;
    uint8_t mega_fetch_count = 0;
    uint8_t semantic_or_gpr = 0; // semantic id (SEMANTIC) or destination GPR (FETCH)
    bool dst_rel = false;
    uint8_t dst_sel[4] = {};
    bool use_const_fields = false;
    uint32_t data_format = 0;
    uint32_t num_format = 0;   // 0 norm, 1 int, 2 scaled
    bool format_signed = false;
    bool srf_mode_no_zero = false;
    uint32_t offset = 0;
    uint32_t endian = 0;
    bool const_buf_no_stride = false;
    bool mega_fetch = false;
    bool alt_const = false;
};

struct TexInstruction {
    uint32_t inst = 0;
    bool bc_frac_mode = false;
    bool fetch_whole_quad = false;
    uint32_t resource_id = 0;
    uint32_t sampler_id = 0;
    uint8_t src_gpr = 0;
    bool src_rel = false;
    bool alt_const = false;
    uint8_t src_sel[4] = {};
    uint8_t dst_gpr = 0;
    bool dst_rel = false;
    uint8_t dst_sel[4] = {};
    int8_t lod_bias = 0;     // 1.3.3 fixed point, in 1/8
    uint8_t coord_normalized = 0; // bit per coordinate x..w
    int8_t offset[3] = {};   // 1.3.1 fixed point, in 1/2
    // VTX_FETCH / VTX_SEMANTIC in a TEX clause: a buffer fetch (uniform
    // blocks read with an index), encoded as a vertex fetch.
    bool vfetch = false;
    VtxInstruction vtx;
};

struct ExportInstruction {
    uint32_t type = 0;       // pixel / position / parameter, or memory buffer kind
    uint32_t array_base = 0; // pixel: render target, position: 60+n, parameter: index
    uint8_t rw_gpr = 0;
    bool rw_rel = false;
    uint8_t index_gpr = 0;
    uint8_t elem_size = 0;
    uint8_t burst_count = 0; // exports burst_count+1 consecutive GPRs/targets
    uint8_t sel[4] = {};     // swizzle (pixel/position/parameter exports)
    uint16_t array_size = 0; // memory exports
    uint8_t comp_mask = 0;
};

struct CfInstruction {
    enum Kind : uint8_t { kNormal, kAlu, kExport };
    uint32_t index = 0;
    uint32_t word0 = 0, word1 = 0;
    Kind kind = kNormal;
    uint32_t inst = 0;
    // Normal
    uint32_t addr = 0;
    uint32_t count = 0; // instructions in the clause (already +1)
    uint32_t pop_count = 0;
    uint32_t cf_const = 0;
    uint32_t cond = 0;
    uint32_t call_count = 0;
    bool end_of_program = false;
    bool valid_pixel_mode = false;
    bool whole_quad_mode = false;
    bool barrier = false;
    // ALU
    KcacheLock kcache[2];
    bool alt_const = false;
    // Export
    ExportInstruction exp;
    int clause = -1; // index into the program's clause list of this kind
};

struct Program {
    // The main program from index 0, then each subroutine a CALL reaches,
    // in order of the CF index.
    std::vector<CfInstruction> cf;
    std::vector<uint32_t> subroutines; // CF indices CALL instructions target
    std::vector<std::vector<AluGroup>> alu_clauses;
    std::vector<std::vector<TexInstruction>> tex_clauses;
    std::vector<std::vector<VtxInstruction>> vtx_clauses;

    const CfInstruction* at(uint32_t index) const; // null if not decoded
};

// Decodes the program in `words`. A fetch shader ends at its RETURN, other
// shaders at the instruction marked end of program; subroutines (CALL
// targets, which may follow the end of the program) end at their RETURN.
// Returns false with a message for malformed microcode.
bool decode(std::span<const uint32_t> words, Program& program, std::string& error);

std::string disassemble(const Program& program);

} // namespace cafe::latte
