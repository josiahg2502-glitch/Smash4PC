#include "latte/program.h"

#include <algorithm>
#include <cstdio>

namespace cafe::latte {
namespace {

using namespace isa;

constexpr uint32_t bits(uint32_t word, int shift, int width) {
    return (word >> shift) & ((width >= 32) ? ~0u : ((1u << width) - 1));
}

int32_t sign_extend(uint32_t value, int width) {
    const uint32_t sign = 1u << (width - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

std::string format(const char* fmt, auto... args) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), fmt, args...);
    return buffer;
}

bool decode_alu_clause(std::span<const uint32_t> words, uint32_t addr, uint32_t slots,
                       std::vector<AluGroup>& clause, std::string& error) {
    size_t pos = size_t{addr} * 2;
    int64_t remaining = slots;
    while (remaining > 0) {
        AluGroup group;
        bool last = false;
        while (!last) {
            if (pos + 2 > words.size()) {
                error = format("ALU clause at %u runs past the end of the program", addr);
                return false;
            }
            const uint32_t w0 = words[pos], w1 = words[pos + 1];
            pos += 2;
            --remaining;
            AluInstruction in;
            in.src[0] = {static_cast<uint16_t>(bits(w0, 0, 9)), static_cast<uint8_t>(bits(w0, 10, 2)),
                         bits(w0, 9, 1) != 0, bits(w0, 12, 1) != 0, false};
            in.src[1] = {static_cast<uint16_t>(bits(w0, 13, 9)), static_cast<uint8_t>(bits(w0, 23, 2)),
                         bits(w0, 22, 1) != 0, bits(w0, 25, 1) != 0, false};
            in.index_mode = static_cast<uint8_t>(bits(w0, 26, 3));
            in.pred_sel = static_cast<uint8_t>(bits(w0, 29, 2));
            last = bits(w0, 31, 1) != 0;
            in.bank_swizzle = static_cast<uint8_t>(bits(w1, 18, 3));
            in.dst_gpr = static_cast<uint8_t>(bits(w1, 21, 7));
            in.dst_rel = bits(w1, 28, 1) != 0;
            in.dst_chan = static_cast<uint8_t>(bits(w1, 29, 2));
            in.clamp = bits(w1, 31, 1) != 0;
            if (bits(w1, 15, 3) == 0) {
                in.op3 = false;
                in.opcode = bits(w1, 7, 11);
                in.src[0].abs = bits(w1, 0, 1) != 0;
                in.src[1].abs = bits(w1, 1, 1) != 0;
                in.update_exec_mask = bits(w1, 2, 1) != 0;
                in.update_pred = bits(w1, 3, 1) != 0;
                in.write = bits(w1, 4, 1) != 0;
                in.omod = static_cast<uint8_t>(bits(w1, 5, 2));
                in.info = &alu::op2(in.opcode);
            } else {
                in.op3 = true;
                in.opcode = bits(w1, 13, 5);
                in.src[2] = {static_cast<uint16_t>(bits(w1, 0, 9)), static_cast<uint8_t>(bits(w1, 10, 2)),
                             bits(w1, 9, 1) != 0, bits(w1, 12, 1) != 0, false};
                in.write = true;
                in.info = &alu::op3(in.opcode);
            }
            if (in.info->name == nullptr) {
                error = format("unknown ALU %s opcode 0x%X in the clause at %u", in.op3 ? "OP3" : "OP2", in.opcode, addr);
                return false;
            }
            group.instructions.push_back(in);
            if (group.instructions.size() > 5) {
                error = format("ALU group with more than five instructions in the clause at %u", addr);
                return false;
            }
        }
        // Slots: an instruction goes to the vector unit of its destination
        // channel unless only the transcendental unit can run it or that
        // vector unit is already taken.
        bool used[5] = {};
        for (AluInstruction& in : group.instructions) {
            const uint32_t f = in.info->flags;
            const bool trans_only = (f & alu::kTrans) && !(f & alu::kVector);
            uint8_t slot = trans_only ? 4 : in.dst_chan;
            if (!trans_only && used[slot]) slot = 4;
            if (used[slot]) {
                error = format("ALU group with two instructions for slot %u in the clause at %u", slot, addr);
                return false;
            }
            used[slot] = true;
            in.slot = slot;
        }
        // Literal constants follow the group, two or four dwords.
        int literal_chans = 0;
        for (const AluInstruction& in : group.instructions) {
            const int sources = in.op3 ? 3 : 2;
            for (int s = 0; s < sources; ++s) {
                if (in.src[s].sel == alu::kLiteral) literal_chans = std::max(literal_chans, in.src[s].chan + 1);
            }
        }
        if (literal_chans > 0) {
            group.literal_count = literal_chans <= 2 ? 2 : 4;
            if (pos + group.literal_count > words.size()) {
                error = format("ALU literals in the clause at %u run past the end of the program", addr);
                return false;
            }
            for (int i = 0; i < group.literal_count; ++i) group.literals[i] = words[pos + i];
            pos += group.literal_count;
            remaining -= group.literal_count / 2;
        }
        clause.push_back(std::move(group));
    }
    if (remaining < 0) {
        error = format("ALU clause at %u is longer than its count", addr);
        return false;
    }
    return true;
}

VtxInstruction decode_vtx(const uint32_t* w) {
    VtxInstruction v;
    v.inst = bits(w[0], 0, 5);
    v.fetch_type = bits(w[0], 5, 2);
    v.fetch_whole_quad = bits(w[0], 7, 1) != 0;
    v.buffer_id = bits(w[0], 8, 8);
    v.src_gpr = static_cast<uint8_t>(bits(w[0], 16, 7));
    v.src_rel = bits(w[0], 23, 1) != 0;
    v.src_sel_x = static_cast<uint8_t>(bits(w[0], 24, 2));
    v.mega_fetch_count = static_cast<uint8_t>(bits(w[0], 26, 6));
    v.semantic_or_gpr = static_cast<uint8_t>(bits(w[1], 0, 8));
    v.dst_rel = v.inst != vtx::kSemantic && bits(w[1], 7, 1) != 0;
    if (v.inst != vtx::kSemantic) v.semantic_or_gpr &= 0x7F;
    for (int i = 0; i < 4; ++i) v.dst_sel[i] = static_cast<uint8_t>(bits(w[1], 9 + 3 * i, 3));
    v.use_const_fields = bits(w[1], 21, 1) != 0;
    v.data_format = bits(w[1], 22, 6);
    v.num_format = bits(w[1], 28, 2);
    v.format_signed = bits(w[1], 30, 1) != 0;
    v.srf_mode_no_zero = bits(w[1], 31, 1) != 0;
    v.offset = bits(w[2], 0, 16);
    v.endian = bits(w[2], 16, 2);
    v.const_buf_no_stride = bits(w[2], 18, 1) != 0;
    v.mega_fetch = bits(w[2], 19, 1) != 0;
    v.alt_const = bits(w[2], 20, 1) != 0;
    return v;
}

TexInstruction decode_tex(const uint32_t* w) {
    TexInstruction t;
    t.inst = bits(w[0], 0, 5);
    t.bc_frac_mode = bits(w[0], 5, 1) != 0;
    t.fetch_whole_quad = bits(w[0], 7, 1) != 0;
    t.resource_id = bits(w[0], 8, 8);
    t.src_gpr = static_cast<uint8_t>(bits(w[0], 16, 7));
    t.src_rel = bits(w[0], 23, 1) != 0;
    t.alt_const = bits(w[0], 24, 1) != 0;
    t.dst_gpr = static_cast<uint8_t>(bits(w[1], 0, 7));
    t.dst_rel = bits(w[1], 7, 1) != 0;
    for (int i = 0; i < 4; ++i) t.dst_sel[i] = static_cast<uint8_t>(bits(w[1], 9 + 3 * i, 3));
    t.lod_bias = static_cast<int8_t>(sign_extend(bits(w[1], 21, 7), 7));
    t.coord_normalized = static_cast<uint8_t>(bits(w[1], 28, 4));
    for (int i = 0; i < 3; ++i) t.offset[i] = static_cast<int8_t>(sign_extend(bits(w[2], 5 * i, 5), 5));
    t.sampler_id = bits(w[2], 15, 5);
    for (int i = 0; i < 4; ++i) t.src_sel[i] = static_cast<uint8_t>(bits(w[2], 20 + 3 * i, 3));
    if (t.inst == tex::kVtxFetch || t.inst == tex::kVtxSemantic) {
        t.vfetch = true;
        t.vtx = decode_vtx(w);
    }
    return t;
}


template <typename T, typename Decoder>
bool decode_fetch_clause(std::span<const uint32_t> words, uint32_t addr, uint32_t count, std::vector<T>& clause,
                         Decoder decoder, const char* what, std::string& error) {
    const size_t pos = size_t{addr} * 2;
    if (pos + size_t{count} * 4 > words.size()) {
        error = format("%s clause at %u runs past the end of the program", what, addr);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) clause.push_back(decoder(&words[pos + i * 4]));
    return true;
}

} // namespace

namespace {

// Decodes CF instructions from `start` until the end of the program or, for
// a subroutine or fetch shader, its RETURN.
bool decode_cf(std::span<const uint32_t> words, uint32_t start, Program& program, std::string& error) {
    for (uint32_t index = start;; ++index) {
        if (size_t{index} * 2 + 2 > words.size()) {
            error = "control flow runs past the end of the program";
            return false;
        }
        const uint32_t w0 = words[index * 2], w1 = words[index * 2 + 1];
        CfInstruction cf;
        cf.index = index;
        cf.word0 = w0;
        cf.word1 = w1;
        const uint32_t type = bits(w1, 28, 2);
        if (type == cf::kAlu || type == cf::kAluExtended) {
            cf.kind = CfInstruction::kAlu;
            // ALU clause opcodes 12-15 set bit 28 too, so they read as type 3.
            cf.inst = bits(w1, 26, 4);
            if (cf.inst == cf::kAluExt) {
                error = format("CF %u: ALU_EXT (extra constant cache banks) is not supported", index);
                return false;
            }
            cf.addr = bits(w0, 0, 22);
            cf.kcache[0] = {static_cast<uint8_t>(bits(w0, 22, 4)), static_cast<uint8_t>(bits(w0, 30, 2)),
                            static_cast<uint16_t>(bits(w1, 2, 8))};
            cf.kcache[1] = {static_cast<uint8_t>(bits(w0, 26, 4)), static_cast<uint8_t>(bits(w1, 0, 2)),
                            static_cast<uint16_t>(bits(w1, 10, 8))};
            cf.count = bits(w1, 18, 7) + 1;
            cf.alt_const = bits(w1, 25, 1) != 0;
            cf.whole_quad_mode = bits(w1, 30, 1) != 0;
            cf.barrier = bits(w1, 31, 1) != 0;
            std::vector<AluGroup> clause;
            if (!decode_alu_clause(words, cf.addr, cf.count, clause, error)) return false;
            cf.clause = static_cast<int>(program.alu_clauses.size());
            program.alu_clauses.push_back(std::move(clause));
        } else if (type == cf::kExport) {
            cf.kind = CfInstruction::kExport;
            cf.inst = bits(w1, 23, 7);
            ExportInstruction& e = cf.exp;
            e.array_base = bits(w0, 0, 13);
            e.type = bits(w0, 13, 2);
            e.rw_gpr = static_cast<uint8_t>(bits(w0, 15, 7));
            e.rw_rel = bits(w0, 22, 1) != 0;
            e.index_gpr = static_cast<uint8_t>(bits(w0, 23, 7));
            e.elem_size = static_cast<uint8_t>(bits(w0, 30, 2));
            e.burst_count = static_cast<uint8_t>(bits(w1, 17, 4));
            if (cf.inst == cf::kExp || cf.inst == cf::kExpDone) {
                for (int i = 0; i < 4; ++i) e.sel[i] = static_cast<uint8_t>(bits(w1, 3 * i, 3));
            } else {
                e.array_size = static_cast<uint16_t>(bits(w1, 0, 12));
                e.comp_mask = static_cast<uint8_t>(bits(w1, 12, 4));
            }
            cf.end_of_program = bits(w1, 21, 1) != 0;
            cf.valid_pixel_mode = bits(w1, 22, 1) != 0;
            cf.whole_quad_mode = bits(w1, 30, 1) != 0;
            cf.barrier = bits(w1, 31, 1) != 0;
            if (cf_export_name(cf.inst) == nullptr) {
                error = format("CF %u: unknown export instruction 0x%X", index, cf.inst);
                return false;
            }
        } else {
            cf.kind = CfInstruction::kNormal;
            cf.inst = bits(w1, 23, 7);
            cf.addr = w0;
            cf.pop_count = bits(w1, 0, 3);
            cf.cf_const = bits(w1, 3, 5);
            cf.cond = bits(w1, 8, 2);
            cf.count = (bits(w1, 10, 3) | (bits(w1, 19, 1) << 3)) + 1;
            cf.call_count = bits(w1, 13, 6);
            cf.end_of_program = bits(w1, 21, 1) != 0;
            cf.valid_pixel_mode = bits(w1, 22, 1) != 0;
            cf.whole_quad_mode = bits(w1, 30, 1) != 0;
            cf.barrier = bits(w1, 31, 1) != 0;
            if (cf_name(cf.inst) == nullptr) {
                error = format("CF %u: unknown control flow instruction 0x%X", index, cf.inst);
                return false;
            }
            if (cf.inst == cf::kTex) {
                std::vector<TexInstruction> clause;
                if (!decode_fetch_clause(words, cf.addr, cf.count, clause, decode_tex, "TEX", error)) return false;
                cf.clause = static_cast<int>(program.tex_clauses.size());
                program.tex_clauses.push_back(std::move(clause));
            } else if (cf.inst == cf::kVtx || cf.inst == cf::kVtxTc) {
                std::vector<VtxInstruction> clause;
                if (!decode_fetch_clause(words, cf.addr, cf.count, clause, decode_vtx, "VTX", error)) return false;
                cf.clause = static_cast<int>(program.vtx_clauses.size());
                program.vtx_clauses.push_back(std::move(clause));
            } else if (cf.inst == cf::kCall) {
                if (cf.addr == 0 || size_t{cf.addr} * 2 >= words.size()) {
                    error = format("CF %u: CALL to %u is outside the program", index, cf.addr);
                    return false;
                }
                if (std::find(program.subroutines.begin(), program.subroutines.end(), cf.addr) ==
                    program.subroutines.end()) {
                    program.subroutines.push_back(cf.addr);
                }
            }
        }
        program.cf.push_back(cf);
        if (cf.end_of_program) break;
        if (cf.kind == CfInstruction::kNormal && cf.inst == cf::kReturn) break;
    }
    return true;
}

} // namespace

bool decode(std::span<const uint32_t> words, Program& program, std::string& error) {
    program = Program{};
    if (!decode_cf(words, 0, program, error)) return false;
    // Subroutines that lie outside the code decoded so far, including ones
    // called from other subroutines.
    for (size_t i = 0; i < program.subroutines.size(); ++i) {
        const uint32_t start = program.subroutines[i];
        const bool decoded = std::any_of(program.cf.begin(), program.cf.end(),
                                         [&](const CfInstruction& cf) { return cf.index == start; });
        if (!decoded && !decode_cf(words, start, program, error)) return false;
    }
    std::sort(program.cf.begin(), program.cf.end(),
              [](const CfInstruction& a, const CfInstruction& b) { return a.index < b.index; });
    std::sort(program.subroutines.begin(), program.subroutines.end());
    return true;
}

const CfInstruction* Program::at(uint32_t index) const {
    const auto it = std::lower_bound(cf.begin(), cf.end(), index,
                                     [](const CfInstruction& c, uint32_t i) { return c.index < i; });
    return it != cf.end() && it->index == index ? &*it : nullptr;
}

} // namespace cafe::latte
