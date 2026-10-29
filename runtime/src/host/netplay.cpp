#include "host/netplay.h"

#include "host/audio.h"
#include "host/keymap.h"

#include <SDL3/SDL.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace cafe::host {
os::ProPadState pad_as_pro(SDL_Gamepad* pad); // host/window.cpp
}

namespace cafe::host::netplay {
namespace {

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------ the protocol
// UDP datagrams, a type byte first; numbers little-endian.
constexpr uint32_t kMagic = 0x504E3453; // "S4NP"
constexpr uint16_t kVersion = 2;
enum Type : uint8_t {
    kPunch = 1,      // either: opens the routers' way (ignored)
    kHello = 2,      // guest, until welcomed: magic, version, name (32 bytes)
    kWelcome = 3,    // host: channel (1 byte), picture width and height (2 bytes each)
    kFull = 4,       // host: no free controller
    kInput = 5,      // guest, every frame: sequence (4), buttons (os::pro bits, 4), sticks (4 x int16, up positive)
    kVideo = 6,      // host: frame (4), piece (2), pieces (2), keyframe (1), then that piece of the H.264 frame
    kAudio = 7,      // host: 48 kHz stereo int16 frames
    kKeyRequest = 8, // guest: a frame was lost, send a keyframe
    kBye = 9,        // either: leaving
};
constexpr size_t kPiece = 1150;                  // H.264 bytes a video datagram (under common MTUs)
constexpr auto kSilence = std::chrono::seconds(5); // nothing heard for this long: gone

uint32_t le32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void start_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

sockaddr_in address_of(Endpoint e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(e.ip);
    a.sin_port = htons(e.port);
    return a;
}

Endpoint endpoint_of(const sockaddr_in& a) { return {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)}; }

// The game's UDP socket (one a process), with STUN for its public address.
class Socket {
public:
    bool open(uint16_t port) {
        start_winsock();
        s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            a.sin_port = 0; // taken: any port
            if (bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        }
        int size = 8 << 20;
        setsockopt(s_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof size);
        setsockopt(s_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof size);
        // (Windows reports an ICMP "port unreachable" as an error on the next receive: not a reason to stop.)
        BOOL off = FALSE;
        DWORD ignored = 0;
        WSAIoctl(s_, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0, &ignored, nullptr, nullptr);
        DWORD timeout = 200;
        setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        socklen_t length = sizeof a;
        getsockname(s_, reinterpret_cast<sockaddr*>(&a), &length);
        lan_ = {local_ip(), ntohs(a.sin_port)};
        return true;
    }

    void send(Endpoint to, const void* data, size_t size) const {
        const sockaddr_in a = address_of(to);
        sendto(s_, static_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    }
    void send(Endpoint to, Type type, const void* payload = nullptr, size_t size = 0) const {
        uint8_t p[1500];
        p[0] = type;
        if (size) std::memcpy(p + 1, payload, std::min(size, sizeof p - 1));
        send(to, p, 1 + std::min(size, sizeof p - 1));
    }

    // A datagram (STUN's answers are taken here); 0 when none came within 200 ms.
    size_t receive(uint8_t* data, size_t capacity, Endpoint& from) {
        sockaddr_in a{};
        socklen_t length = sizeof a;
        const int n = recvfrom(s_, reinterpret_cast<char*>(data), static_cast<int>(capacity), 0, reinterpret_cast<sockaddr*>(&a), &length);
        if (n <= 0) return 0;
        from = endpoint_of(a);
        if (n >= 20 && data[0] == 0x01 && data[1] == 0x01 && le32(data + 4) == 0x42A41221) { // a STUN answer
            take_stun(data, static_cast<size_t>(n));
            return 0;
        }
        return static_cast<size_t>(n);
    }

    // Asks STUN (stun.l.google.com) for this socket's public address; the answer comes to receive().
    void ask_stun() {
        static Endpoint server = [] {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* found = nullptr;
            Endpoint e{};
            if (getaddrinfo("stun.l.google.com", "19302", &hints, &found) == 0 && found) {
                e = endpoint_of(*reinterpret_cast<sockaddr_in*>(found->ai_addr));
                freeaddrinfo(found);
            }
            return e;
        }();
        if (server.ip == 0) return;
        uint8_t request[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
        for (int i = 8; i < 20; ++i) request[i] = static_cast<uint8_t>(random_id());
        send(server, request, sizeof request);
    }

    Endpoint lan() const { return lan_; }
    Endpoint pub() const {
        std::lock_guard lock(mutex_);
        return pub_;
    }

private:
    void take_stun(const uint8_t* p, size_t n) {
        for (size_t at = 20; at + 4 <= n;) {
            const uint16_t type = static_cast<uint16_t>(p[at] << 8 | p[at + 1]);
            const uint16_t length = static_cast<uint16_t>(p[at + 2] << 8 | p[at + 3]);
            if (at + 4 + length > n) break;
            const uint8_t* v = p + at + 4;
            if ((type == 0x0020 || type == 0x0001) && length >= 8 && v[1] == 0x01) { // (XOR-)MAPPED-ADDRESS, IPv4
                uint16_t port = static_cast<uint16_t>(v[2] << 8 | v[3]);
                uint32_t ip = static_cast<uint32_t>(v[4] << 24 | v[5] << 16 | v[6] << 8 | v[7]);
                if (type == 0x0020) port ^= 0x2112, ip ^= 0x2112A442;
                std::lock_guard lock(mutex_);
                pub_ = {ip, port};
                return;
            }
            at += 4 + ((length + 3) & ~3u);
        }
    }

    static uint32_t local_ip() {
        char name[256] = {};
        if (gethostname(name, sizeof name) != 0) return 0;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* found = nullptr;
        if (getaddrinfo(name, nullptr, &hints, &found) != 0) return 0;
        uint32_t ip = 0;
        for (addrinfo* a = found; a && ip == 0; a = a->ai_next) {
            const uint32_t v = ntohl(reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
            if ((v >> 24) != 127) ip = v;
        }
        freeaddrinfo(found);
        return ip;
    }

    SOCKET s_ = INVALID_SOCKET;
    Endpoint lan_;
    mutable std::mutex mutex_;
    Endpoint pub_;
};

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void start_media_foundation() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // (per thread)
}

void set_codec_value(ICodecAPI* api, const GUID& what, uint32_t value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&what, &v);
}

// The H.264 frames an MFT gives for its input so far (sync MFTs: Windows' own encoder and decoder).
// `on_change` handles a new output type (the decoder's first frame).
template <typename F>
HRESULT drain(IMFTransform* t, F&& got, const std::function<void()>& on_change = {}) {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        t->GetOutputStreamInfo(0, &info);
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;
        const bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        if (!provides) {
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1 << 20), &buffer);
            sample->AddBuffer(buffer);
        }
        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = sample;
        DWORD status = 0;
        const HRESULT hr = t->ProcessOutput(0, 1, &out, &status);
        release(out.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE && on_change) {
            release(buffer);
            release(out.pSample);
            on_change();
            continue;
        }
        if (SUCCEEDED(hr) && out.pSample) {
            IMFMediaBuffer* contiguous = nullptr;
            if (SUCCEEDED(out.pSample->ConvertToContiguousBuffer(&contiguous))) {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(contiguous->Lock(&data, nullptr, &length))) {
                    UINT32 key = 0;
                    out.pSample->GetUINT32(MFSampleExtension_CleanPoint, &key);
                    got(data, length, key != 0);
                    contiguous->Unlock();
                }
                release(contiguous);
            }
        }
        release(buffer);
        release(out.pSample);
        if (FAILED(hr)) return hr; // MF_E_TRANSFORM_NEED_MORE_INPUT: all out
    }
}

IMFSample* sample_of(const void* data, size_t size, int64_t time) {
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer))) return nullptr;
    BYTE* p = nullptr;
    buffer->Lock(&p, nullptr, nullptr);
    std::memcpy(p, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));
    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer);
    buffer->Release();
    sample->SetSampleTime(time);
    sample->SetSampleDuration(10'000'000 / 60);
    return sample;
}

IMFMediaType* video_type(const GUID& subtype, uint32_t width, uint32_t height) {
    IMFMediaType* type = nullptr;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return type;
}

// ------------------------------------------------------------------ host

// Windows' H.264 encoder, low-latency, baseline (no B-frames: each frame goes out as soon as it is coded).
// shortcut: software encoding (a few ms a 720p frame on its own thread); hardware MFTs (NVENC, AMF, Quick Sync)
// are asynchronous and need their event model, worth it for higher resolutions.
class Encoder {
public:
    bool open() {
        start_media_foundation();
        if (FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t_)))) return false;
        if (SUCCEEDED(t_->QueryInterface(IID_ICodecAPI, reinterpret_cast<void**>(&api_)))) {
            set_codec_value(api_, CODECAPI_AVLowLatencyMode, TRUE);
            set_codec_value(api_, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            set_codec_value(api_, CODECAPI_AVEncCommonMeanBitRate, 10'000'000);
            set_codec_value(api_, CODECAPI_AVEncMPVGOPSize, 120);
            set_codec_value(api_, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        }
        IMFMediaType* out = video_type(MFVideoFormat_H264, kStreamWidth, kStreamHeight);
        out->SetUINT32(MF_MT_AVG_BITRATE, 10'000'000);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        HRESULT hr = t_->SetOutputType(0, out, 0);
        out->Release();
        if (FAILED(hr)) return false;
        IMFMediaType* in = video_type(MFVideoFormat_NV12, kStreamWidth, kStreamHeight);
        hr = t_->SetInputType(0, in, 0);
        in->Release();
        if (FAILED(hr)) return false;
        t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    template <typename F>
    void encode(const std::vector<uint8_t>& nv12, int64_t time, bool key, F&& got) {
        if (key && api_) set_codec_value(api_, CODECAPI_AVEncVideoForceKeyFrame, 1);
        IMFSample* sample = sample_of(nv12.data(), nv12.size(), time);
        if (sample == nullptr) return;
        if (t_->ProcessInput(0, sample, 0) == MF_E_NOTACCEPTING) {
            drain(t_, got);
            t_->ProcessInput(0, sample, 0);
        }
        sample->Release();
        drain(t_, got);
    }

private:
    IMFTransform* t_ = nullptr;
    ICodecAPI* api_ = nullptr;
};

// RGBA rows to NV12, BT.709 limited range (the guest's texture says so).
void to_nv12(const uint8_t* rgba, bool bgra, std::vector<uint8_t>& out) {
    const uint32_t w = kStreamWidth, h = kStreamHeight;
    out.resize(size_t{w} * h * 3 / 2);
    uint8_t* y_plane = out.data();
    uint8_t* uv_plane = out.data() + size_t{w} * h;
    const int r_at = bgra ? 2 : 0, b_at = bgra ? 0 : 2;
    for (uint32_t y = 0; y < h; y += 2) {
        const uint8_t* row0 = rgba + size_t{y} * w * 4;
        const uint8_t* row1 = row0 + size_t{w} * 4;
        for (uint32_t x = 0; x < w; x += 2) {
            int rs = 0, gs = 0, bs = 0;
            for (int k = 0; k < 4; ++k) {
                const uint8_t* p = (k < 2 ? row0 : row1) + (x + (k & 1)) * 4;
                const int r = p[r_at], g = p[1], b = p[b_at];
                y_plane[size_t{y + (k >> 1)} * w + x + (k & 1)] = static_cast<uint8_t>(16 + ((47 * r + 157 * g + 16 * b) >> 8));
                rs += r, gs += g, bs += b;
            }
            uint8_t* uv = uv_plane + size_t{y / 2} * w + x;
            uv[0] = static_cast<uint8_t>(128 + ((-26 * rs - 87 * gs + 112 * bs) >> 10));
            uv[1] = static_cast<uint8_t>(128 + ((112 * rs - 102 * gs - 10 * bs) >> 10));
        }
    }
}

struct Guest {
    Endpoint address;
    int channel = 0;
    std::string name;
    bool need_key = true; // its video starts at a keyframe
    uint32_t input_sequence = 0;
    bool pressed = false; // (logged once)
    os::ProPadState pad{};
    Clock::time_point heard = Clock::now();
};

Socket g_socket;
std::mutex g_mutex; // the guests
std::array<std::optional<Guest>, os::pro::kChannels> g_guests;
std::atomic<int> g_guest_count{0};
std::atomic<bool> g_hosting{false}, g_force_key{false};
std::string g_room_name;

Guest* guest_at(Endpoint e) {
    for (auto& g : g_guests) {
        if (g && g->address == e) return &*g;
    }
    return nullptr;
}

void drop(Guest& g, const char* why) {
    std::fprintf(stderr, "ttt2: netplay: %s (player %d) %s\n", g.name.c_str(), g.channel + 2, why);
    g_guests[g.channel].reset();
    --g_guest_count;
}

// One H.264 frame to every guest that can take it, in pieces.
void send_video(const uint8_t* data, size_t size, bool key) {
    static uint32_t frame = 0;
    ++frame;
    std::vector<Endpoint> to;
    {
        std::lock_guard lock(g_mutex);
        for (auto& g : g_guests) {
            if (!g || (g->need_key && !key)) continue;
            g->need_key = false;
            to.push_back(g->address);
        }
    }
    const auto pieces = static_cast<uint16_t>((size + kPiece - 1) / kPiece);
    uint8_t p[10 + kPiece];
    p[0] = kVideo;
    std::memcpy(p + 1, &frame, 4);
    std::memcpy(p + 7, &pieces, 2);
    p[9] = key;
    for (uint16_t i = 0; i < pieces; ++i) {
        const size_t n = std::min(kPiece, size - size_t{i} * kPiece);
        std::memcpy(p + 5, &i, 2);
        std::memcpy(p + 10, data + size_t{i} * kPiece, n);
        for (const Endpoint& e : to) g_socket.send(e, p, 10 + n);
    }
}

void send_all_guests(Type type, const void* data, size_t size) {
    std::vector<Endpoint> to;
    {
        std::lock_guard lock(g_mutex);
        for (auto& g : g_guests) {
            if (g) to.push_back(g->address);
        }
    }
    for (const Endpoint& e : to) g_socket.send(e, type, data, size);
}

// The renderer's newest frame, waiting for the encoder (older ones are dropped).
struct PendingFrame {
    const uint8_t* pixels = nullptr;
    bool bgra = false;
    std::function<void()> wait, release;
};
std::mutex g_frame_mutex;
std::condition_variable g_frame_cv;
std::optional<PendingFrame> g_frame;

void encoder_main() {
    Encoder encoder;
    if (!encoder.open()) {
        std::fprintf(stderr, "ttt2: netplay: no H.264 encoder (Windows Media Foundation); guests get no picture\n");
        return;
    }
    std::vector<uint8_t> nv12;
    const auto start = Clock::now();
    for (;;) {
        PendingFrame f;
        {
            std::unique_lock lock(g_frame_mutex);
            g_frame_cv.wait(lock, [] { return g_frame.has_value(); });
            f = std::move(*g_frame);
            g_frame.reset();
        }
        f.wait();
        to_nv12(f.pixels, f.bgra, nv12);
        f.release();
        const int64_t time = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() / 100;
        encoder.encode(nv12, time, g_force_key.exchange(false),
                       [](const BYTE* data, DWORD size, bool key) { send_video(data, size, key); });
    }
}

// What the guests send.
void host_receive_main() {
    std::vector<uint8_t> p(2048);
    for (;;) {
        Endpoint from;
        const size_t n = g_socket.receive(p.data(), p.size(), from);
        const auto now = Clock::now();
        std::lock_guard lock(g_mutex);
        for (auto& g : g_guests) {
            if (g && now - g->heard > kSilence) drop(*g, "stopped answering");
        }
        if (n == 0) continue;
        Guest* g = guest_at(from);
        if (g) g->heard = now;
        switch (p[0]) {
        case kHello: {
            if (g) { // (the welcome was lost)
                const uint8_t w[5] = {static_cast<uint8_t>(g->channel), kStreamWidth & 0xFF, kStreamWidth >> 8,
                                      kStreamHeight & 0xFF, kStreamHeight >> 8};
                g_socket.send(from, kWelcome, w, sizeof w);
                break;
            }
            uint16_t version = 0;
            if (n < 39 || le32(p.data() + 1) != kMagic || (std::memcpy(&version, p.data() + 5, 2), version != kVersion)) break;
            const auto free = std::find_if(g_guests.begin(), g_guests.end(), [](const auto& x) { return !x; });
            if (free == g_guests.end()) {
                g_socket.send(from, kFull);
                break;
            }
            Guest& guest = free->emplace();
            guest.address = from;
            guest.channel = static_cast<int>(free - g_guests.begin());
            guest.name.assign(reinterpret_cast<const char*>(p.data() + 7), strnlen(reinterpret_cast<const char*>(p.data() + 7), 31));
            ++g_guest_count;
            g_force_key = true;
            const uint8_t w[5] = {static_cast<uint8_t>(guest.channel), kStreamWidth & 0xFF, kStreamWidth >> 8,
                                  kStreamHeight & 0xFF, kStreamHeight >> 8};
            g_socket.send(from, kWelcome, w, sizeof w);
            std::fprintf(stderr, "ttt2: netplay: %s joined as player %d\n", guest.name.c_str(), guest.channel + 2);
            break;
        }
        case kInput: {
            if (!g || n < 17) break;
            const uint32_t sequence = le32(p.data() + 1);
            if (sequence <= g->input_sequence) break; // (late)
            g->input_sequence = sequence;
            os::ProPadState s{true};
            s.buttons = le32(p.data() + 5);
            int16_t axes[4];
            std::memcpy(axes, p.data() + 9, 8);
            s.left_x = axes[0] / 32767.0f;
            s.left_y = axes[1] / 32767.0f;
            s.right_x = axes[2] / 32767.0f;
            s.right_y = axes[3] / 32767.0f;
            if (s.buttons && !g->pressed) {
                g->pressed = true;
                std::fprintf(stderr, "ttt2: netplay: player %d's controller works\n", g->channel + 2);
            }
            g->pad = s;
            break;
        }
        case kKeyRequest:
            if (g) g_force_key = true;
            break;
        case kBye:
            if (g) drop(*g, "left");
            break;
        default: break; // punches
        }
    }
}

// The room on the room server, every two seconds: listed, and the guests trying to reach it introduced.
void host_rooms_main() {
    const uint32_t id = random_id();
    for (int round = 0;; ++round) {
        if (g_socket.pub().ip == 0 || round % 15 == 0) g_socket.ask_stun(); // (and now and then: NATs forget)
        CheckIn c;
        c.id = id;
        c.pub = g_socket.pub();
        c.lan = g_socket.lan();
        c.room.name = g_room_name;
        c.room.players = 1 + g_guest_count;
        c.room.max = kMaxPlayers;
        const CheckInReply r = check_in(c);
        if (round == 0) {
            std::fprintf(stderr, r.ok ? "ttt2: netplay: room listed on %s\n" : "ttt2: netplay: room server %s not reached; "
                                                                               "guests can still join by address\n",
                         room_server().c_str());
        }
        for (const Introduction& peer : r.peers) { // open the way from this side too
            g_socket.send(peer.pub, kPunch);
            if (peer.lan.ip) g_socket.send(peer.lan, kPunch);
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// ------------------------------------------------------------------ guest

// Windows' H.264 decoder, to NV12.
class Decoder {
public:
    bool open(uint32_t width, uint32_t height) {
        start_media_foundation();
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t_)))) return false;
        IMFAttributes* attributes = nullptr;
        if (SUCCEEDED(t_->GetAttributes(&attributes))) {
            attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            attributes->Release();
        }
        IMFMediaType* in = video_type(MFVideoFormat_H264, width, height);
        const HRESULT hr = t_->SetInputType(0, in, 0);
        in->Release();
        if (FAILED(hr) || !choose_output()) return false;
        t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // `got(nv12, width, height)` for each picture the data completes.
    template <typename F>
    void decode(const uint8_t* data, size_t size, F&& got) {
        IMFSample* sample = sample_of(data, size, 0);
        if (sample == nullptr) return;
        t_->ProcessInput(0, sample, 0);
        sample->Release();
        drain(
            t_, [&](const BYTE* nv12, DWORD length, bool) {
                if (length >= size_t{width_} * height_ * 3 / 2) got(nv12, width_, height_);
            },
            [this] { choose_output(); });
    }

private:
    bool choose_output() {
        for (DWORD i = 0;; ++i) {
            IMFMediaType* type = nullptr;
            if (FAILED(t_->GetOutputAvailableType(0, i, &type))) return false;
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12) {
                const HRESULT hr = t_->SetOutputType(0, type, 0);
                MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width_, &height_);
                type->Release();
                return SUCCEEDED(hr);
            }
            type->Release();
        }
    }

    IMFTransform* t_ = nullptr;
    UINT32 width_ = 0, height_ = 0;
};

// The guest's controller: every pad, and the keyboard with the default keys.
os::ProPadState local_pad() {
    os::ProPadState s{true};
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) pad = SDL_OpenGamepad(ids[i]);
        if (pad == nullptr) continue;
        const os::ProPadState p = pad_as_pro(pad);
        s.buttons |= p.buttons;
        if (std::fabs(p.left_x) + std::fabs(p.left_y) > std::fabs(s.left_x) + std::fabs(s.left_y)) s.left_x = p.left_x, s.left_y = p.left_y;
        if (std::fabs(p.right_x) + std::fabs(p.right_y) > std::fabs(s.right_x) + std::fabs(s.right_y)) s.right_x = p.right_x, s.right_y = p.right_y;
    }
    SDL_free(ids);
    // The GamePad's buttons as a Pro Controller's.
    static constexpr std::pair<uint32_t, uint32_t> kAsPro[] = {
        {os::vpad::kA, os::pro::kA}, {os::vpad::kB, os::pro::kB}, {os::vpad::kX, os::pro::kX}, {os::vpad::kY, os::pro::kY},
        {os::vpad::kUp, os::pro::kUp}, {os::vpad::kDown, os::pro::kDown}, {os::vpad::kLeft, os::pro::kLeft},
        {os::vpad::kRight, os::pro::kRight}, {os::vpad::kL, os::pro::kL}, {os::vpad::kR, os::pro::kR},
        {os::vpad::kZL, os::pro::kZL}, {os::vpad::kZR, os::pro::kZR}, {os::vpad::kPlus, os::pro::kPlus},
        {os::vpad::kMinus, os::pro::kMinus}, {os::vpad::kHome, os::pro::kHome}};
    // shortcut: the default keys only (not the player's keyboard.ini); read it here if guests ask.
    static const keymap::Keys keys = keymap::default_keys();
    const bool* down = SDL_GetKeyboardState(nullptr);
    float kx = 0, ky = 0;
    for (size_t i = 0; i < keymap::kInputCount; ++i) {
        if (!down[keys[i]]) continue;
        for (const auto& [vpad, pro] : kAsPro) {
            if (keymap::kInputs[i].button & vpad) s.buttons |= pro;
        }
        kx += keymap::kInputs[i].stick_x;
        ky += keymap::kInputs[i].stick_y;
    }
    if (kx != 0 || ky != 0) s.left_x = std::clamp(kx, -1.0f, 1.0f), s.left_y = std::clamp(ky, -1.0f, 1.0f);
    // SMASH_NETPLAY_TEST_A=1: A for a tenth of a second every two seconds (trying netplay without a pad).
    static const bool test_a = std::getenv("SMASH_NETPLAY_TEST_A") != nullptr;
    if (test_a && SDL_GetTicks() % 2000 < 100) s.buttons |= os::pro::kA;
    return s;
}

void show_error(const std::string& text) {
    std::fprintf(stderr, "smash: netplay: %s\n", text.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Smash 4 PC netplay", text.c_str(), nullptr);
}

} // namespace

// ------------------------------------------------------------------ host

bool start_host(const std::string& name) {
    if (!g_socket.open(kDefaultPort)) {
        std::fprintf(stderr, "ttt2: netplay: no UDP socket (error %d)\n", WSAGetLastError());
        return false;
    }
    g_room_name = name.empty() ? "Player" : name;
    g_hosting = true;
    std::fprintf(stderr, "ttt2: netplay: hosting %s's room on UDP port %u\n", g_room_name.c_str(), g_socket.lan().port);
    std::thread(encoder_main).detach();
    std::thread(host_receive_main).detach();
    std::thread(host_rooms_main).detach();
    return true;
}

bool hosting() { return g_hosting; }
bool wants_video() { return g_guest_count > 0; }

bool remote_pad(int channel, os::ProPadState& state) {
    if (!g_hosting || channel < 0 || channel >= os::pro::kChannels) return false;
    std::lock_guard lock(g_mutex);
    if (!g_guests[channel]) return false;
    state = g_guests[channel]->pad;
    state.connected = true;
    return true;
}

void video_frame(const uint8_t* pixels, bool bgra, std::function<void()> wait, std::function<void()> done) {
    std::optional<PendingFrame> dropped;
    {
        std::lock_guard lock(g_frame_mutex);
        dropped = std::move(g_frame);
        g_frame = PendingFrame{pixels, bgra, std::move(wait), std::move(done)};
    }
    g_frame_cv.notify_one();
    if (dropped) dropped->release();
}

void audio(const float* frames, uint32_t count) {
    if (g_guest_count == 0) return;
    // 5 ms a datagram.
    static std::vector<int16_t> pending;
    for (uint32_t i = 0; i < 2 * count; ++i) {
        pending.push_back(static_cast<int16_t>(std::clamp(frames[i], -1.0f, 1.0f) * 32767.0f));
    }
    if (pending.size() < 2 * 240) return;
    send_all_guests(kAudio, pending.data(), pending.size() * 2);
    pending.clear();
}

// ------------------------------------------------------------------ guest

int run_guest(const std::string& target, const std::string& name) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO);
    Socket socket;
    if (!socket.open(0)) {
        show_error("No network (a UDP socket could not be opened).");
        return 1;
    }
    // Where the host may be: a room's (introduced by the room server, public and local) or an address.
    std::vector<Endpoint> candidates;
    uint32_t room = 0;
    if (target.rfind("room:", 0) == 0) {
        room = static_cast<uint32_t>(std::strtoul(target.c_str() + 5, nullptr, 16));
    } else {
        std::string host = target, port = std::to_string(kDefaultPort);
        if (std::count(host.begin(), host.end(), ':') == 1) {
            port = host.substr(host.find(':') + 1);
            host = host.substr(0, host.find(':'));
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
            show_error("Cannot find " + target + ".");
            return 1;
        }
        candidates.push_back(endpoint_of(*reinterpret_cast<sockaddr_in*>(found->ai_addr)));
        freeaddrinfo(found);
    }

    // Hello until welcomed (20 s), asking the room server for the host's addresses meanwhile.
    uint8_t hello[38] = {};
    std::memcpy(hello, &kMagic, 4);
    std::memcpy(hello + 4, &kVersion, 2);
    std::memcpy(hello + 6, name.data(), std::min<size_t>(name.size(), 31));
    std::atomic<bool> introduced{room == 0}, stop_rooms{false};
    std::mutex candidates_mutex;
    std::thread rooms;
    if (room) {
        rooms = std::thread([&] {
            const uint32_t id = random_id();
            for (int round = 0; !stop_rooms; ++round) {
                if (socket.pub().ip == 0 || round % 10 == 0) socket.ask_stun();
                CheckIn c;
                c.id = id;
                c.pub = socket.pub();
                c.lan = socket.lan();
                c.want = room;
                const CheckInReply r = check_in(c);
                for (const Introduction& peer : r.peers) {
                    if (peer.id != room) continue;
                    std::lock_guard lock(candidates_mutex);
                    // SMASH_NETPLAY_PUBLIC_ONLY=1: only the public address, as from another network (testing on one PC).
                    static const bool public_only = std::getenv("SMASH_NETPLAY_PUBLIC_ONLY") != nullptr;
                    for (Endpoint e : {peer.pub, public_only ? Endpoint{} : peer.lan}) {
                        if (e.ip && std::find(candidates.begin(), candidates.end(), e) == candidates.end()) candidates.push_back(e);
                    }
                    introduced = true;
                }
                for (int i = 0; i < 10 && !stop_rooms; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    Endpoint host_address{};
    int player = 0;
    uint32_t width = 0, height = 0;
    bool full = false;
    std::vector<uint8_t> p(2048);
    const auto asked = Clock::now();
    while (player == 0 && !full && Clock::now() - asked < std::chrono::seconds(20)) {
        {
            std::lock_guard lock(candidates_mutex);
            for (const Endpoint& e : candidates) socket.send(e, kHello, hello, sizeof hello);
        }
        Endpoint from;
        const size_t n = socket.receive(p.data(), p.size(), from);
        if (n >= 6 && p[0] == kWelcome) {
            host_address = from;
            player = p[1] + 2;
            width = p[2] | p[3] << 8;
            height = p[4] | p[5] << 8;
        } else if (n >= 1 && p[0] == kFull) {
            full = true;
        }
    }
    stop_rooms = true;
    if (rooms.joinable()) rooms.join();
    if (player == 0) {
        show_error(full           ? "That room is full (8 players)."
                   : !introduced  ? "The room server did not know that room: it has closed, or the server is not reachable."
                   : room         ? "Could not reach the host's PC. One of your routers may not allow direct connections "
                                    "(some mobile and carrier connections); a VPN such as Tailscale works around it."
                                  : "No answer from " + target + ". Is the host's game running with netplay on?");
        return 1;
    }

    char title[96];
    std::snprintf(title, sizeof title, "Smash 4 PC - netplay (player %d)", player);
    SDL_Window* window = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (renderer == nullptr) {
        show_error(std::string("No window: ") + SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderLogicalPresentation(renderer, static_cast<int>(width), static_cast<int>(height), SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_NV12);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, SDL_COLORSPACE_BT709_LIMITED);
    SDL_Texture* texture = SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(kAudioRate)};
    SDL_AudioStream* sound = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (sound) SDL_ResumeAudioStreamDevice(sound);

    // Received on a thread of its own: frames put together from their pieces and decoded (the newest picture for
    // the window), the sound straight to the device.
    std::mutex picture_mutex;
    std::vector<uint8_t> picture;
    bool fresh = false;
    std::atomic<bool> connected{true}, leaving{false};
    std::atomic<uint32_t> pictures{0}, video_bytes{0}, lost_frames{0}; // for the log, every 10 s
    std::thread receiver([&] {
        Decoder decoder;
        if (!decoder.open(width, height)) std::fprintf(stderr, "smash: netplay: no H.264 decoder\n");
        uint32_t frame = 0, pieces = 0, got = 0;
        std::vector<uint8_t> data;
        std::vector<bool> have;
        bool broken = true; // until a keyframe
        auto heard = Clock::now(), asked_key = Clock::time_point{};
        const auto ask_key = [&] {
            if (Clock::now() - asked_key < std::chrono::milliseconds(250)) return;
            asked_key = Clock::now();
            socket.send(host_address, kKeyRequest);
        };
        std::vector<uint8_t> q(2048);
        while (!leaving) {
            Endpoint from;
            const size_t n = socket.receive(q.data(), q.size(), from);
            if (n == 0 || !(from == host_address)) {
                if (Clock::now() - heard > kSilence) break;
                continue;
            }
            heard = Clock::now();
            if (q[0] == kVideo && n > 10) {
                const uint32_t f = le32(q.data() + 1);
                uint16_t index, count;
                std::memcpy(&index, q.data() + 5, 2);
                std::memcpy(&count, q.data() + 7, 2);
                if (f < frame || count == 0 || index >= count) continue; // (an old frame's piece)
                if (f != frame) {
                    if (got != pieces) { // the last frame never completed
                        broken = true;
                        ++lost_frames;
                    }
                    frame = f;
                    pieces = count;
                    got = 0;
                    data.assign(size_t{count} * kPiece, 0);
                    have.assign(count, false);
                }
                if (have[index]) continue;
                have[index] = true;
                ++got;
                const size_t length = n - 10;
                std::memcpy(data.data() + size_t{index} * kPiece, q.data() + 10, length);
                if (index + 1 == count) data.resize(size_t{index} * kPiece + length);
                if (got != pieces) continue;
                const bool key = q[9] != 0;
                if (broken && !key) {
                    ask_key();
                    continue;
                }
                broken = false;
                video_bytes += static_cast<uint32_t>(data.size());
                decoder.decode(data.data(), data.size(), [&](const BYTE* nv12, UINT32 w, UINT32 h) {
                    if (w != width || h < height) return;
                    std::lock_guard lock(picture_mutex);
                    picture.assign(nv12, nv12 + size_t{w} * h * 3 / 2);
                    fresh = true;
                    ++pictures;
                });
            } else if (q[0] == kAudio && sound) {
                // Over 150 ms queued (the link stalled): start over rather than lag behind.
                if (SDL_GetAudioStreamQueued(sound) > static_cast<int>(kAudioRate * 4 * 15 / 100)) SDL_ClearAudioStream(sound);
                SDL_PutAudioStreamData(sound, q.data() + 1, static_cast<int>(n - 1));
            }
        }
        connected = false;
    });

    uint32_t sequence = 0;
    auto last_log = Clock::now();
    bool running = true;
    while (running && connected) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11) {
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            }
        }
        // The controller, every frame (a lost one is replaced by the next).
        const os::ProPadState now = local_pad();
        uint8_t input[16];
        ++sequence;
        std::memcpy(input, &sequence, 4);
        std::memcpy(input + 4, &now.buttons, 4);
        const int16_t axes[4] = {static_cast<int16_t>(now.left_x * 32767), static_cast<int16_t>(now.left_y * 32767),
                                 static_cast<int16_t>(now.right_x * 32767), static_cast<int16_t>(now.right_y * 32767)};
        std::memcpy(input + 8, axes, 8);
        socket.send(host_address, kInput, input, sizeof input);
        if (Clock::now() - last_log > std::chrono::seconds(10)) {
            std::fprintf(stderr, "smash: netplay: %.1f pictures/s, %.1f Mbit/s, %u frames lost\n", pictures.exchange(0) / 10.0,
                         video_bytes.exchange(0) * 8 / 10.0 / 1e6, lost_frames.exchange(0));
            last_log = Clock::now();
        }
        {
            std::lock_guard lock(picture_mutex);
            if (fresh) {
                SDL_UpdateNVTexture(texture, nullptr, picture.data(), static_cast<int>(width), picture.data() + size_t{width} * height,
                                    static_cast<int>(width));
                fresh = false;
            }
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }
    socket.send(host_address, kBye);
    leaving = true;
    receiver.join();
    // SMASH_NETPLAY_DUMP=FILE: the last picture, raw NV12, for checking the stream.
    if (const char* dump = std::getenv("SMASH_NETPLAY_DUMP")) {
        std::lock_guard lock(picture_mutex);
        if (FILE* f = std::fopen(dump, "wb")) {
            std::fwrite(picture.data(), 1, picture.size(), f);
            std::fclose(f);
        }
    }
    if (running && !connected) show_error("The host's game closed or the connection was lost.");
    SDL_Quit();
    return 0;
}

} // namespace cafe::host::netplay
