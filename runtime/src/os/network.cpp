// nsysnet's SSL and helpers, and nlibcurl, for a console without web access:
// the libraries initialise, but no SSL context can be created and every curl
// transfer finishes at once with "could not resolve host". Byte-order and
// address-formatting helpers work normally. (Sockets: sockets.cpp.)

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/sysmem.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace cafe::os {
namespace {

// TTT2_TRACE_NET=1: each network-service call the title makes (connection, account, time, sockets, curl).
inline void trace_net(const char* what) {
    static const bool on = std::getenv("TTT2_TRACE_NET") != nullptr;
    if (on) std::fprintf(stderr, "net-call: %s\n", what);
}

// Guest data is already big-endian: network order is host order there.
uint32_t byte_order32(uint32_t v) { return v; }
uint16_t byte_order16(uint16_t v) { return v; }

uint32_t g_ntoa_buffer = 0;
uint32_t inet_ntoa(uint32_t address) {
    if (g_ntoa_buffer == 0) g_ntoa_buffer = system_alloc(16);
    std::snprintf(guest<char>(g_ntoa_buffer), 16, "%u.%u.%u.%u", address >> 24, (address >> 16) & 0xFF,
                  (address >> 8) & 0xFF, address & 0xFF);
    return g_ntoa_buffer;
}

// AF_INET only; returns 1 on success like POSIX.
int32_t inet_pton(int32_t family, uint32_t text, uint8_t* out) {
    if (family != 2 || out == nullptr) return -1;
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(guest<char>(text), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 ||
        b > 255 || c > 255 || d > 255) {
        return 0;
    }
    out[0] = static_cast<uint8_t>(a);
    out[1] = static_cast<uint8_t>(b);
    out[2] = static_cast<uint8_t>(c);
    out[3] = static_cast<uint8_t>(d);
    return 1;
}

// NSSL: initialises; contexts cannot be created without a network.
constexpr int32_t kSslError = -0x10000;
int32_t NSSLInit() {
    trace_net(__func__);
    return 0;
}
int32_t NSSLFinish() {
    trace_net(__func__);
    return 0;
}
int32_t NSSLCreateContext(int32_t) {
    trace_net(__func__);
    return kSslError;
}
int32_t NSSLDestroyContext(int32_t) {
    trace_net(__func__);
    return 0;
}
int32_t NSSLAddServerPKIGroups(int32_t, uint32_t, uint32_t, uint32_t) {
    trace_net(__func__);
    return kSslError;
}

// ----------------------------------------------------------------- nlibcurl
constexpr int32_t kCurlOk = 0;
constexpr int32_t kCurlCouldntResolveHost = 6;
constexpr int32_t kCurlMsgDone = 1;

std::mutex g_curl_mutex;
std::map<uint32_t, std::vector<uint32_t>> g_multi_handles; // multi -> easy handles
std::map<uint32_t, std::deque<uint32_t>> g_multi_done;     // multi -> finished, unreported
uint32_t g_curl_message = 0;

uint32_t new_handle() { return system_alloc(16); }

int32_t curl_global_init_mem(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    trace_net(__func__);
    return kCurlOk;
}
void curl_global_cleanup() {}
uint32_t curl_easy_init() {
    trace_net(__func__);
    return new_handle();
}
void curl_easy_cleanup(uint32_t handle) {
    trace_net(__func__);
    if (handle) system_free(handle);
}
void curl_easy_reset(uint32_t) {}
int32_t curl_easy_pause(uint32_t, int32_t) {
    trace_net(__func__);
    return kCurlOk;
}
int32_t curl_easy_setopt(uint32_t, int32_t, uint32_t) {
    trace_net(__func__);
    return kCurlOk;
}
int32_t curl_easy_getinfo(uint32_t, int32_t, be<uint32_t>* out) {
    trace_net(__func__);
    if (out) *out = 0;
    return kCurlOk;
}
uint32_t g_curl_error_text = 0;
uint32_t curl_easy_strerror(int32_t) {
    trace_net(__func__);
    if (g_curl_error_text == 0) {
        static const char kText[] = "Couldn't resolve host name";
        g_curl_error_text = system_alloc(sizeof kText);
        std::memcpy(guest<char>(g_curl_error_text), kText, sizeof kText);
    }
    return g_curl_error_text;
}

uint32_t curl_multi_init() {
    trace_net(__func__);
    return new_handle();
}
int32_t curl_multi_cleanup(uint32_t multi) {
    trace_net(__func__);
    std::lock_guard lock(g_curl_mutex);
    g_multi_handles.erase(multi);
    g_multi_done.erase(multi);
    if (multi) system_free(multi);
    return kCurlOk;
}
int32_t curl_multi_add_handle(uint32_t multi, uint32_t easy) {
    trace_net(__func__);
    std::lock_guard lock(g_curl_mutex);
    g_multi_handles[multi].push_back(easy);
    g_multi_done[multi].push_back(easy);
    return kCurlOk;
}
int32_t curl_multi_remove_handle(uint32_t multi, uint32_t easy) {
    trace_net(__func__);
    std::lock_guard lock(g_curl_mutex);
    auto& handles = g_multi_handles[multi];
    handles.erase(std::remove(handles.begin(), handles.end(), easy), handles.end());
    auto& done = g_multi_done[multi];
    done.erase(std::remove(done.begin(), done.end(), easy), done.end());
    return kCurlOk;
}
// Every transfer is already over.
int32_t curl_multi_perform(uint32_t, be<int32_t>* running) {
    trace_net(__func__);
    if (running) *running = 0;
    return kCurlOk;
}
int32_t curl_multi_fdset(uint32_t, uint32_t, uint32_t, uint32_t, be<int32_t>* max_fd) {
    trace_net(__func__);
    if (max_fd) *max_fd = -1;
    return kCurlOk;
}
int32_t curl_multi_timeout(uint32_t, be<int32_t>* timeout_ms) {
    trace_net(__func__);
    if (timeout_ms) *timeout_ms = 0;
    return kCurlOk;
}
// CURLMsg: msg, easy handle, result.
uint32_t curl_multi_info_read(uint32_t multi, be<int32_t>* remaining) {
    trace_net(__func__);
    std::lock_guard lock(g_curl_mutex);
    auto& done = g_multi_done[multi];
    if (done.empty()) {
        if (remaining) *remaining = 0;
        return 0;
    }
    if (g_curl_message == 0) g_curl_message = system_alloc(12);
    *guest<be<int32_t>>(g_curl_message) = kCurlMsgDone;
    *guest<be<uint32_t>>(g_curl_message + 4) = done.front();
    *guest<be<int32_t>>(g_curl_message + 8) = kCurlCouldntResolveHost;
    done.pop_front();
    if (remaining) *remaining = static_cast<int32_t>(done.size());
    return g_curl_message;
}

uint32_t curl_share_init() {
    trace_net(__func__);
    return new_handle();
}
int32_t curl_share_setopt(uint32_t, int32_t, uint32_t) {
    trace_net(__func__);
    return kCurlOk;
}
int32_t curl_share_cleanup(uint32_t share) {
    trace_net(__func__);
    if (share) system_free(share);
    return kCurlOk;
}

// curl_slist { char* data; curl_slist* next; } in guest memory.
uint32_t curl_slist_append(uint32_t list, uint32_t text) {
    trace_net(__func__);
    const std::string_view s = guest_string(text);
    const uint32_t copy = system_alloc(static_cast<uint32_t>(s.size() + 1));
    std::memcpy(guest<char>(copy), s.data(), s.size());
    const uint32_t node = system_alloc(8);
    *guest<be<uint32_t>>(node) = copy;
    *guest<be<uint32_t>>(node + 4) = 0u;
    if (list == 0) return node;
    uint32_t last = list;
    while (const uint32_t next = *guest<be<uint32_t>>(last + 4)) last = next;
    *guest<be<uint32_t>>(last + 4) = node;
    return list;
}
void curl_slist_free_all(uint32_t list) {
    trace_net(__func__);
    while (list != 0) {
        const uint32_t next = *guest<be<uint32_t>>(list + 4);
        system_free(*guest<be<uint32_t>>(list));
        system_free(list);
        list = next;
    }
}

void set_resolver_allocator(uint32_t, uint32_t) {}
int32_t curl_easy_perform(uint32_t) {
    trace_net(__func__);
    return kCurlCouldntResolveHost;
}
void curl_formfree(uint32_t) {}

} // namespace

CAFE_EXPORT(nsysnet, set_resolver_allocator, set_resolver_allocator);
CAFE_EXPORT(nsysnet, htonl, byte_order32);
CAFE_EXPORT(nsysnet, ntohl, byte_order32);
CAFE_EXPORT(nsysnet, htons, byte_order16);
CAFE_EXPORT(nsysnet, ntohs, byte_order16);
CAFE_EXPORT(nsysnet, inet_ntoa, inet_ntoa);
CAFE_EXPORT(nsysnet, inet_pton, inet_pton);
CAFE_EXPORT(nsysnet, NSSLInit, NSSLInit);
CAFE_EXPORT(nsysnet, NSSLFinish, NSSLFinish);
CAFE_EXPORT(nsysnet, NSSLCreateContext, NSSLCreateContext);
CAFE_EXPORT(nsysnet, NSSLDestroyContext, NSSLDestroyContext);
CAFE_EXPORT(nsysnet, NSSLAddServerPKIGroups, NSSLAddServerPKIGroups);

CAFE_EXPORT(nlibcurl, curl_global_init_mem, curl_global_init_mem);
CAFE_EXPORT(nlibcurl, curl_global_cleanup, curl_global_cleanup);
CAFE_EXPORT(nlibcurl, curl_easy_init, curl_easy_init);
CAFE_EXPORT(nlibcurl, mw_curl_easy_init, curl_easy_init);
CAFE_EXPORT(nlibcurl, curl_easy_perform, curl_easy_perform);
CAFE_EXPORT(nlibcurl, curl_formfree, curl_formfree);
CAFE_EXPORT(nlibcurl, curl_easy_cleanup, curl_easy_cleanup);
CAFE_EXPORT(nlibcurl, curl_easy_reset, curl_easy_reset);
CAFE_EXPORT(nlibcurl, curl_easy_pause, curl_easy_pause);
CAFE_EXPORT(nlibcurl, curl_easy_setopt, curl_easy_setopt);
CAFE_EXPORT(nlibcurl, curl_easy_getinfo, curl_easy_getinfo);
CAFE_EXPORT(nlibcurl, curl_easy_strerror, curl_easy_strerror);
CAFE_EXPORT(nlibcurl, curl_multi_init, curl_multi_init);
CAFE_EXPORT(nlibcurl, curl_multi_cleanup, curl_multi_cleanup);
CAFE_EXPORT(nlibcurl, curl_multi_add_handle, curl_multi_add_handle);
CAFE_EXPORT(nlibcurl, curl_multi_remove_handle, curl_multi_remove_handle);
CAFE_EXPORT(nlibcurl, curl_multi_perform, curl_multi_perform);
CAFE_EXPORT(nlibcurl, curl_multi_fdset, curl_multi_fdset);
CAFE_EXPORT(nlibcurl, curl_multi_timeout, curl_multi_timeout);
CAFE_EXPORT(nlibcurl, curl_multi_info_read, curl_multi_info_read);
CAFE_EXPORT(nlibcurl, curl_share_init, curl_share_init);
CAFE_EXPORT(nlibcurl, curl_share_setopt, curl_share_setopt);
CAFE_EXPORT(nlibcurl, curl_share_cleanup, curl_share_cleanup);
CAFE_EXPORT(nlibcurl, curl_slist_append, curl_slist_append);
CAFE_EXPORT(nlibcurl, curl_slist_free_all, curl_slist_free_all);

} // namespace cafe::os
