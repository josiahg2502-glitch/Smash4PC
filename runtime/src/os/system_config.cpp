// MCP product settings and UC system configuration: the console's region,
// language and the like, as an EU console set to English (United Kingdom),
// matching the title's region.

#include "kernel.h"

#include "cafe/export.h"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace cafe::os {
namespace {

constexpr uint8_t kRegionEurope = 4;
constexpr uint32_t kLanguageEnglish = 1;
constexpr uint32_t kCountryUnitedKingdom = 110;

// MCP ---------------------------------------------------------------------
uint32_t MCP_Open() { return 1; }
void MCP_Close(uint32_t) {}

// SysProdSettings, 0x46 bytes: +0x03 platform region, +0x0B game region.
int32_t MCP_GetSysProdSettings(uint32_t, uint8_t* settings) {
    std::memset(settings, 0, 0x46);
    settings[0x03] = kRegionEurope;
    settings[0x0B] = kRegionEurope;
    return 0;
}

// UC ----------------------------------------------------------------------
// UCSysConfig entry, 0x54 bytes: name[64], access, data type, error, data
// size, data pointer.
struct UCEntry {
    char name[64];
    be<uint32_t> access;
    be<uint32_t> type;
    be<int32_t> error;
    be<uint32_t> size;
    be<uint32_t> data;
};
static_assert(sizeof(UCEntry) == 0x54);

int32_t UCOpen() { return 1; }
int32_t UCClose(int32_t) { return 0; }

bool lookup(std::string_view name, uint32_t& value) {
    struct Setting { std::string_view name; uint32_t value; };
    static constexpr Setting kSettings[] = {
        {"cafe.language", kLanguageEnglish},
        {"cafe.cntry_reg", kCountryUnitedKingdom},
        {"cafe.initial_launch", 2}, // set up, with a user
        {"cafe.eula_version", 0},
        {"cafe.eula_agree", 1},
        {"cafe.version", 0},
        {"cafe.eco", 0},
        {"cafe.fast_boot", 0},
        {"parent.enable", 0},
        {"nn.act.account_repaired", 0},
        {"p_acct1.net_communication_on_game", 0},
        {"p_acct1.int_movie", 0},
        {"p_acct1.network_launcher", 0},
        {"p_acct1.game_rating", 18},
    };
    for (const Setting& s : kSettings) {
        if (s.name == name) {
            value = s.value;
            return true;
        }
    }
    return false;
}

// Values are written at the width the caller asks for.
int32_t UCReadSysConfig(int32_t, uint32_t count, UCEntry* entries) {
    for (uint32_t i = 0; i < count; ++i) {
        UCEntry& e = entries[i];
        const std::string_view name(e.name, strnlen(e.name, sizeof e.name));
        uint8_t* out = guest<uint8_t>(e.data);
        const uint32_t size = e.size;
        uint32_t value = 0;
        const bool known = lookup(name, value);
        if (!known) {
            std::fprintf(stderr, "ttt2: UCReadSysConfig: unknown setting %.*s (size %u)\n",
                         static_cast<int>(name.size()), name.data(), size);
        }
        e.error = 0;
        if (out == nullptr) continue;
        std::memset(out, 0, size);
        if (size == 1) *out = static_cast<uint8_t>(value);
        else if (size == 2) *reinterpret_cast<be<uint16_t>*>(out) = static_cast<uint16_t>(value);
        else if (size >= 4) *reinterpret_cast<be<uint32_t>*>(out) = value;
    }
    return 0;
}

int32_t UCWriteSysConfig(int32_t, uint32_t, uint32_t) { return 0; }
int32_t UCDeleteSysConfig(int32_t, uint32_t, uint32_t) { return 0; }

} // namespace

CAFE_EXPORT(coreinit, MCP_Open, MCP_Open);
CAFE_EXPORT(coreinit, MCP_Close, MCP_Close);
CAFE_EXPORT(coreinit, MCP_GetSysProdSettings, MCP_GetSysProdSettings);
CAFE_EXPORT(coreinit, UCOpen, UCOpen);
CAFE_EXPORT(coreinit, UCClose, UCClose);
CAFE_EXPORT(coreinit, UCReadSysConfig, UCReadSysConfig);
CAFE_EXPORT(coreinit, UCWriteSysConfig, UCWriteSysConfig);
CAFE_EXPORT(coreinit, UCDeleteSysConfig, UCDeleteSysConfig);

} // namespace cafe::os
