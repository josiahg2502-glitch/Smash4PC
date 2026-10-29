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
#include <deque>
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

// ------------------------------------------------------------------ the protocol
// Packets: a type byte, the payload's length (4 bytes, little-endian), the payload.
constexpr uint32_t kMagic = 0x504E3453; // "S4NP"
constexpr uint16_t kVersion = 1;
enum Type : uint8_t {
    kHello = 1,   // guest: magic, version, name (32 bytes)
    kWelcome = 2, // host: channel (1 byte), picture width and height (2 bytes each)
    kFull = 3,    // host: no free controller, the connection closes
    kInput = 4,   // guest: buttons (os::pro bits, 4 bytes), sticks (4 x int16, up positive)
    kVideo = 5,   // host: H.264 (Annex B) for one frame
    kAudio = 6,   // host: 48 kHz stereo int16 frames
};
constexpr uint32_t kMaxPacket = 4u << 20;

bool send_all(SOCKET s, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        const int n = send(s, p, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(SOCKET s, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size) {
        const int n = recv(s, p, static_cast<int>(size), 0);
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> packet(Type type, const void* payload, size_t size) {
    std::vector<uint8_t> p(5 + size);
    p[0] = type;
    const auto n = static_cast<uint32_t>(size);
    std::memcpy(p.data() + 1, &n, 4);
    if (size) std::memcpy(p.data() + 5, payload, size);
    return p;
}

bool read_packet(SOCKET s, Type& type, std::vector<uint8_t>& payload) {
    uint8_t header[5];
    if (!recv_all(s, header, 5)) return false;
    uint32_t size;
    std::memcpy(&size, header + 1, 4);
    if (size > kMaxPacket) return false;
    type = static_cast<Type>(header[0]);
    payload.resize(size);
    return size == 0 || recv_all(s, payload.data(), size);
}

void start_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

void no_delay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

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
    SOCKET socket = INVALID_SOCKET;
    int channel = 0;
    std::string name;
    std::atomic<bool> alive{true};
    bool need_key = true; // its video starts at a keyframe (under g_mutex)
    os::ProPadState pad{};
    bool pressed = false; // (logged once)
    // Sending, on a thread of its own: a slow guest only loses its own frames.
    std::mutex send_mutex;
    std::condition_variable send_cv;
    std::deque<std::vector<uint8_t>> queue;
    size_t queued_video = 0;
};

std::mutex g_mutex; // the guests and their pads
std::array<std::shared_ptr<Guest>, os::pro::kChannels> g_guests;
std::atomic<int> g_guest_count{0};
std::atomic<bool> g_hosting{false}, g_force_key{false};

void enqueue(Guest& g, std::vector<uint8_t> p, bool video) {
    {
        std::lock_guard lock(g.send_mutex);
        if (video) {
            // Over three frames behind: drop video until the next keyframe.
            if (g.queued_video >= 3) {
                std::lock_guard guests(g_mutex);
                g.need_key = true;
                g_force_key = true;
                return;
            }
            ++g.queued_video;
        } else if (g.queue.size() > 400) {
            return; // (a stalled guest: its sound waits no longer)
        }
        g.queue.push_back(std::move(p));
    }
    g.send_cv.notify_one();
}

void sender(std::shared_ptr<Guest> g) {
    for (;;) {
        std::vector<uint8_t> p;
        {
            std::unique_lock lock(g->send_mutex);
            g->send_cv.wait(lock, [&] { return !g->queue.empty() || !g->alive; });
            if (!g->alive) return;
            p = std::move(g->queue.front());
            g->queue.pop_front();
            if (p[0] == kVideo) --g->queued_video;
        }
        if (!send_all(g->socket, p.data(), p.size())) {
            g->alive = false;
            return;
        }
    }
}

void broadcast(Type type, const void* data, size_t size, bool key = false) {
    std::vector<std::shared_ptr<Guest>> to;
    {
        std::lock_guard lock(g_mutex);
        for (auto& g : g_guests) {
            if (!g || !g->alive) continue;
            if (type == kVideo) {
                if (g->need_key && !key) continue;
                g->need_key = false;
            }
            to.push_back(g);
        }
    }
    if (to.empty()) return;
    const std::vector<uint8_t> p = packet(type, data, size);
    for (auto& g : to) enqueue(*g, p, type == kVideo);
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
    const auto start = std::chrono::steady_clock::now();
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
        const int64_t time = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count() / 100;
        encoder.encode(nv12, time, g_force_key.exchange(false),
                       [](const BYTE* data, DWORD size, bool key) { broadcast(kVideo, data, size, key); });
    }
}

void drop_guest(const std::shared_ptr<Guest>& g) {
    {
        std::lock_guard lock(g_mutex);
        if (g_guests[g->channel] == g) g_guests[g->channel].reset();
    }
    g->alive = false;
    g->send_cv.notify_all();
    closesocket(g->socket);
    --g_guest_count;
    std::fprintf(stderr, "ttt2: netplay: %s (player %d) left\n", g->name.c_str(), g->channel + 2);
}

void receiver(std::shared_ptr<Guest> g) {
    Type type;
    std::vector<uint8_t> p;
    while (g->alive && read_packet(g->socket, type, p)) {
        if (type != kInput || p.size() < 12) continue;
        os::ProPadState s{true};
        std::memcpy(&s.buttons, p.data(), 4);
        int16_t axes[4];
        std::memcpy(axes, p.data() + 4, 8);
        s.left_x = axes[0] / 32767.0f;
        s.left_y = axes[1] / 32767.0f;
        s.right_x = axes[2] / 32767.0f;
        s.right_y = axes[3] / 32767.0f;
        std::lock_guard lock(g_mutex);
        if (s.buttons && !g->pad.buttons && !g->pressed) {
            g->pressed = true;
            std::fprintf(stderr, "ttt2: netplay: player %d's controller works\n", g->channel + 2);
        }
        g->pad = s;
    }
    drop_guest(g);
}

void welcome(SOCKET s) {
    no_delay(s);
    DWORD timeout = 5000; // the hello, then no limit
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    Type type;
    std::vector<uint8_t> p;
    uint32_t magic = 0;
    uint16_t version = 0;
    if (!read_packet(s, type, p) || type != kHello || p.size() < 38 || (std::memcpy(&magic, p.data(), 4), magic != kMagic) ||
        (std::memcpy(&version, p.data() + 4, 2), version != kVersion)) {
        closesocket(s);
        return;
    }
    timeout = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    auto g = std::make_shared<Guest>();
    g->socket = s;
    g->name.assign(reinterpret_cast<const char*>(p.data() + 6), strnlen(reinterpret_cast<const char*>(p.data() + 6), 32));
    {
        std::lock_guard lock(g_mutex);
        const auto free = std::find(g_guests.begin(), g_guests.end(), nullptr);
        if (free == g_guests.end()) {
            const auto full = packet(kFull, nullptr, 0);
            send_all(s, full.data(), full.size());
            closesocket(s);
            return;
        }
        g->channel = static_cast<int>(free - g_guests.begin());
        *free = g;
    }
    const uint8_t w[5] = {static_cast<uint8_t>(g->channel), kStreamWidth & 0xFF, kStreamWidth >> 8, kStreamHeight & 0xFF,
                          kStreamHeight >> 8};
    const auto hello = packet(kWelcome, w, sizeof w);
    send_all(s, hello.data(), hello.size());
    ++g_guest_count;
    g_force_key = true;
    std::fprintf(stderr, "ttt2: netplay: %s joined as player %d\n", g->name.c_str(), g->channel + 2);
    std::thread(sender, g).detach();
    std::thread(receiver, g).detach();
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

bool start_host(uint16_t port) {
    start_winsock();
    SOCKET listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    DWORD v6only = 0; // IPv4 guests too
    setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_any;
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
        listen(listener, 8) != 0) {
        std::fprintf(stderr, "ttt2: netplay: cannot take guests on port %u (error %d)\n", port, WSAGetLastError());
        closesocket(listener);
        return false;
    }
    g_hosting = true;
    std::fprintf(stderr, "ttt2: netplay: hosting on port %u (players 2-8 join with smash.exe --join <this PC>:%u)\n", port,
                 port);
    std::thread(encoder_main).detach();
    std::thread([listener] {
        for (;;) {
            const SOCKET s = accept(listener, nullptr, nullptr);
            if (s == INVALID_SOCKET) return;
            std::thread(welcome, s).detach();
        }
    }).detach();
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
    // ~10 ms a packet.
    static std::vector<int16_t> pending;
    for (uint32_t i = 0; i < 2 * count; ++i) {
        pending.push_back(static_cast<int16_t>(std::clamp(frames[i], -1.0f, 1.0f) * 32767.0f));
    }
    if (pending.size() < 2 * 480) return;
    broadcast(kAudio, pending.data(), pending.size() * 2);
    pending.clear();
}

// ------------------------------------------------------------------ guest

int run_guest(const std::string& address, const std::string& name) {
    start_winsock();
    std::string host = address, port = std::to_string(kDefaultPort);
    // HOST:PORT, [IPv6]:PORT, or a bare host.
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        if (close != std::string::npos) {
            if (close + 1 < host.size() && host[close + 1] == ':') port = host.substr(close + 2);
            host = host.substr(1, close - 1);
        }
    } else if (std::count(host.begin(), host.end(), ':') == 1) {
        port = host.substr(host.find(':') + 1);
        host = host.substr(0, host.find(':'));
    }
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO);

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        show_error("Cannot find " + address + ".");
        return 1;
    }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = found; a && s == INVALID_SOCKET; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s != INVALID_SOCKET && connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    freeaddrinfo(found);
    if (s == INVALID_SOCKET) {
        show_error("Cannot connect to " + address + ". Is the host's game running with netplay on, and port " + port +
                   " open to you (forwarded on their router, or on the same LAN or VPN)?");
        return 1;
    }
    no_delay(s);
    uint8_t hello[38] = {};
    std::memcpy(hello, &kMagic, 4);
    std::memcpy(hello + 4, &kVersion, 2);
    std::memcpy(hello + 6, name.data(), std::min<size_t>(name.size(), 31));
    const auto hello_packet = packet(kHello, hello, sizeof hello);
    Type type = kHello;
    std::vector<uint8_t> p;
    if (!send_all(s, hello_packet.data(), hello_packet.size()) || !read_packet(s, type, p) || type == kFull ||
        type != kWelcome || p.size() < 5) {
        show_error(type == kFull ? "The host's game is full (8 players)." : "The host did not answer as Smash 4 PC netplay.");
        closesocket(s);
        return 1;
    }
    const int player = p[0] + 2;
    const uint32_t width = p[1] | p[2] << 8, height = p[3] | p[4] << 8;

    char title[96];
    std::snprintf(title, sizeof title, "Smash 4 PC - netplay at %s (player %d)", address.c_str(), player);
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

    // Received on a thread of its own: the newest picture for the window, the sound straight to the device.
    std::mutex picture_mutex;
    std::vector<uint8_t> picture;
    bool fresh = false;
    std::atomic<bool> connected{true};
    std::atomic<uint32_t> pictures{0}, video_bytes{0}; // for the log, every 10 s
    std::thread([&, s] {
        Decoder decoder;
        if (!decoder.open(width, height)) std::fprintf(stderr, "smash: netplay: no H.264 decoder\n");
        Type t;
        std::vector<uint8_t> data;
        while (read_packet(s, t, data)) {
            if (t == kVideo) {
                video_bytes += static_cast<uint32_t>(data.size());
                decoder.decode(data.data(), data.size(), [&](const BYTE* nv12, UINT32 w, UINT32 h) {
                    if (w != width || h < height) return;
                    std::lock_guard lock(picture_mutex);
                    picture.assign(nv12, nv12 + size_t{w} * h * 3 / 2);
                    fresh = true;
                    ++pictures;
                });
            } else if (t == kAudio && sound) {
                // Over 150 ms queued (the link stalled): start over rather than lag behind.
                if (SDL_GetAudioStreamQueued(sound) > static_cast<int>(kAudioRate * 4 * 15 / 100)) SDL_ClearAudioStream(sound);
                SDL_PutAudioStreamData(sound, data.data(), static_cast<int>(data.size()));
            }
        }
        connected = false;
    }).detach();

    os::ProPadState sent{};
    auto last_sent = std::chrono::steady_clock::now(), last_log = last_sent;
    bool running = true;
    while (running && connected) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11) {
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            }
        }
        // The controller: when it changes, and every 50 ms.
        const os::ProPadState now = local_pad();
        const auto t = std::chrono::steady_clock::now();
        if (std::memcmp(&now, &sent, sizeof now) != 0 || t - last_sent > std::chrono::milliseconds(50)) {
            uint8_t input[12];
            std::memcpy(input, &now.buttons, 4);
            const int16_t axes[4] = {static_cast<int16_t>(now.left_x * 32767), static_cast<int16_t>(now.left_y * 32767),
                                     static_cast<int16_t>(now.right_x * 32767), static_cast<int16_t>(now.right_y * 32767)};
            std::memcpy(input + 4, axes, 8);
            const auto pkt = packet(kInput, input, sizeof input);
            if (!send_all(s, pkt.data(), pkt.size())) break;
            sent = now;
            last_sent = t;
        }
        if (t - last_log > std::chrono::seconds(10)) {
            std::fprintf(stderr, "smash: netplay: %.1f pictures/s, %.1f Mbit/s\n", pictures.exchange(0) / 10.0,
                         video_bytes.exchange(0) * 8 / 10.0 / 1e6);
            last_log = t;
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
    closesocket(s);
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
