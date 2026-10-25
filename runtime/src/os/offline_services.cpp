// Services Smash imports that Tekken does not, answered as by a console
// without them: no local wireless, amiibo reader, microphone, Wii Remotes
// or GameCube adapter; DLC as register_aoc() gives it. Settings calls for
// missing devices are accepted and ignored.

#include "cafe/export.h"
#include "cafe/guest.h"
#include "cafe/vfs.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace cafe::os {
namespace {

void no_op() {}
int32_t zero() { return 0; }
int32_t no_device() { return -1; }
uint64_t zero64() { return 0; }

int32_t VPADGetLcdMode(uint32_t, be<uint32_t>* mode) {
    if (mode) *mode = 1; // the GamePad screen is on
    return 0;
}

// --------------------------------- nn_nfp: the reader never finds an amiibo
// States from GetNfpState: none, initialized, searching for a tag.
enum NfpState : uint32_t { kNfpNone = 0, kNfpInit = 1, kNfpSearch = 2 };
std::atomic<uint32_t> g_nfp_state{kNfpNone};
constexpr uint32_t kNfpNoTag = 0xA1B10400u; // nn::Result: failure, nn_nfp, no tag present

uint32_t nfp_initialize() { g_nfp_state = kNfpInit; return 0; }
uint32_t nfp_finalize() { g_nfp_state = kNfpNone; return 0; }
uint32_t nfp_start_detection() { g_nfp_state = kNfpSearch; return 0; }
uint32_t nfp_stop_detection() { g_nfp_state = kNfpInit; return 0; }
uint32_t nfp_state() { return g_nfp_state; }
uint32_t nfp_ok() { return 0; }
uint32_t nfp_no_tag() { return kNfpNoTag; }

// ------------------------------------------------- nn_aoc: the DLC titles register_aoc() gave
// The titles' content folders are mounted at /vol/aoc<title id> (vfs::mount_aoc); an item is owned when its
// content/<id, 4 hex digits>/ folder is there, as a dump of the console's DLC has it.
constexpr int32_t kAocNotFound = -2; // AOC_ERROR: the title has no such content

struct AocTitle {
    uint64_t id;
    uint32_t group;
    uint16_t version;
    std::filesystem::path content;
};
std::vector<AocTitle> g_aoc_titles;

// AOCTitle as the title has it: {u64 title id, u32 group id, u16 version, char path[88], 2 bytes}.
constexpr uint32_t kAocTitleSize = 0x68;

uint32_t AOC_CalculateWorkBufferSize(uint32_t count) { return 0x80 + std::min(count, 256u) * 0x61; }
int32_t AOC_ListTitle(be<uint32_t>* count, GuestAddress list, uint32_t max, GuestAddress, uint32_t) {
    uint32_t n = 0;
    for (const AocTitle& t : g_aoc_titles) {
        if (n >= max || list.value == 0) break;
        uint8_t* out = guest_pointer(list.value + n * kAocTitleSize);
        std::memset(out, 0, kAocTitleSize);
        *reinterpret_cast<be<uint64_t>*>(out) = t.id;
        *reinterpret_cast<be<uint32_t>*>(out + 8) = t.group;
        *reinterpret_cast<be<uint16_t>*>(out + 12) = t.version;
        const std::string path = vfs::aoc_path(t.id);
        std::memcpy(out + 14, path.c_str(), std::min<size_t>(path.size(), 87));
        ++n;
    }
    if (count) *count = n;
    return 0;
}
int32_t AOC_OpenTitle(GuestAddress path_out, GuestAddress title, GuestAddress, uint32_t) {
    if (path_out.value == 0 || title.value == 0) return kAocNotFound;
    const uint8_t* t = guest_pointer(title.value);
    const char* path = reinterpret_cast<const char*>(t + 14);
    if (path[0] == 0) return kAocNotFound;
    std::memcpy(guest_pointer(path_out.value), path, strnlen(path, 87) + 1);
    std::fprintf(stderr, "ttt2: aoc: opened %s\n", path);
    return 0;
}
int32_t AOC_GetPurchaseInfo(GuestAddress purchased, uint64_t title_id, GuestAddress ids, uint32_t count, GuestAddress,
                            uint32_t) {
    const auto title = std::find_if(g_aoc_titles.begin(), g_aoc_titles.end(), [&](const AocTitle& t) { return t.id == title_id; });
    if (title == g_aoc_titles.end() || purchased.value == 0 || ids.value == 0) return kAocNotFound;
    auto* out = reinterpret_cast<be<uint32_t>*>(guest_pointer(purchased.value));
    const auto* items = reinterpret_cast<const be<uint16_t>*>(guest_pointer(ids.value));
    uint32_t owned = 0;
    for (uint32_t i = 0; i < count; ++i) {
        char name[8];
        std::snprintf(name, sizeof(name), "%04x", unsigned{items[i]});
        std::error_code ec;
        out[i] = std::filesystem::is_directory(title->content / name, ec) ? 1u : 0u;
        owned += out[i];
    }
    // (Smash asks for one item at a time: a line per hundred questions.)
    static uint32_t asked = 0, yes = 0;
    asked += count, yes += owned;
    if (asked % 100 < count) std::fprintf(stderr, "ttt2: aoc: %u items asked about, %u owned\n", asked, yes);
    return 0;
}


} // namespace

CAFE_EXPORT(nn_uds, __sti___11_uds_Api_cpp_f5d9abb2, no_op); // the library's static constructor

CAFE_EXPORT(nn_nfp, Initialize__Q2_2nn3nfpFv, nfp_initialize);
CAFE_EXPORT(nn_nfp, Finalize__Q2_2nn3nfpFv, nfp_finalize);
CAFE_EXPORT(nn_nfp, StartDetection__Q2_2nn3nfpFv, nfp_start_detection);
CAFE_EXPORT(nn_nfp, StopDetection__Q2_2nn3nfpFv, nfp_stop_detection);
CAFE_EXPORT(nn_nfp, GetNfpState__Q2_2nn3nfpFv, nfp_state);
// shortcut: the info structs are left as the caller made them; zero them if a title reads them back.
CAFE_EXPORT(nn_nfp, InitializeCreateInfo__Q2_2nn3nfpFPQ3_2nn3nfp25ApplicationAreaCreateInfo, nfp_ok);
CAFE_EXPORT(nn_nfp, InitializeRegisterInfoSet__Q2_2nn3nfpFPQ3_2nn3nfp15RegisterInfoSet, nfp_ok);
// Everything that needs a tag on the reader.
CAFE_EXPORT(nn_nfp, Mount__Q2_2nn3nfpFv, nfp_no_tag);
CAFE_EXPORT(nn_nfp, Unmount__Q2_2nn3nfpFv, nfp_no_tag);
CAFE_EXPORT(nn_nfp, Flush__Q2_2nn3nfpFv, nfp_no_tag);
CAFE_EXPORT(nn_nfp, Restore__Q2_2nn3nfpFv, nfp_no_tag);
CAFE_EXPORT(nn_nfp, Format__Q2_2nn3nfpFPCUci, nfp_no_tag);
CAFE_EXPORT(nn_nfp, GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo, nfp_no_tag);
CAFE_EXPORT(nn_nfp, GetNfpCommonInfo__Q2_2nn3nfpFPQ3_2nn3nfp10CommonInfo, nfp_no_tag);
CAFE_EXPORT(nn_nfp, GetNfpReadOnlyInfo__Q2_2nn3nfpFPQ3_2nn3nfp12ReadOnlyInfo, nfp_no_tag);
CAFE_EXPORT(nn_nfp, GetNfpRegisterInfo__Q2_2nn3nfpFPQ3_2nn3nfp12RegisterInfo, nfp_no_tag);
CAFE_EXPORT(nn_nfp, SetNfpRegisterInfo__Q2_2nn3nfpFRCQ3_2nn3nfp15RegisterInfoSet, nfp_no_tag);
CAFE_EXPORT(nn_nfp, CreateApplicationArea__Q2_2nn3nfpFRCQ3_2nn3nfp25ApplicationAreaCreateInfo, nfp_no_tag);
CAFE_EXPORT(nn_nfp, DeleteApplicationArea__Q2_2nn3nfpFv, nfp_no_tag);
CAFE_EXPORT(nn_nfp, OpenApplicationArea__Q2_2nn3nfpFUi, nfp_no_tag);
CAFE_EXPORT(nn_nfp, ReadApplicationArea__Q2_2nn3nfpFPvUi, nfp_no_tag);
CAFE_EXPORT(nn_nfp, WriteApplicationArea__Q2_2nn3nfpFPCvUiRCQ3_2nn3nfp5TagId, nfp_no_tag);

// Video decoding (h264): not available yet, so every call fails and titles
// skip their movies.
// shortcut: no decoder; bridge to a host H.264 decoder when the movies should play.
CAFE_EXPORT(h264, H264DECGetImageSize, no_device);
CAFE_EXPORT(h264, H264DECMemoryRequirement, no_device);
CAFE_EXPORT(h264, H264DECCheckMemSegmentation, no_device);
CAFE_EXPORT(h264, H264DECInitParam, no_device);
CAFE_EXPORT(h264, H264DECOpen, no_device);
CAFE_EXPORT(h264, H264DECBegin, no_device);
CAFE_EXPORT(h264, H264DECSetBitstream, no_device);
CAFE_EXPORT(h264, H264DECExecute, no_device);
CAFE_EXPORT(h264, H264DECEnd, no_device);
CAFE_EXPORT(h264, H264DECClose, no_device);
CAFE_EXPORT(h264, H264DECSetParam_FPTR_OUTPUT, no_device);
CAFE_EXPORT(h264, H264DECSetParam_OUTPUT_PER_FRAME, no_device);
CAFE_EXPORT(h264, H264DECSetParam_USER_MEMORY, no_device);

// A DLC title for AOC_ListTitle, its content/ folder mounted at /vol/aoc<id>.
void register_aoc(uint64_t id, uint32_t group, uint16_t version, const std::filesystem::path& content) {
    vfs::mount_aoc(id, content);
    g_aoc_titles.push_back({id, group, version, content});
}

CAFE_EXPORT(nn_aoc, AOC_Initialize, zero);
CAFE_EXPORT(nn_aoc, AOC_CalculateWorkBufferSize, AOC_CalculateWorkBufferSize);
CAFE_EXPORT(nn_aoc, AOC_ListTitle, AOC_ListTitle);
CAFE_EXPORT(nn_aoc, AOC_OpenTitle, AOC_OpenTitle);
CAFE_EXPORT(nn_aoc, AOC_CloseTitle, zero);
CAFE_EXPORT(nn_aoc, AOC_GetPurchaseInfo, AOC_GetPurchaseInfo);
CAFE_EXPORT(nn_aoc, AOC_DebugRealDeviceAccess, zero);

// Wii Remotes (padscore): Pro Controllers only (input.cpp); the rest is setup.
CAFE_EXPORT(padscore, KPADInitEx, no_op);
CAFE_EXPORT(padscore, KPADReset, no_op);
CAFE_EXPORT(padscore, KPADEnableStickCrossClamp, no_op);
CAFE_EXPORT(padscore, KPADSetAccParam, no_op);
CAFE_EXPORT(padscore, KPADSetBtnRepeat, no_op);
CAFE_EXPORT(padscore, KPADSetControlDpdCallback, no_op);
CAFE_EXPORT(padscore, KPADSetControlMplsCallback, no_op);
CAFE_EXPORT(padscore, KPADSetDistParam, no_op);
CAFE_EXPORT(padscore, KPADSetFSStickClamp, no_op);
CAFE_EXPORT(padscore, KPADSetHoriParam, no_op);
CAFE_EXPORT(padscore, KPADSetMaxControllers, zero);
CAFE_EXPORT(padscore, KPADSetPosParam, no_op);
CAFE_EXPORT(padscore, KPADSetSamplingCallback, zero);
CAFE_EXPORT(padscore, WBCSetupCalibration, zero);
CAFE_EXPORT(padscore, WENCGetEncodeData, zero);
CAFE_EXPORT(padscore, WPADInit, no_op);
CAFE_EXPORT(padscore, WPADSetExtensionCallback, zero);
CAFE_EXPORT(padscore, WPADGetInfoAsync, no_device);
CAFE_EXPORT(padscore, WPADCanSendStreamData, zero);
CAFE_EXPORT(padscore, WPADSendStreamData, no_device);
CAFE_EXPORT(padscore, WPADControlSpeaker, no_device);

// GamePad extras.
CAFE_EXPORT(vpad, VPADEnableStickCrossClamp, no_op);
CAFE_EXPORT(vpad, VPADGetLcdMode, VPADGetLcdMode);
CAFE_EXPORT(vpad, VPADGetTVMenuStatus, zero);

// GameCube controller adapter (nn_hpad): none attached.
CAFE_EXPORT(nn_hpad, HPADInit, no_op);
CAFE_EXPORT(nn_hpad, HPADRead, zero);
CAFE_EXPORT(nn_hpad, HPADControlMotor, no_op);
CAFE_EXPORT(nn_hpad, HPADSetConnectCallback, zero);
CAFE_EXPORT(nn_hpad, HPADSetGgggConnectCallback, zero);
CAFE_EXPORT(nn_hpad, HPADSetSamplingCallback, zero);

// Microphone: none.
CAFE_EXPORT(mic, MICInit, no_device);
CAFE_EXPORT(mic, MICOpen, no_device);
CAFE_EXPORT(mic, MICClose, no_device);
CAFE_EXPORT(mic, MICUninit, no_device);
CAFE_EXPORT(mic, MICGetState, no_device);
CAFE_EXPORT(mic, MICSetState, no_device);
CAFE_EXPORT(mic, MICGetStatus, no_device);
CAFE_EXPORT(mic, MICSetDataConsumed, no_device);

// Temporary download folder (nn_temp): not available.
CAFE_EXPORT(nn_temp, TEMPInit, zero);
CAFE_EXPORT(nn_temp, TEMPShutdown, no_op);
CAFE_EXPORT(nn_temp, TEMPCreateAndInitTempDir, no_device);
CAFE_EXPORT(nn_temp, TEMPShutdownTempDir, zero);
CAFE_EXPORT(nn_temp, TEMPChangeOthersMode, zero);
CAFE_EXPORT(nn_temp, TEMPOpenFile, no_device);

// libcurl (the v1.0 game's online code): no network, so it never starts.
int32_t curl_failed_init() { return 2; } // CURLE_FAILED_INIT
CAFE_EXPORT(nlibcurl, curl_global_init, curl_failed_init);

// System applications.
CAFE_EXPORT(sysapp, SYSGetArguments, zero);
CAFE_EXPORT(sysapp, SYSSwitchToEManual, no_op);
CAFE_EXPORT(sysapp, SYSSwitchToEShop, no_op);
CAFE_EXPORT(sysapp, _SYSGetSystemApplicationTitleId, zero64);

// Audio device mixing (snd_user MIX): the runtime mixes every device the same.
CAFE_EXPORT(snd_user, MIXInitDeviceControl, no_op);
CAFE_EXPORT(snd_user, MIXSetDeviceAux, no_op);
CAFE_EXPORT(snd_user, MIXSetDeviceFader, no_op);
CAFE_EXPORT(snd_user, MIXSetInput, no_op);
CAFE_EXPORT(snd_user, MIXSetSoundMode, no_op);


} // namespace cafe::os
