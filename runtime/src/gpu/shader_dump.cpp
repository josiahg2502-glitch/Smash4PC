// TTT2_DUMP_SHADERS=<directory>: saves every distinct shader program the
// game draws with, and the context registers of each distinct combination,
// for developing the Latte shader translator offline.
//
//   <dir>/vs_<hash>.bin, ps_<hash>.bin, fs_<hash>.bin   microcode as in memory
//   <dir>/draw_<vs>_<ps>_<fs>.regs                      context registers
//                                                      0x28000-0x29000, host u32

#include "gpu/backend.h"
#include "gpu/latte.h"

#include "cafe/guest.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>

namespace cafe::gpu {
namespace {

uint64_t fnv1a(const uint8_t* data, size_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

const char* dump_directory() {
    static const char* dir = [] {
        const char* d = std::getenv("TTT2_DUMP_SHADERS");
        if (d != nullptr && *d != '\0') std::filesystem::create_directories(d);
        return (d != nullptr && *d != '\0') ? d : nullptr;
    }();
    return dir;
}

std::mutex g_mutex;
std::set<uint64_t> g_programs;
std::set<std::string> g_combinations;

uint64_t dump_program(const char* kind, uint32_t start_reg, uint32_t size_reg, const Registers& regs) {
    const uint32_t address = regs[start_reg] << 8;
    const uint32_t size = regs[size_reg] << 3;
    if (address == 0 || size == 0 || size > 0x100000) return 0;
    const uint8_t* code = guest<uint8_t>(address);
    const uint64_t hash = fnv1a(code, size);
    if (g_programs.insert(hash).second) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s_%016llx.bin", dump_directory(), kind,
                      static_cast<unsigned long long>(hash));
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(code, 1, size, f);
            std::fclose(f);
        }
    }
    return hash;
}

} // namespace

void dump_shaders(const Registers& regs) {
    if (dump_directory() == nullptr) return;
    std::lock_guard lock(g_mutex);
    using namespace latte;
    const uint64_t vs = dump_program("vs", reg::SQ_PGM_START_VS, reg::SQ_PGM_START_VS + 4, regs);
    const uint64_t ps = dump_program("ps", reg::SQ_PGM_START_PS, reg::SQ_PGM_START_PS + 4, regs);
    const uint64_t fs = dump_program("fs", reg::SQ_PGM_START_FS, reg::SQ_PGM_START_FS + 4, regs);
    char name[128];
    std::snprintf(name, sizeof(name), "draw_%016llx_%016llx_%016llx.regs", static_cast<unsigned long long>(vs),
                  static_cast<unsigned long long>(ps), static_cast<unsigned long long>(fs));
    if (!g_combinations.insert(name).second) return;
    const std::string path = std::string(dump_directory()) + "/" + name;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(&regs.value[kContextBase >> 2], 4, (kContextEnd - kContextBase) >> 2, f);
        std::fclose(f);
    }
}

} // namespace cafe::gpu
