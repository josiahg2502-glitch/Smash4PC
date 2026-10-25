// nn_fp, the friends service: with the player's network account
// (os/online_identity.h) a session with the friends server (NEX, Cemu's
// NexFriends client) for login, the friend list and presence; without one,
// an offline console whose logins fail. Async calls end in the title's
// callback on a runtime thread, as the console's IPC replies do.
// After Cemu's iosu_fpd and nn_fp (MPL 2.0).

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/sysmem.h"
#include "os/act_server.h"
#include "os/online_identity.h"

#include "cemu_compat.h"
#include "Cemu/nex/nex.h"
#include "Cemu/nex/nexFriends.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#endif

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// NexFriends tells the friends server which title the player is in.
namespace CafeSystem {
uint64 GetForegroundTitleId() { return 0x0005000010144F00ull; }
uint16 GetForegroundTitleVersion() { return 304; }
} // namespace CafeSystem

namespace cafe::os {
namespace {

using Result = uint32_t;
constexpr uint32_t kModuleFp = 0x0B;
constexpr Result kSuccess = 0;
constexpr Result kOffline = (5u << 29) | (kModuleFp << 20) | 0x300;
constexpr Result kFailed = (7u << 29) | (kModuleFp << 20); // the server did not answer
constexpr uint8_t kRelationshipFriend = 3;

bool online() { return online::ready(); }

// TTT2_TRACE_FP=1: each call the title makes, and each callback it gets.
void trace(const char* what) {
    static const bool on = std::getenv("TTT2_TRACE_FP") != nullptr;
    if (on) std::fprintf(stderr, "fp: %s\n", what);
}

// ------------------------------------------------------------ title callbacks
struct Callback {
    uint32_t function;
    std::vector<uint32_t> args;
    uint32_t result = 0; // a guest nn::Result the call points at, freed after
};
std::mutex g_callback_mutex;
std::condition_variable g_callback_cv;
std::deque<Callback> g_callbacks;
Thread* g_callback_thread = nullptr;

void* callback_main(void*) {
    bind_current_thread(g_callback_thread);
    for (;;) {
        Callback c;
        {
            std::unique_lock lock(g_callback_mutex);
            g_callback_cv.wait(lock, [] { return !g_callbacks.empty(); });
            c = std::move(g_callbacks.front());
            g_callbacks.pop_front();
        }
        trace("callback starts");
        switch (c.args.size()) {
        case 2: call_guest(g_callback_thread->ctx, c.function, {c.args[0], c.args[1]}); break;
        case 3: call_guest(g_callback_thread->ctx, c.function, {c.args[0], c.args[1], c.args[2]}); break;
        default: break;
        }
        trace("callback returned");
        if (c.result) system_free(c.result);
    }
    return nullptr;
}

void post(Callback c) {
    {
        std::lock_guard lock(g_callback_mutex);
        if (g_callback_thread == nullptr) {
            g_callback_thread = create_internal_thread("nn_fp callbacks", 1);
            pthread_t host;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 8u << 20);
            pthread_create(&host, &attr, callback_main, nullptr);
            pthread_attr_destroy(&attr);
            pthread_detach(host);
        }
        g_callbacks.push_back(std::move(c));
    }
    g_callback_cv.notify_one();
}

// callback(const nn::Result*, param): FP callbacks take the result by reference.
void reply(uint32_t function, uint32_t param, Result result) {
    if (function == 0) return;
    const uint32_t r = system_alloc(4);
    *guest<be<uint32_t>>(r) = result;
    post({function, {r, param}, r});
}

// ------------------------------------------------------------------ session
std::recursive_mutex g_mutex;
std::unique_ptr<NexFriends> g_session;
nexPresenceV2 g_presence{};
bool g_login_called = false;
uint32_t g_notify_function = 0, g_notify_param = 0, g_notify_mask = 0;

void on_notification(NexFriends::NOTIFICATION_TYPE type, uint32 pid) {
    std::lock_guard lock(g_mutex);
    if (g_notify_function && (g_notify_mask & (1u << type))) post({g_notify_function, {uint32_t(type), pid, g_notify_param}});
}

// Signs in to the friends server; false if the account server or the name lookup fails.
bool start_session() {
    std::lock_guard lock(g_mutex);
    if (g_session) return true;
    const act_server::NexToken t = act_server::nex_token(0x0005001010001C00ull, 0, 0x00003200);
    if (!t.ok) return false;
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(t.host.c_str(), nullptr, &hints, &found) != 0 || !found) return false;
    const uint32_t ip = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    const online::Account& a = online::account();
    g_presence = {};
    g_presence.isOnline = 1;
    g_presence.gameKey.titleId = CafeSystem::GetForegroundTitleId();
    g_presence.gameKey.ukn = CafeSystem::GetForegroundTitleVersion();
    std::array<uint8_t, FFL_SIZE> mii{};
    std::memcpy(mii.data(), a.mii.data(), FFL_SIZE);
    // shortcut: country index 49 (United States); map account.dat's Country when another region needs it.
    g_session = std::make_unique<NexFriends>(ip, t.port, "ridfebb9", a.principal_id, t.password.c_str(), t.token.c_str(),
                                             a.account_id.c_str(), mii.data(), a.mii_name.data(), uint8_t(49), g_presence);
    g_session->setNotificationHandler(on_notification);
    // Logins, retries and change tracking run from update(), once a second (as Cemu's FPD timer does).
    static bool pumping = false;
    if (!pumping) {
        pumping = true;
        std::thread([] {
            for (;;) {
                {
                    std::lock_guard lock(g_mutex);
                    if (g_session) g_session->update();
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }).detach();
    }
    std::fprintf(stderr, "ttt2: online: friends server session started\n");
    return true;
}

// ---------------------------------------------------------------- the calls
Result fp_ok() {
    trace(__func__);
    return kSuccess;
}
bool fp_is_initialized() {
    trace(__func__);
    return true;
}

// Logs in to the friends server in the background, then the title's callback (if any) gets the result.
void begin_login(uint32_t callback, uint32_t param) {
    g_login_called = true;
    std::thread([callback, param] {
        bool ok = start_session();
        for (int i = 0; ok && i < 300; ++i) { // up to 30 s
            {
                std::lock_guard lock(g_mutex);
                if (g_session && g_session->isOnline()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (i == 299) ok = false;
        }
        std::fprintf(stderr, "ttt2: online: friends login %s\n", ok ? "done" : "failed");
        reply(callback, param, ok ? kSuccess : kFailed);
    }).detach();
}

Result fp_login_async(uint32_t callback, uint32_t param) {
    trace(__func__);
    if (!online()) return kOffline;
    begin_login(callback, param);
    return kSuccess;
}

bool fp_has_logged_in() {
    trace(__func__);
    return online() && g_login_called;
}

Result fp_logout() {
    trace(__func__);
    std::lock_guard lock(g_mutex);
    g_session.reset();
    g_login_called = false;
    return kSuccess;
}

uint32_t fp_principal_id() {
    trace(__func__);
    return online() ? online::account().principal_id : 0;
}

Result fp_friend_list(be<uint32_t>* pids, be<uint32_t>* count, uint32_t offset, uint32_t max) {
    trace(__func__);
    if (count) *count = 0u;
    std::lock_guard lock(g_mutex);
    if (!g_session) return kSuccess;
    std::vector<uint32> list(std::max<uint32_t>(max, 1));
    uint32 n = 0;
    g_session->getFriendPIDs(list.data(), &n, static_cast<sint32>(offset), static_cast<sint32>(max), false);
    for (uint32 i = 0; i < n && pids; ++i) pids[i] = list[i];
    if (count) *count = n;
    return kSuccess;
}

Result fp_empty_list(GuestAddress, be<uint32_t>* count, uint32_t, uint32_t) {
    trace(__func__);
    if (count) *count = 0u;
    return kSuccess;
}

// UTF-8 to UTF-16BE, `length` units with the terminator.
void put_utf16(const char* text, be<uint16_t>* out, size_t length) {
    size_t n = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p && n + 1 < length;) {
        uint32_t c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) {
            c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = (c & 0x1F) << 6 | (p[0] & 0x3F);
            p += 1;
        }
        out[n++] = static_cast<uint16_t>(c);
    }
    for (; n < length; ++n) out[n] = 0;
}

Result fp_friend_screen_names(be<uint16_t>* names, const be<uint32_t>* pids, uint32_t count, uint8_t, uint8_t*) {
    trace(__func__);
    std::lock_guard lock(g_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        nexFriend f;
        const bool known = g_session && g_session->getFriendByPID(f, pids[i]);
        put_utf16(known ? f.nnaInfo.principalInfo.mii.miiNickname : "", names + 11 * i, 11);
    }
    return kSuccess;
}

Result fp_friend_miis(uint8_t* miis, const be<uint32_t>* pids, uint32_t count) {
    trace(__func__);
    std::lock_guard lock(g_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        nexFriend f;
        if (g_session && g_session->getFriendByPID(f, pids[i])) std::memcpy(miis + FFL_SIZE * i, f.nnaInfo.principalInfo.mii.miiData, FFL_SIZE);
        else std::memset(miis + FFL_SIZE * i, 0, FFL_SIZE);
    }
    return kSuccess;
}

// FriendPresence, 0x34 bytes: GameMode (0x2C), profile (4), online, valid.
void put_game_mode(uint8_t* out, const nexPresenceV2& p) {
    auto* w = reinterpret_cast<be<uint32_t>*>(out);
    w[0] = p.joinFlagMask;
    w[1] = uint32_t(p.joinAvailability);
    w[2] = p.gameId;
    w[3] = p.gameMode;
    w[4] = p.hostPid;
    w[5] = p.groupId;
    std::memcpy(out + 0x18, p.appSpecificData, 0x14);
}

Result fp_friend_presences(uint8_t* out, const be<uint32_t>* pids, uint32_t count) {
    trace(__func__);
    std::lock_guard lock(g_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* p = out + 0x34 * i;
        std::memset(p, 0, 0x34);
        nexFriend f;
        if (!g_session || !g_session->getFriendByPID(f, pids[i])) continue;
        put_game_mode(p, f.presence);
        p[0x30] = f.presence.isOnline ? 1 : 0;
        p[0x31] = 1;
    }
    return kSuccess;
}

Result fp_friend_relationships(uint8_t* out, const be<uint32_t>* pids, uint32_t count) {
    trace(__func__);
    std::lock_guard lock(g_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        nexFriend f;
        out[i] = g_session && g_session->getFriendByPID(f, pids[i]) ? kRelationshipFriend : 0;
    }
    return kSuccess;
}

// GameMode (0x2C bytes) -> the presence other players see.
Result fp_update_game_mode(const uint8_t* mode, uint32_t, uint32_t) {
    trace(__func__);
    std::lock_guard lock(g_mutex);
    if (!g_session || !mode) return kSuccess;
    const auto* w = reinterpret_cast<const be<uint32_t>*>(mode);
    g_presence.joinFlagMask = w[0];
    g_presence.joinAvailability = static_cast<uint8_t>(uint32_t(w[1]));
    g_presence.gameId = w[2];
    g_presence.gameMode = w[3];
    g_presence.hostPid = w[4];
    g_presence.groupId = w[5];
    std::memcpy(g_presence.appSpecificData, mode + 0x18, 0x14);
    g_session->updateMyPresence(g_presence);
    return kSuccess;
}

Result fp_set_notification_handler(uint32_t mask, uint32_t function, uint32_t param) {
    trace(__func__);
    std::lock_guard lock(g_mutex);
    g_notify_mask = mask;
    g_notify_function = function;
    g_notify_param = param;
    // A console logs in to the friends server itself at start and tells titles once it is online;
    // titles that never call LoginAsync wait for that notification.
    if (online() && !g_login_called) begin_login(0, 0);
    return kSuccess;
}

Result fp_add_recent_play(uint32_t, uint32_t) {
    trace(__func__);
    return kSuccess;
}

// Account registration happened on the console; friend requests and blocks are left to the console too.
Result fp_register_account_async(uint32_t callback, uint32_t param) {
    trace(__func__);
    reply(callback, param, online() ? kSuccess : kOffline);
    return kSuccess;
}
Result fp_unsupported_async(uint32_t, uint32_t callback, uint32_t param) {
    trace(__func__);
    reply(callback, param, kOffline);
    return kSuccess;
}
Result fp_friend_request_async(uint32_t, uint8_t, uint32_t, uint32_t, uint32_t callback, uint32_t param) {
    trace(__func__);
    reply(callback, param, kOffline);
    return kSuccess;
}

uint32_t fp_result_to_error_code(uint32_t result) {
    trace(__func__);
    return 1210000 + (result & 0xFFFF);
}

} // namespace

// SMASH_ONLINE_TEST: the friends login on its own, without the title.
bool test_friends_login() {
    if (!start_session()) return false;
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard lock(g_mutex);
            if (g_session->isOnline()) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

CAFE_EXPORT(nn_fp, Initialize__Q2_2nn2fpFv, fp_ok);
CAFE_EXPORT(nn_fp, Finalize__Q2_2nn2fpFv, fp_ok);
CAFE_EXPORT(nn_fp, IsInitialized__Q2_2nn2fpFv, fp_is_initialized);
CAFE_EXPORT(nn_fp, LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv, fp_login_async);
CAFE_EXPORT(nn_fp, HasLoggedIn__Q2_2nn2fpFv, fp_has_logged_in);
CAFE_EXPORT(nn_fp, Logout__Q2_2nn2fpFv, fp_logout);
CAFE_EXPORT(nn_fp, GetMyPrincipalId__Q2_2nn2fpFv, fp_principal_id);
CAFE_EXPORT(nn_fp, GetFriendList__Q2_2nn2fpFPUiT1UiT3, fp_friend_list);
CAFE_EXPORT(nn_fp, GetBlackList__Q2_2nn2fpFPUiT1UiT3, fp_empty_list);
CAFE_EXPORT(nn_fp, GetFriendScreenName__Q2_2nn2fpFPA11_wPCUiUibPUc, fp_friend_screen_names);
CAFE_EXPORT(nn_fp, GetFriendMii__Q2_2nn2fpFP12FFLStoreDataPCUiUi, fp_friend_miis);
CAFE_EXPORT(nn_fp, GetFriendPresence__Q2_2nn2fpFPQ3_2nn2fp14FriendPresencePCUiUi, fp_friend_presences);
CAFE_EXPORT(nn_fp, GetFriendRelationship__Q2_2nn2fpFPUcPCUiUi, fp_friend_relationships);
CAFE_EXPORT(nn_fp, UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCwUi, fp_update_game_mode);
CAFE_EXPORT(nn_fp, SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv, fp_set_notification_handler);
CAFE_EXPORT(nn_fp, AddRecentPlayRecord__Q2_2nn2fpFPCQ3_2nn2fp16RecentPlayRecordUi, fp_add_recent_play);
CAFE_EXPORT(nn_fp, RegisterAccountAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv, fp_register_account_async);
CAFE_EXPORT(nn_fp, AddBlackListAsync__Q2_2nn2fpFUiPFQ2_2nn6ResultPv_vPv, fp_unsupported_async);
CAFE_EXPORT(nn_fp, AddFriendRequestAsync__Q2_2nn2fpFUiUcPCwT2T3PFQ2_2nn6ResultPv_vPv, fp_friend_request_async);
CAFE_EXPORT(nn_fp, ResultToErrorCode__Q2_2nn2fpFQ2_2nn6Result, fp_result_to_error_code);

} // namespace cafe::os
