#include "rpx.h"

#include "cafe/layout.h"

#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace cafe::rpx {
namespace {

constexpr uint32_t kMaxSectionSize = 512u * 1024u * 1024u;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("RPX: " + message);
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("cannot open " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), {}};
}

std::vector<uint8_t> inflate_section(const uint8_t* stored, uint32_t size,
                                     uint32_t index) {
    if (size < 4) {
        fail("section " + std::to_string(index) + " has truncated zlib prefix");
    }
    const uint32_t expected = read_be32(stored);
    if (expected > kMaxSectionSize) {
        fail("section " + std::to_string(index) + " is implausibly large");
    }
    std::vector<uint8_t> output(expected);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        fail("inflateInit failed");
    }
    stream.next_in = const_cast<uint8_t*>(stored + 4);
    stream.avail_in = size - 4;
    stream.next_out = output.data();
    stream.avail_out = expected;
    const int result = inflate(&stream, Z_FINISH);
    const bool exact = result == Z_STREAM_END && stream.total_out == expected &&
                       stream.avail_in == 0;
    inflateEnd(&stream);
    if (!exact) {
        fail("section " + std::to_string(index) +
             " decompresses to the wrong size");
    }
    return output;
}

std::string c_string(const Section& table, uint32_t offset) {
    if (offset >= table.data.size()) {
        fail("string offset outside " + table.name);
    }
    const auto begin = table.data.begin() + offset;
    const auto end = std::find(begin, table.data.end(), uint8_t{0});
    if (end == table.data.end()) {
        fail("unterminated string in " + table.name);
    }
    return {begin, end};
}

std::string import_module_name(const std::string& section_name) {
    std::string name = section_name;
    for (const char* prefix : {".fimport_", ".dimport_"}) {
        if (name.rfind(prefix, 0) == 0) {
            name.erase(0, std::char_traits<char>::length(prefix));
        }
    }
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".rpl") == 0) {
        name.resize(name.size() - 4);
    }
    return name;
}

uint32_t relocation_value(const Image& image, const Relocation& relocation) {
    if (relocation.symbol >= image.symbols.size()) {
        fail("relocation symbol index out of range");
    }
    return image.symbols[relocation.symbol].value +
           static_cast<uint32_t>(relocation.addend);
}

void apply_relocation(Image& image, const Relocation& relocation) {
    Section& target = image.sections.at(relocation.target_section);
    const uint32_t value = relocation_value(image, relocation);
    const uint32_t place = relocation.offset;
    const uint32_t width = relocation.type == kRPpcAddr32 ||
                                   relocation.type == kRPpcRel24 ||
                                   relocation.type == kRPpcRel14
                               ? 4
                               : 2;
    if (!target.contains(place) || place - target.address + width > target.size ||
        target.data.size() != target.size) {
        fail("relocation outside its target section");
    }
    uint8_t* data = target.data.data() + (place - target.address);
    switch (relocation.type) {
    case kRPpcAddr32:
        write_be32(data, value);
        return;
    case kRPpcAddr16Lo:
        write_be16(data, static_cast<uint16_t>(value));
        return;
    case kRPpcAddr16Hi:
        write_be16(data, static_cast<uint16_t>(value >> 16));
        return;
    case kRPpcAddr16Ha:
        write_be16(data, static_cast<uint16_t>((value + 0x8000) >> 16));
        return;
    case kRPpcRel24: {
        // Import stubs live far beyond a 26-bit displacement. The Cafe loader
        // bridges them with trampolines; the recompiler instead consults
        // rel24_targets, so only in-range branches are rewritten here.
        const int64_t delta = int64_t{value} - int64_t{place};
        if (delta >= -0x2000000 && delta < 0x2000000) {
            const uint32_t word = read_be32(data);
            write_be32(data, (word & ~0x03FFFFFCu) |
                                 (static_cast<uint32_t>(delta) & 0x03FFFFFCu));
        }
        return;
    }
    case kRPpcRel14: {
        const int64_t delta = int64_t{value} - int64_t{place};
        if (delta < -0x8000 || delta >= 0x8000) {
            fail("REL14 relocation out of range");
        }
        const uint32_t word = read_be32(data);
        write_be32(data, (word & ~0xFFFCu) |
                             (static_cast<uint32_t>(delta) & 0xFFFCu));
        return;
    }
    default:
        fail("unsupported relocation type " + std::to_string(relocation.type));
    }
}

} // namespace

const Section* Image::section_named(const std::string& name) const {
    for (const auto& section : sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

const Section* Image::section_containing(uint32_t address) const {
    for (const auto& section : sections) {
        if (section.allocated() && section.size != 0 &&
            section.contains(address)) {
            return &section;
        }
    }
    return nullptr;
}

Image load(const std::filesystem::path& path, bool apply_relocations) {
    return load(read_file(path), apply_relocations);
}

Image load(const std::vector<uint8_t>& raw, bool apply_relocations) {
    if (raw.size() < 52 || raw[0] != 0x7F || raw[1] != 'E' || raw[2] != 'L' ||
        raw[3] != 'F' || raw[4] != 1 || raw[5] != 2) {
        fail("not a big-endian ELF32 file");
    }
    const uint16_t machine = read_be16(&raw[18]);
    const uint32_t shoff = read_be32(&raw[32]);
    const uint16_t shentsize = read_be16(&raw[46]);
    const uint16_t shnum = read_be16(&raw[48]);
    const uint16_t shstrndx = read_be16(&raw[50]);
    if (machine != 20 || shentsize != 40 || shnum == 0 || shstrndx >= shnum ||
        uint64_t{shoff} + uint64_t{shnum} * 40 > raw.size()) {
        fail("unsupported or truncated section header table");
    }

    Image image;
    image.entry_point = read_be32(&raw[24]);
    image.sections.resize(shnum);
    std::vector<uint32_t> name_offsets(shnum);
    for (uint32_t index = 0; index < shnum; ++index) {
        const uint8_t* header = &raw[shoff + index * 40];
        Section& section = image.sections[index];
        section.index = index;
        name_offsets[index] = read_be32(header);
        section.type = read_be32(header + 4);
        section.flags = read_be32(header + 8);
        section.address = read_be32(header + 12);
        const uint32_t offset = read_be32(header + 16);
        const uint32_t stored_size = read_be32(header + 20);
        section.link = read_be32(header + 24);
        section.info = read_be32(header + 28);
        section.alignment = read_be32(header + 32);
        section.entry_size = read_be32(header + 36);
        if (section.type == kShtNobits || section.type == kShtNull) {
            section.size = stored_size;
            continue;
        }
        if (uint64_t{offset} + stored_size > raw.size()) {
            fail("section " + std::to_string(index) + " exceeds the file");
        }
        if (section.flags & kShfRplZlib) {
            section.data = inflate_section(&raw[offset], stored_size, index);
        } else {
            section.data.assign(raw.begin() + offset,
                                raw.begin() + offset + stored_size);
        }
        section.size = static_cast<uint32_t>(section.data.size());
    }
    for (auto& section : image.sections) {
        section.name = c_string(image.sections[shstrndx],
                                name_offsets[section.index]);
    }

    for (const auto& section : image.sections) {
        if (section.type == kShtRplFileInfo) {
            if (section.data.size() < 0x40 ||
                read_be32(section.data.data()) != 0xCAFE0402) {
                fail("unsupported RPL file info");
            }
            const uint8_t* p = section.data.data();
            FileInfo& fi = image.file_info;
            uint32_t* fields[] = {&fi.text_size, &fi.text_align, &fi.data_size,
                                  &fi.data_align, &fi.load_size, &fi.load_align,
                                  &fi.temp_size, &fi.tramp_adjust, &fi.sda_base,
                                  &fi.sda2_base, &fi.stack_size,
                                  &fi.filename_offset, &fi.flags, &fi.heap_size,
                                  &fi.tag_offset, &fi.min_version};
            for (size_t i = 0; i < std::size(fields); ++i) {
                *fields[i] = read_be32(p + 4 + i * 4);
            }
        }
    }

    const Section* symtab = nullptr;
    for (const auto& section : image.sections) {
        if (section.type == kShtSymtab) {
            if (symtab != nullptr) {
                fail("multiple symbol tables");
            }
            symtab = &section;
        }
    }
    if (symtab == nullptr) {
        fail("no symbol table");
    }
    if (symtab->entry_size != 16 || symtab->data.size() % 16 != 0 ||
        symtab->link >= image.sections.size()) {
        fail("malformed symbol table");
    }
    const Section& strtab = image.sections[symtab->link];
    const size_t symbol_count = symtab->data.size() / 16;
    image.symbols.resize(symbol_count);
    for (size_t index = 0; index < symbol_count; ++index) {
        const uint8_t* entry = symtab->data.data() + index * 16;
        Symbol& symbol = image.symbols[index];
        const uint32_t name = read_be32(entry);
        if (name != 0) {
            symbol.name = c_string(strtab, name);
        }
        symbol.value = read_be32(entry + 4);
        symbol.size = read_be32(entry + 8);
        symbol.type = entry[12] & 0xF;
        symbol.bind = entry[12] >> 4;
        symbol.section = read_be16(entry + 14);
        if (symbol.section != 0 && symbol.section < image.sections.size() &&
            image.sections[symbol.section].type == kShtRplImports) {
            const Section& stubs = image.sections[symbol.section];
            if (!stubs.contains(symbol.value)) {
                fail("import symbol outside its stub section");
            }
            // Per-object copies of each import survive with stripped names;
            // only the named symbol defines the import, the rest alias its slot.
            if (symbol.type == kSttSection || symbol.name.empty()) {
                continue;
            }
            if (!image.import_by_stub.emplace(symbol.value, image.imports.size())
                     .second) {
                fail("two imports share stub " + symbol.name);
            }
            image.imports.push_back({import_module_name(stubs.name), symbol.name,
                                     symbol.value,
                                     stubs.name.rfind(".dimport_", 0) == 0,
                                     symbol.value});
        }
    }

    // Give each data import a slot of its own; unnamed aliases share the
    // stub value, so remapping by value moves them too.
    std::map<uint32_t, uint32_t> data_slots;
    for (auto& import : image.imports) {
        if (import.is_data) {
            import.address = layout::kDataImportBase +
                             static_cast<uint32_t>(data_slots.size()) *
                                 layout::kDataImportSlot;
            if (import.address >= layout::kDataImportLimit) {
                fail("too many data imports for the layout");
            }
            data_slots[import.stub_address] = import.address;
        }
    }
    for (auto& symbol : image.symbols) {
        if (symbol.section != 0 && symbol.section < image.sections.size() &&
            image.sections[symbol.section].type == kShtRplImports) {
            const auto slot = data_slots.find(symbol.value);
            if (slot != data_slots.end()) {
                symbol.value = slot->second;
            }
        }
    }

    for (const auto& section : image.sections) {
        if (section.type != kShtRela) {
            continue;
        }
        if (section.entry_size != 12 || section.data.size() % 12 != 0 ||
            section.info >= image.sections.size() ||
            &image.sections[section.link] != symtab) {
            fail("malformed relocation section " + section.name);
        }
        for (size_t offset = 0; offset < section.data.size(); offset += 12) {
            const uint8_t* entry = section.data.data() + offset;
            const uint32_t info = read_be32(entry + 4);
            image.relocations.push_back(
                {read_be32(entry), info & 0xFF, info >> 8,
                 static_cast<int32_t>(read_be32(entry + 8)), section.info});
        }
    }
    for (const auto& relocation : image.relocations) {
        if (relocation.type == kRPpcRel24) {
            image.rel24_targets[relocation.offset] =
                relocation_value(image, relocation);
        }
        if (apply_relocations) {
            apply_relocation(image, relocation);
        }
    }
    return image;
}

} // namespace cafe::rpx
