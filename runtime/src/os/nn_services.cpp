// System services the title queries during start-up: background daemons
// (nn_ndm), network connection (nn_ac), user account (nn_act), friends
// (nn_fp) and network time (nn_acp). With the player's console files and
// network account (os/online_identity.h) the console is connected and the
// account is theirs, signing in to Pretendo for NEX tokens; without them it
// is an offline console with a local user: network functions report "no
// connection" and the account is not a network account.

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "host/local_time.h"
#include "os/act_server.h"
#include "os/online_identity.h"

namespace cafe::os {
namespace {

// TTT2_TRACE_NET=1: each network-service call the title makes (connection, account, time, sockets, curl).
inline void trace_net(const char* what) {
    static const bool on = std::getenv("TTT2_TRACE_NET") != nullptr;
    if (on) std::fprintf(stderr, "net-call: %s (lr 0x%08X)\n", what, current_context() ? current_context()->lr : 0u);
}

using Result = uint32_t; // nn::Result: bits 29-31 level, 20-28 module

constexpr Result kSuccess = 0;
// Level "status" (non-fatal failure) in the given module.
constexpr Result failure(uint32_t module, uint32_t description) {
    return (5u << 29) | (module << 20) | description;
}
constexpr uint32_t kModuleAc = 0x0C;
constexpr uint32_t kModuleAct = 0x07;
constexpr uint32_t kModuleAcp = 0x0E;
constexpr Result kNotConnected = failure(kModuleAc, 0x2580); // no network configured
constexpr Result kNoNetworkAccount = failure(kModuleAct, 0x1100);
constexpr Result kNoNetworkTime = failure(kModuleAcp, 0x100);

// ---------------------------------------------------------------- nn_ndm
Result ndm_ok() {
    trace_net(__func__);
    return kSuccess;
}
Result ndm_daemons(uint32_t) {
    trace_net(__func__);
    return kSuccess;
}
Result ndm_home_button(int32_t) {
    trace_net(__func__);
    return kSuccess;
}

// ----------------------------------------------------------------- nn_ac
constexpr uint32_t kStatusFailed = 0xFFFFFFFF;
Result ac_ok() {
    trace_net(__func__);
    return kSuccess;
}
bool online() { return online::ready(); }
// The connection comes up when the title asks for it (ConnectAsync), as on a console that was not
// connected at start: Smash, finding itself connected at start, stays on a black screen before its
// title (a step its offline start skips).
std::atomic<bool> g_connected{false};
bool ac_online() { return online() && g_connected; }
bool act_online() { return online(); }
Result ac_connect_async() {
    trace_net(__func__);
    if (!online()) return kNotConnected;
    g_connected = true;
    return kSuccess;
}
Result ac_get_status(be<uint32_t>* status) {
    trace_net(__func__);
    if (status) *status = ac_online() ? 0u : kStatusFailed;
    return kSuccess;
}
Result ac_is_application_connected(uint8_t* connected) {
    trace_net(__func__);
    if (connected) *connected = ac_online();
    return kSuccess;
}
// shortcut: a fixed private address and /24 mask; ask the host for its own if a title sends it to peers.
Result ac_get_address(be<uint32_t>* address) {
    trace_net(__func__);
    if (address) *address = ac_online() ? 0xC0A80064u : 0u; // 192.168.0.100
    return ac_online() ? kSuccess : kNotConnected;
}
Result ac_get_subnet(be<uint32_t>* mask) {
    trace_net(__func__);
    if (mask) *mask = ac_online() ? 0xFFFFFF00u : 0u;
    return ac_online() ? kSuccess : kNotConnected;
}
Result ac_get_last_error_code(be<uint32_t>* code) {
    trace_net(__func__);
    if (code) *code = 0;
    return kSuccess;
}

// ---------------------------------------------------------------- nn_act
constexpr uint8_t kSlot = 1;
constexpr uint32_t kPersistentId = 0x80000001;
Result act_ok() {
    trace_net(__func__);
    return kSuccess;
}
uint8_t act_slot() {
    trace_net(__func__);
    return kSlot;
}
uint8_t act_num_accounts() {
    trace_net(__func__);
    return 1;
}
bool act_network_account() {
    trace_net(__func__);
    return act_online();
}
uint32_t act_persistent_id() {
    trace_net(__func__);
    return kPersistentId;
}
uint32_t act_principal_id() {
    trace_net(__func__);
    return act_online() ? online::account().principal_id : 0;
}
Result act_principal_id_ex(be<uint32_t>* id, uint8_t) {
    trace_net(__func__);
    if (id) *id = act_principal_id();
    return act_online() ? kSuccess : kNoNetworkAccount;
}
Result act_account_id(char* out) {
    trace_net(__func__);
    if (out) std::snprintf(out, 17, "%s", act_online() ? online::account().account_id.c_str() : "TTT2Player");
    return kSuccess;
}
Result act_country(char* out) {
    trace_net(__func__);
    if (out) std::strcpy(out, "GB");
    return kSuccess;
}
// Offset of local time from UTC in microseconds.
int64_t act_utc_offset() {
    trace_net(__func__);
    return host::utc_offset_seconds(std::time(nullptr)) * 1000000;
}
Result act_parental_slot(uint8_t* out, uint8_t) {
    trace_net(__func__);
    if (out) *out = kSlot;
    return kSuccess;
}
Result act_nfs_password(char*, uint8_t) {
    trace_net(__func__);
    return kNoNetworkAccount;
}
Result act_host_servers(uint32_t, uint32_t, uint32_t, uint8_t) {
    trace_net(__func__);
    return kNoNetworkAccount;
}
// ACTNexAuthenticationResult (0x25C bytes): token[0x201], password[0x41] at 0x204, host[0x10] at 0x248, port at 0x258.
// shortcut: Smash's update (v304) as the title asking; take it from the loaded title if another title needs it.
constexpr uint64_t kTitleId = 0x0005000010144F00ull;
constexpr uint16_t kTitleVersion = 304;
constexpr Result kNexTokenFailed = failure(kModuleAct, 0x1E80); // could not reach the account server
Result act_nex_token(uint8_t* out, uint32_t server_id) {
    trace_net(__func__);
    if (out) std::memset(out, 0, 0x25C);
    if (!act_online()) return kNoNetworkAccount;
    const act_server::NexToken t = act_server::nex_token(kTitleId, kTitleVersion, server_id);
    if (!t.ok) return t.error_code ? failure(kModuleAct, 0x1000 + (t.error_code & 0x3FF)) : kNexTokenFailed;
    if (out) {
        std::snprintf(reinterpret_cast<char*>(out), 0x201, "%s", t.token.c_str());
        std::snprintf(reinterpret_cast<char*>(out + 0x204), 0x41, "%s", t.password.c_str());
        std::snprintf(reinterpret_cast<char*>(out + 0x248), 0x10, "%s", t.host.c_str());
        out[0x258] = static_cast<uint8_t>(t.port >> 8);
        out[0x259] = static_cast<uint8_t>(t.port);
    }
    return kSuccess;
}
// Error codes shown to the user look like 102-xxxx for nn_act.
uint32_t act_error_code(be<uint32_t>* result) {
    trace_net(__func__);
    return 1020000 + ((result ? uint32_t{*result} : 0) & 0xFFFF);
}

// The one account's Mii, name and ids, the same on every start.
// shortcut: the Mii is all zeros (no valid FFLStoreData); build a real default Mii if a title checks it.
Result act_mii(uint8_t* data) {
    trace_net(__func__);
    if (data && act_online()) std::memcpy(data, online::account().mii.data(), 0x60);
    else if (data) std::memset(data, 0, 0x60);
    return kSuccess;
}
Result act_mii_ex(uint8_t* data, uint8_t) {
    trace_net(__func__);
    return act_mii(data);
}
// UTF-16BE, at most 10 characters and a terminator.
Result put_name(be<uint16_t>* out) {
    static constexpr char kName[] = "Player";
    if (out && act_online()) {
        for (size_t i = 0; i < 11; ++i) out[i] = online::account().mii_name[i];
        out[10] = 0;
    } else if (out) {
        for (size_t i = 0; i < sizeof kName; ++i) out[i] = static_cast<uint16_t>(kName[i]);
    }
    return kSuccess;
}
uint32_t act_simple_address_id() {
    trace_net(__func__);
    return act_online() ? online::account().simple_address_id : 0;
}
uint8_t act_slot_of_uuid(GuestAddress) {
    trace_net(__func__);
    return kSlot;
}
bool act_slot_occupied(uint8_t slot) {
    trace_net(__func__);
    return slot == kSlot;
}
Result act_transferable_id_ex(be<uint64_t>* id, uint32_t, uint8_t) {
    trace_net(__func__);
    if (id) *id = act_online() ? online::account().transferable_id_base : 0x0123456789ABCDEFull;
    return kSuccess;
}
Result act_uuid(uint8_t* uuid) {
    trace_net(__func__);
    static constexpr uint8_t kUuid[16] = {0x53, 0x4D, 0x41, 0x53, 0x48, 0x57, 0x49, 0x49,
                                          0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    if (uuid) std::memcpy(uuid, act_online() ? online::account().uuid.data() : kUuid, sizeof kUuid);
    return kSuccess;
}
Result act_uuid_ex(uint8_t* uuid, uint8_t) {
    trace_net(__func__);
    return act_uuid(uuid);
}

// ---------------------------------------------------------------- nn_acp
// Network time is kept in timer ticks, like OSTime.
Result ACPConvertNetworkTimeToOSCalendarTime(int64_t time, GuestAddress calendar) {
    trace_net(__func__);
    ticks_to_calendar_time(time, calendar.value);
    return 0;
}

Result ACPGetNetworkTime(be<int64_t>* time, be<uint32_t>* unknown) {
    trace_net(__func__);
    if (time) *time = 0;
    if (unknown) *unknown = 0;
    return kNoNetworkTime;
}

} // namespace

CAFE_EXPORT(nn_ndm, Initialize__Q2_2nn3ndmFv, ndm_ok);
CAFE_EXPORT(nn_ndm, Finalize__Q2_2nn3ndmFv, ndm_ok);
CAFE_EXPORT(nn_ndm, SuspendDaemons__Q2_2nn3ndmFUi, ndm_daemons);
CAFE_EXPORT(nn_ndm, ResumeDaemons__Q2_2nn3ndmFUi, ndm_daemons);
CAFE_EXPORT(nn_ndm, EnableHomeButtonMenuAndBgProcess__Q2_2nn3ndmFi, ndm_home_button);

CAFE_EXPORT(nn_ac, Initialize__Q2_2nn2acFv, ac_ok);
CAFE_EXPORT(nn_ac, Finalize__Q2_2nn2acFv, ac_ok);
CAFE_EXPORT(nn_ac, Close__Q2_2nn2acFv, ac_ok);
CAFE_EXPORT(nn_ac, ConnectAsync__Q2_2nn2acFv, ac_connect_async);
CAFE_EXPORT(nn_ac, GetStatus__Q2_2nn2acFPQ3_2nn2ac6Status, ac_get_status);
CAFE_EXPORT(nn_ac, IsApplicationConnected__Q2_2nn2acFPb, ac_is_application_connected);
CAFE_EXPORT(nn_ac, GetAssignedAddress__Q2_2nn2acFPUl, ac_get_address);
CAFE_EXPORT(nn_ac, GetAssignedSubnet__Q2_2nn2acFPUl, ac_get_subnet);
CAFE_EXPORT(nn_ac, GetLastErrorCode__Q2_2nn2acFPUi, ac_get_last_error_code);

CAFE_EXPORT(nn_act, Initialize__Q2_2nn3actFv, act_ok);
CAFE_EXPORT(nn_act, Finalize__Q2_2nn3actFv, act_ok);
CAFE_EXPORT(nn_act, GetSlotNo__Q2_2nn3actFv, act_slot);
CAFE_EXPORT(nn_act, GetNumOfAccounts__Q2_2nn3actFv, act_num_accounts);
CAFE_EXPORT(nn_act, IsNetworkAccount__Q2_2nn3actFv, act_network_account);
CAFE_EXPORT(nn_act, HasNfsAccount__Q2_2nn3actFv, act_network_account);
CAFE_EXPORT(nn_act, GetPersistentId__Q2_2nn3actFv, act_persistent_id);
CAFE_EXPORT(nn_act, GetPrincipalId__Q2_2nn3actFv, act_principal_id);
CAFE_EXPORT(nn_act, GetPrincipalIdEx__Q2_2nn3actFPUiUc, act_principal_id_ex);
CAFE_EXPORT(nn_act, GetAccountId__Q2_2nn3actFPc, act_account_id);
CAFE_EXPORT(nn_act, GetCountry__Q2_2nn3actFPc, act_country);
CAFE_EXPORT(nn_act, GetUtcOffset__Q2_2nn3actFv, act_utc_offset);
CAFE_EXPORT(nn_act, GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc, act_parental_slot);
CAFE_EXPORT(nn_act, GetNfsPasswordEx__Q2_2nn3actFPcUc, act_nfs_password);
CAFE_EXPORT(nn_act, GetHostServerSettings__Q2_2nn3actFP11ACTNnasTypeP10ACTNfsTypePUcUc, act_host_servers);
CAFE_EXPORT(nn_act, AcquireNexServiceToken__Q2_2nn3actFP26ACTNexAuthenticationResultUi, act_nex_token);
CAFE_EXPORT(nn_act, GetErrorCode__Q2_2nn3actFRCQ2_2nn6Result, act_error_code);

CAFE_EXPORT(nn_fp, GetMyScreenName__Q2_2nn2fpFPw, put_name);

CAFE_EXPORT(nn_act, Cancel__Q2_2nn3actFv, act_ok);
CAFE_EXPORT(nn_act, GetMii__Q2_2nn3actFP12FFLStoreData, act_mii);
CAFE_EXPORT(nn_act, GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc, act_mii_ex);
CAFE_EXPORT(nn_act, GetMiiName__Q2_2nn3actFPw, put_name);
CAFE_EXPORT(nn_act, GetSimpleAddressId__Q2_2nn3actFv, act_simple_address_id);
CAFE_EXPORT(nn_act, GetSlotNoEx__Q2_2nn3actFRC7ACTUuid, act_slot_of_uuid);
CAFE_EXPORT(nn_act, IsSlotOccupied__Q2_2nn3actFUc, act_slot_occupied);
CAFE_EXPORT(nn_act, GetTransferableIdEx__Q2_2nn3actFPULUiUc, act_transferable_id_ex);
CAFE_EXPORT(nn_act, GetUuid__Q2_2nn3actFP7ACTUuid, act_uuid);
CAFE_EXPORT(nn_act, GetUuidEx__Q2_2nn3actFP7ACTUuidUc, act_uuid_ex);

CAFE_EXPORT(nn_ac, GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status, ac_get_status);

CAFE_EXPORT(nn_acp, ACPGetNetworkTime, ACPGetNetworkTime);
CAFE_EXPORT(nn_acp, ACPConvertNetworkTimeToOSCalendarTime, ACPConvertNetworkTimeToOSCalendarTime);

} // namespace cafe::os
