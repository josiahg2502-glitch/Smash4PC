// nsysnet's BSD sockets on the host's: IPv4 TCP and UDP. Socket handles are
// 1 to 32 (select's fd_set is a 32-bit mask of them); errors are the Cafe
// codes, in socketlasterr() and the thread's errno. Every guest thread is a
// host thread, so blocking calls block on the host socket itself.
// After Cemu's nsysnet (MPL 2.0, src/Cafe/OS/libs/nsysnet/nsysnet.cpp).

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/sysmem.h"

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using HostSocket = SOCKET;
using socklen_t = int;
#define CLOSE_SOCKET closesocket
#define LAST_ERROR WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using HostSocket = int;
#define INVALID_SOCKET (-1)
#define CLOSE_SOCKET close
#define LAST_ERROR errno
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace cafe::os {
namespace {

// TTT2_TRACE_NET=1: each network-service call the title makes (connection, account, time, sockets, curl).
inline void trace_net(const char* what) {
    static const bool on = std::getenv("TTT2_TRACE_NET") != nullptr;
    if (on) std::fprintf(stderr, "net-call: %s\n", what);
}

// Cafe constants.
constexpr int32_t kAfInet = 2;
constexpr int32_t kSockStream = 1, kSockDgram = 2;
constexpr int32_t kProtoIp = 0, kProtoTcp = 6, kProtoUdp = 17;
constexpr int32_t kSolSocket = -1;
constexpr int32_t kSoReuseAddr = 0x0004, kSoKeepAlive = 0x0008, kSoBroadcast = 0x0020;
constexpr int32_t kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002, kSoLastError = 0x1007, kSoType = 0x1008;
constexpr int32_t kSoNbio = 0x1014, kSoBio = 0x1015, kSoNonBlock = 0x1016;
constexpr int32_t kTcpNoDelay = 0x2004;
constexpr int32_t kMsgPeek = 0x02, kMsgDontWait = 0x20;

// Cafe error codes.
constexpr int32_t kOk = 0, kENoBufs = 1, kETimedOut = 2, kEIsConn = 3, kEConnAborted = 5, kEWouldBlock = 6,
                  kEConnRefused = 7, kEConnReset = 8, kENotConn = 9, kEAlready = 10, kEInval = 11, kEMsgSize = 12,
                  kEShutdown = 15, kEAddrNotAvail = 19, kEAddrInUse = 20, kEAfNoSupport = 21, kEInProgress = 22,
                  kENotSock = 24, kENetUnreach = 30, kELibNotReady = 43, kEMFile = 51;

thread_local int32_t t_error = 0;

void set_error(int32_t error) {
    t_error = error;
    if (Thread* t = current_thread()) field<int32_t>(t->guest, osthread::kErrno) = error;
}

// The host's last socket error as a Cafe one (`connecting`: a connect() in progress).
int32_t cafe_error(bool connecting = false) {
    const int e = LAST_ERROR;
#ifdef _WIN32
    switch (e) {
    case WSAEWOULDBLOCK: return connecting ? kEInProgress : kEWouldBlock;
    case WSAEINPROGRESS: return kEInProgress;
    case WSAEALREADY: return kEAlready;
    case WSAEISCONN: return kEIsConn;
    case WSAECONNABORTED: return kEConnAborted;
    case WSAECONNREFUSED: return kEConnRefused;
    case WSAECONNRESET: return kEConnReset;
    case WSAENOTCONN: return kENotConn;
    case WSAESHUTDOWN: return kEShutdown;
    case WSAEINVAL: return kEInval;
    case WSAEMSGSIZE: return kEMsgSize;
    case WSAETIMEDOUT: return kETimedOut;
    case WSAEADDRINUSE: return kEAddrInUse;
    case WSAEADDRNOTAVAIL: return kEAddrNotAvail;
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH: return kENetUnreach;
    case WSAENOBUFS: return kENoBufs;
    case WSAENOTSOCK: return kENotSock;
    default: return kEInval;
    }
#else
    switch (e) {
    case EWOULDBLOCK: return connecting ? kEInProgress : kEWouldBlock;
    case EINPROGRESS: return kEInProgress;
    case EALREADY: return kEAlready;
    case EISCONN: return kEIsConn;
    case ECONNABORTED: return kEConnAborted;
    case ECONNREFUSED: return kEConnRefused;
    case ECONNRESET: return kEConnReset;
    case ENOTCONN: return kENotConn;
    case ESHUTDOWN: return kEShutdown;
    case EINVAL: return kEInval;
    case EMSGSIZE: return kEMsgSize;
    case ETIMEDOUT: return kETimedOut;
    case EADDRINUSE: return kEAddrInUse;
    case EADDRNOTAVAIL: return kEAddrNotAvail;
    case ENETUNREACH:
    case EHOSTUNREACH: return kENetUnreach;
    case ENOBUFS: return kENoBufs;
    case ENOTSOCK: return kENotSock;
    default: return kEInval;
    }
#endif
}

int32_t fail(int32_t error) {
    set_error(error);
    return -1;
}

struct Socket {
    HostSocket s = INVALID_SOCKET;
    int32_t type = 0;
    bool non_blocking = false;
    bool shut_recv = false;
};

constexpr int kMaxSockets = 32;
std::mutex g_mutex;
std::array<Socket, kMaxSockets + 1> g_sockets{}; // [1..32]
bool g_ready = false;

Socket* lookup(int32_t handle) {
    std::lock_guard lock(g_mutex);
    if (handle < 1 || handle > kMaxSockets || g_sockets[handle].s == INVALID_SOCKET) return nullptr;
    return &g_sockets[handle];
}

void set_host_non_blocking(HostSocket s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}

// The Cafe sockaddr is BSD's with a big-endian 16-bit family; port and address are network order already.
sockaddr to_host(const uint8_t* guest_addr) {
    sockaddr a{};
    a.sa_family = static_cast<decltype(a.sa_family)>(guest_addr[0] << 8 | guest_addr[1]);
    std::memcpy(a.sa_data, guest_addr + 2, 14);
    return a;
}
void to_guest(const sockaddr& a, uint8_t* guest_addr) {
    guest_addr[0] = static_cast<uint8_t>(a.sa_family >> 8);
    guest_addr[1] = static_cast<uint8_t>(a.sa_family);
    std::memcpy(guest_addr + 2, a.sa_data, 14);
}

// Waits until `s` can be read (or written); false if `non_blocking` and it cannot now.
bool ready(HostSocket s, bool write, bool non_blocking) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval zero{0, 0};
    const int r = select(static_cast<int>(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr,
                         non_blocking ? &zero : nullptr);
    return r > 0;
}

// --------------------------------------------------------------------- calls
int32_t socket_lib_init() {
    trace_net(__func__);
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    g_ready = true;
    return 0;
}
int32_t socket_lib_finish() {
    trace_net(__func__);
    g_ready = false;
    return 0;
}
int32_t socketlasterr() { return t_error; }

int32_t socket_create(int32_t family, int32_t type, int32_t protocol) {
    trace_net(__func__);
    if (!g_ready) return fail(kELibNotReady);
    if (family != kAfInet) return fail(kEAfNoSupport);
    if ((type != kSockStream && type != kSockDgram) ||
        (protocol != kProtoIp && protocol != kProtoTcp && protocol != kProtoUdp)) {
        return fail(kEInval);
    }
    const HostSocket s = ::socket(AF_INET, type == kSockStream ? SOCK_STREAM : SOCK_DGRAM,
                                  protocol == kProtoTcp ? IPPROTO_TCP : protocol == kProtoUdp ? IPPROTO_UDP : 0);
    if (s == INVALID_SOCKET) return fail(cafe_error());
#ifdef _WIN32
    if (type == kSockDgram) {
        // No WSAECONNRESET on a UDP socket after a peer's port was unreachable (the console has none).
        BOOL report = FALSE;
        DWORD bytes = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &bytes, nullptr, nullptr);
    }
#endif
    std::lock_guard lock(g_mutex);
    for (int h = 1; h <= kMaxSockets; ++h) {
        if (g_sockets[h].s == INVALID_SOCKET) {
            g_sockets[h] = {s, type, false, false};
            set_error(kOk);
            return h;
        }
    }
    CLOSE_SOCKET(s);
    return fail(kEMFile);
}

int32_t socketclose(int32_t handle) {
    std::lock_guard lock(g_mutex);
    if (handle < 1 || handle > kMaxSockets || g_sockets[handle].s == INVALID_SOCKET) return fail(kENotSock);
    CLOSE_SOCKET(g_sockets[handle].s);
    g_sockets[handle] = {};
    set_error(kOk);
    return 0;
}

int32_t socket_shutdown(int32_t handle, int32_t how) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so) return fail(kENotSock);
    if (how == 0 || how == 2) so->shut_recv = true;
    if (::shutdown(so->s, how) != 0) return fail(cafe_error());
    set_error(kOk);
    return 0;
}

int32_t socket_bind(int32_t handle, const uint8_t* addr, int32_t) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so || !addr) return fail(so ? kEInval : kENotSock);
    const sockaddr a = to_host(addr);
    if (::bind(so->s, &a, sizeof a) != 0) return fail(cafe_error());
    set_error(kOk);
    return 0;
}

int32_t socket_connect(int32_t handle, const uint8_t* addr, int32_t) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so || !addr) return fail(so ? kEInval : kENotSock);
    const sockaddr a = to_host(addr);
    if (::connect(so->s, &a, sizeof a) != 0) return fail(cafe_error(true));
    set_error(kOk);
    return 0;
}

int32_t socket_getsockname(int32_t handle, uint8_t* addr, be<int32_t>* length) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so || !addr) return fail(so ? kEInval : kENotSock);
    sockaddr a{};
    socklen_t len = sizeof a;
    if (::getsockname(so->s, &a, &len) != 0) return fail(cafe_error());
    to_guest(a, addr);
    if (length) *length = 16;
    set_error(kOk);
    return 0;
}

int32_t socket_send(int32_t handle, const char* data, int32_t length, int32_t flags) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so) return fail(kENotSock);
    if ((flags & kMsgDontWait) && !so->non_blocking && !ready(so->s, true, true)) return fail(kEWouldBlock);
    const int r = ::send(so->s, data, length, 0);
    if (r < 0) return fail(cafe_error());
    set_error(kOk);
    return r;
}

int32_t socket_recv(int32_t handle, char* data, int32_t length, int32_t flags) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so) return fail(kENotSock);
    if (so->shut_recv) return fail(kEShutdown);
    if ((flags & kMsgDontWait) && !so->non_blocking && !ready(so->s, false, true)) return fail(kEWouldBlock);
    const int r = ::recv(so->s, data, length, (flags & kMsgPeek) ? MSG_PEEK : 0);
    if (r < 0) return fail(cafe_error());
    set_error(kOk);
    return r;
}

// The first datagram to or from each address, once (the game server, then the players matchmaking finds).
void note_peer(const char* direction, const sockaddr& a) {
    static std::mutex mutex;
    static std::vector<uint64_t> seen;
    const auto& in = reinterpret_cast<const sockaddr_in&>(a);
    const uint64_t key = uint64_t(in.sin_addr.s_addr) << 16 | in.sin_port | (direction[0] == 'f' ? 1ull << 48 : 0);
    std::lock_guard lock(mutex);
    if (seen.size() > 64 || std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    const auto* ip = reinterpret_cast<const uint8_t*>(&in.sin_addr);
    std::fprintf(stderr, "ttt2: net: first datagram %s %u.%u.%u.%u:%u\n", direction, ip[0], ip[1], ip[2], ip[3],
                 ntohs(in.sin_port));
}

int32_t socket_sendto(int32_t handle, const char* data, int32_t length, int32_t flags, const uint8_t* to, int32_t) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so || !to) return fail(so ? kEInval : kENotSock);
    if ((flags & kMsgDontWait) && !so->non_blocking && !ready(so->s, true, true)) return fail(kEWouldBlock);
    const sockaddr a = to_host(to);
    const int r = ::sendto(so->s, data, length, 0, &a, sizeof a);
    if (r < 0) return fail(cafe_error());
    note_peer("to", a);
    set_error(kOk);
    return r;
}

int32_t socket_recvfrom(int32_t handle, char* data, int32_t length, int32_t flags, uint8_t* from,
                        be<int32_t>* from_length) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so) return fail(kENotSock);
    if (so->shut_recv) return fail(kEShutdown);
    if ((flags & kMsgDontWait) && !so->non_blocking && !ready(so->s, false, true)) return fail(kEWouldBlock);
    sockaddr a{};
    socklen_t len = sizeof a;
    const int r = ::recvfrom(so->s, data, length, (flags & kMsgPeek) ? MSG_PEEK : 0, &a, &len);
    if (r < 0) return fail(cafe_error());
    note_peer("from", a);
    if (from && from_length) {
        to_guest(a, from);
        *from_length = 16;
    }
    set_error(kOk);
    return r;
}

// The same datagram to several addresses (Cafe sockaddrs, 16 bytes each).
int32_t socket_sendto_multi(int32_t handle, const char* data, int32_t length, uint32_t, const uint8_t* to,
                            uint32_t count) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so || !to) return fail(so ? kEInval : kENotSock);
    for (uint32_t i = 0; i < count; ++i) {
        const sockaddr a = to_host(to + 16 * i);
        if (::sendto(so->s, data, length, 0, &a, sizeof a) < 0) return fail(cafe_error());
    }
    set_error(kOk);
    return length;
}

int32_t socket_setsockopt(int32_t handle, int32_t level, int32_t name, const uint8_t* value, int32_t length) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so) return fail(kENotSock);
    const int32_t v = value && length >= 4 ? int32_t(value[0] << 24 | value[1] << 16 | value[2] << 8 | value[3]) : 0;
    const auto host_int = [&](int host_level, int host_name) {
        const int x = v;
        return ::setsockopt(so->s, host_level, host_name, reinterpret_cast<const char*>(&x), sizeof x);
    };
    int r = 0;
    if (level == kSolSocket) {
        switch (name) {
        case kSoReuseAddr: r = host_int(SOL_SOCKET, SO_REUSEADDR); break;
        case kSoBroadcast: r = host_int(SOL_SOCKET, SO_BROADCAST); break;
        case kSoKeepAlive: r = host_int(SOL_SOCKET, SO_KEEPALIVE); break;
        case kSoRcvBuf: r = host_int(SOL_SOCKET, SO_RCVBUF); break;
        case kSoSndBuf: r = host_int(SOL_SOCKET, SO_SNDBUF); break;
        case kSoNbio:
        case kSoBio:
        case kSoNonBlock:
            so->non_blocking = name == kSoNbio || (name == kSoNonBlock && v != 0);
            set_host_non_blocking(so->s, so->non_blocking);
            break;
        default: break; // the console's own (window scaling, user buffers): nothing to do on the host
        }
    } else if (level == kProtoTcp && name == kTcpNoDelay) {
        r = host_int(IPPROTO_TCP, TCP_NODELAY);
    } // IP options (multicast, TTL, TOS): left at the host's defaults
    if (r != 0) return fail(cafe_error());
    set_error(kOk);
    return 0;
}

int32_t socket_getsockopt(int32_t handle, int32_t level, int32_t name, uint8_t* value, be<int32_t>* length) {
    trace_net(__func__);
    Socket* so = lookup(handle);
    if (!so) return fail(kENotSock);
    int32_t out = 0;
    if (level == kSolSocket && (name == kSoLastError || name == kSoRcvBuf || name == kSoSndBuf)) {
        int x = 0;
        socklen_t len = sizeof x;
        ::getsockopt(so->s, SOL_SOCKET, name == kSoLastError ? SO_ERROR : name == kSoRcvBuf ? SO_RCVBUF : SO_SNDBUF,
                     reinterpret_cast<char*>(&x), &len);
        out = name == kSoLastError && x != 0 ? kEConnRefused : x;
    } else if (level == kSolSocket && name == kSoType) {
        out = so->type;
    } else if (level == kSolSocket && name == kSoNonBlock) {
        out = so->non_blocking;
    }
    if (value) *reinterpret_cast<be<int32_t>*>(value) = out;
    if (length) *length = 4;
    set_error(kOk);
    return 0;
}

// fd_set: one bit per handle. Timeout: { u32 seconds, u32 microseconds }, big-endian.
int32_t socket_select(int32_t nfds, be<uint32_t>* readable, be<uint32_t>* writable, be<uint32_t>* failed,
                      be<uint32_t>* timeout) {
    trace_net(__func__);
    const uint32_t masks[3] = {readable ? uint32_t(*readable) : 0u, writable ? uint32_t(*writable) : 0u,
                               failed ? uint32_t(*failed) : 0u};
    // (Cemu's finding: a missing timeout returns at once.)
    if (timeout == nullptr) {
        if (readable) *readable = 0u;
        if (writable) *writable = 0u;
        if (failed) *failed = 0u;
        set_error(kOk);
        return 0;
    }
    timeval tv{static_cast<long>(uint32_t(timeout[0])), static_cast<long>(uint32_t(timeout[1]))};
    fd_set sets[3];
    int highest = -1;
    for (int k = 0; k < 3; ++k) {
        FD_ZERO(&sets[k]);
        for (int h = 1; h < std::min(nfds, kMaxSockets + 1); ++h) {
            if (!((masks[k] >> h) & 1)) continue;
            if (Socket* so = lookup(h)) {
                FD_SET(so->s, &sets[k]);
                highest = std::max(highest, static_cast<int>(so->s));
            }
        }
    }
    if (highest < 0) { // nothing to wait on: just the time
#ifdef _WIN32
        Sleep(tv.tv_sec * 1000 + tv.tv_usec / 1000);
#else
        select(0, nullptr, nullptr, nullptr, &tv);
#endif
        if (readable) *readable = 0u;
        if (writable) *writable = 0u;
        if (failed) *failed = 0u;
        set_error(kOk);
        return 0;
    }
    const int r = ::select(highest + 1, readable ? &sets[0] : nullptr, writable ? &sets[1] : nullptr,
                           failed ? &sets[2] : nullptr, &tv);
    if (r < 0) return fail(cafe_error());
    be<uint32_t>* outs[3] = {readable, writable, failed};
    int count = 0;
    for (int k = 0; k < 3; ++k) {
        if (!outs[k]) continue;
        uint32_t mask = 0;
        for (int h = 1; h <= kMaxSockets; ++h) {
            if (!((masks[k] >> h) & 1)) continue;
            Socket* so = lookup(h);
            if (so && FD_ISSET(so->s, &sets[k])) {
                mask |= 1u << h;
                ++count;
            }
        }
        *outs[k] = mask;
    }
    set_error(kOk);
    return count;
}

// addrinfo: { flags, family, socktype, protocol, addrlen, canonname*, addr*, next* }, 32 bytes.
int32_t socket_getaddrinfo(uint32_t node, uint32_t service, const be<int32_t>* hints, be<uint32_t>* result) {
    trace_net(__func__);
    if (result) *result = 0u;
    addrinfo hint{};
    hint.ai_family = AF_INET;
    if (hints) {
        hint.ai_flags = hints[0];
        hint.ai_socktype = hints[2] == kSockStream ? SOCK_STREAM : hints[2] == kSockDgram ? SOCK_DGRAM : 0;
        hint.ai_protocol = hints[3];
    }
    addrinfo* found = nullptr;
    const std::string host = node ? std::string(guest_string(node)) : std::string();
    const std::string port = service ? std::string(guest_string(service)) : std::string();
    if (::getaddrinfo(node ? host.c_str() : nullptr, service ? port.c_str() : nullptr, &hint, &found) != 0 || !found) {
        std::fprintf(stderr, "ttt2: net: %s does not resolve\n", host.c_str());
        return 8; // EAI_NONAME
    }
    uint32_t first = 0, previous = 0;
    for (addrinfo* a = found; a; a = a->ai_next) {
        if (a->ai_family != AF_INET) continue;
        const uint32_t entry = system_alloc(32 + 16);
        auto* e = guest<be<uint32_t>>(entry);
        e[0] = static_cast<uint32_t>(a->ai_flags);
        e[1] = static_cast<uint32_t>(kAfInet);
        e[2] = static_cast<uint32_t>(a->ai_socktype == SOCK_STREAM ? kSockStream : kSockDgram);
        e[3] = static_cast<uint32_t>(a->ai_protocol);
        e[4] = 16u;
        e[5] = 0u;
        e[6] = entry + 32;
        e[7] = 0u;
        to_guest(*a->ai_addr, guest<uint8_t>(entry + 32));
        if (previous) guest<be<uint32_t>>(previous)[7] = entry;
        else first = entry;
        previous = entry;
    }
    ::freeaddrinfo(found);
    if (result) *result = first;
    return first ? 0 : 8;
}

void socket_freeaddrinfo(uint32_t list) {
    trace_net(__func__);
    while (list) {
        const uint32_t next = guest<be<uint32_t>>(list)[7];
        system_free(list);
        list = next;
    }
}

// hostent: { name*, aliases*, addrtype, length, addr_list* }; kept in one static block, as on the console.
uint32_t socket_gethostbyname(uint32_t name) {
    trace_net(__func__);
    static uint32_t block = 0;
    if (block == 0) block = system_alloc(20 + 8 + 4 + 256);
    addrinfo hint{};
    hint.ai_family = AF_INET;
    addrinfo* found = nullptr;
    const std::string host(guest_string(name));
    if (::getaddrinfo(host.c_str(), nullptr, &hint, &found) != 0 || !found) return 0;
    auto* h = guest<be<uint32_t>>(block);
    const uint32_t list = block + 20, address = block + 28, text = block + 32;
    std::snprintf(guest<char>(text), 256, "%s", host.c_str());
    h[0] = text;
    h[1] = 0u;
    h[2] = static_cast<uint32_t>(kAfInet);
    h[3] = 4u;
    h[4] = list;
    guest<be<uint32_t>>(list)[0] = address;
    guest<be<uint32_t>>(list)[1] = 0u;
    std::memcpy(guest<uint8_t>(address), &reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr, 4);
    ::freeaddrinfo(found);
    return block;
}

} // namespace

CAFE_EXPORT(nsysnet, socket_lib_init, socket_lib_init);
CAFE_EXPORT(nsysnet, socket_lib_finish, socket_lib_finish);
CAFE_EXPORT(nsysnet, socketlasterr, socketlasterr);
CAFE_EXPORT(nsysnet, socket, socket_create);
CAFE_EXPORT(nsysnet, mw_socket, socket_create);
CAFE_EXPORT(nsysnet, socketclose, socketclose);
CAFE_EXPORT(nsysnet, shutdown, socket_shutdown);
CAFE_EXPORT(nsysnet, bind, socket_bind);
CAFE_EXPORT(nsysnet, connect, socket_connect);
CAFE_EXPORT(nsysnet, getsockname, socket_getsockname);
CAFE_EXPORT(nsysnet, send, socket_send);
CAFE_EXPORT(nsysnet, recv, socket_recv);
CAFE_EXPORT(nsysnet, sendto, socket_sendto);
CAFE_EXPORT(nsysnet, recvfrom, socket_recvfrom);
CAFE_EXPORT(nsysnet, sendto_multi, socket_sendto_multi);
CAFE_EXPORT(nsysnet, setsockopt, socket_setsockopt);
CAFE_EXPORT(nsysnet, getsockopt, socket_getsockopt);
CAFE_EXPORT(nsysnet, select, socket_select);
CAFE_EXPORT(nsysnet, getaddrinfo, socket_getaddrinfo);
CAFE_EXPORT(nsysnet, freeaddrinfo, socket_freeaddrinfo);
CAFE_EXPORT(nsysnet, gethostbyname, socket_gethostbyname);

} // namespace cafe::os
