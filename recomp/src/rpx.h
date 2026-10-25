#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cafe::rpx {

constexpr uint32_t kShtNull = 0;
constexpr uint32_t kShtProgbits = 1;
constexpr uint32_t kShtSymtab = 2;
constexpr uint32_t kShtStrtab = 3;
constexpr uint32_t kShtRela = 4;
constexpr uint32_t kShtNobits = 8;
constexpr uint32_t kShtRplExports = 0x80000001;
constexpr uint32_t kShtRplImports = 0x80000002;
constexpr uint32_t kShtRplCrcs = 0x80000003;
constexpr uint32_t kShtRplFileInfo = 0x80000004;

constexpr uint32_t kShfWrite = 0x1;
constexpr uint32_t kShfAlloc = 0x2;
constexpr uint32_t kShfExecinstr = 0x4;
constexpr uint32_t kShfRplZlib = 0x08000000;

constexpr uint8_t kSttNotype = 0;
constexpr uint8_t kSttObject = 1;
constexpr uint8_t kSttFunc = 2;
constexpr uint8_t kSttSection = 3;
constexpr uint8_t kSttFile = 4;

constexpr uint32_t kRPpcAddr32 = 1;
constexpr uint32_t kRPpcAddr16Lo = 4;
constexpr uint32_t kRPpcAddr16Hi = 5;
constexpr uint32_t kRPpcAddr16Ha = 6;
constexpr uint32_t kRPpcRel24 = 10;
constexpr uint32_t kRPpcRel14 = 11;

struct Section {
    uint32_t index{};
    std::string name;
    uint32_t type{};
    uint32_t flags{};
    uint32_t address{};
    uint32_t size{}; // decompressed (or NOBITS) size
    uint32_t link{};
    uint32_t info{};
    uint32_t alignment{};
    uint32_t entry_size{};
    std::vector<uint8_t> data; // decompressed; empty for NOBITS

    bool allocated() const { return (flags & kShfAlloc) != 0; }
    bool executable() const { return (flags & kShfExecinstr) != 0; }
    bool contains(uint32_t addr) const {
        return addr >= address && addr - address < size;
    }
};

struct Symbol {
    std::string name;
    uint32_t value{};
    uint32_t size{};
    uint8_t type{};
    uint8_t bind{};
    uint16_t section{};
};

struct Relocation {
    uint32_t offset{};       // absolute guest address being patched
    uint32_t type{};
    uint32_t symbol{};
    int32_t addend{};
    uint32_t target_section{};
};

// One imported function or data object, reached through a stub slot inside a
// .fimport_* / .dimport_* section.
struct Import {
    std::string module; // "coreinit", "gx2", ... (".rpl" suffix removed)
    std::string name;
    uint32_t stub_address{};
    bool is_data{};
    // Where references resolve: the stub for functions, a layout slot for
    // data (see cafe/layout.h).
    uint32_t address{};
};

struct FileInfo {
    uint32_t text_size{}, text_align{}, data_size{}, data_align{};
    uint32_t load_size{}, load_align{}, temp_size{}, tramp_adjust{};
    uint32_t sda_base{}, sda2_base{}, stack_size{}, filename_offset{};
    uint32_t flags{}, heap_size{}, tag_offset{}, min_version{};
};

struct Image {
    uint32_t entry_point{};
    std::vector<Section> sections;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocations;
    std::vector<Import> imports;
    std::map<uint32_t, size_t> import_by_stub; // stub address -> imports index
    // Every REL24 site -> its absolute target, including import calls that no
    // 26-bit displacement can encode.
    std::map<uint32_t, uint32_t> rel24_targets;
    FileInfo file_info;

    const Section* section_named(const std::string& name) const;
    const Section* section_containing(uint32_t address) const;
};

// Parses and decompresses an RPX/RPL. When apply_relocations is set, every
// relocation is written into section data as the Cafe loader would with each
// section placed at its sh_addr. Function imports resolve to their stub slots;
// data imports to cafe::layout data-import slots, in symbol-table order.
Image load(const std::filesystem::path& path, bool apply_relocations = true);
Image load(const std::vector<uint8_t>& raw, bool apply_relocations = true);

inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) |
           (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}
inline uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

} // namespace cafe::rpx
