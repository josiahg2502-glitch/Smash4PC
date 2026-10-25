#include "cafe/generated.h"
#include "cafe/layout.h"
#include "cafe/runtime.h"

#include "rpx.h"

#include "host/digest.h"

#include <cstring>
#include <string>
#include <vector>

namespace cafe {

std::string sha256_hex(const std::vector<uint8_t>& data) {
    uint8_t digest[32];
    host::sha256(data.data(), data.size(), digest);
    std::string hex;
    char byte[3];
    for (unsigned int i = 0; i < sizeof digest; ++i) {
        std::snprintf(byte, sizeof byte, "%02x", digest[i]);
        hex += byte;
    }
    return hex;
}

LoadedImage load_image(const std::vector<uint8_t>& rpx_bytes) {
    const std::string digest = sha256_hex(rpx_bytes);
    if (digest != cafe_program_info.rpx_sha256) {
        fatal("this RPX is not the executable the port was generated from\n"
              "  expected sha256 %s\n  found           %s",
              cafe_program_info.rpx_sha256, digest.c_str());
    }
    // Same loader, same relocation and data-import placement as cafe-recomp,
    // so addresses baked into the generated code match memory.
    const rpx::Image image = rpx::load(rpx_bytes);

    LoadedImage loaded;
    loaded.entry_point = image.entry_point;
    loaded.sda_base = image.file_info.sda_base;
    loaded.sda2_base = image.file_info.sda2_base;
    loaded.stack_size = image.file_info.stack_size;
    for (const auto& symbol : image.symbols) {
        if (symbol.name == "__preinit_user" && symbol.type == rpx::kSttFunc) loaded.preinit_user = symbol.value;
    }
    for (const auto& section : image.sections) {
        // Import stub sections are never executed or read: calls to them are
        // bound to native functions when the code is generated.
        if (!section.allocated() || section.size == 0 ||
            section.type == rpx::kShtRplImports || section.address == 0) {
            continue;
        }
        if (section.type != rpx::kShtProgbits && section.type != rpx::kShtNobits) {
            continue;
        }
        commit_guest_memory(section.address, section.size);
        if (section.type == rpx::kShtProgbits) {
            std::memcpy(guest_pointer(section.address), section.data.data(), section.size);
        }
        if (section.address >= 0x10000000u) {
            loaded.data_end = std::max(loaded.data_end, section.address + section.size);
        }
    }
    // Storage for imported data objects (filled in by the libraries that own
    // them) and the address range handed out for host function pointers.
    commit_guest_memory(layout::kDataImportBase,
                        layout::kDataImportLimit - layout::kDataImportBase);
    return loaded;
}

} // namespace cafe
