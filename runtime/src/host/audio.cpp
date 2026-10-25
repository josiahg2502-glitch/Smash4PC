#include "host/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace cafe::host {
namespace {

SDL_AudioStream* g_stream = nullptr;
float g_volume = 1.0f; // TTT2_AUDIO_VOLUME, percent

// TTT2_AUDIO_DUMP: 16-bit stereo WAV; the header's sizes are rewritten
// every second so a run that is killed still leaves a valid file.
std::mutex g_dump_mutex;
FILE* g_dump = nullptr;
uint32_t g_dump_frames = 0;

void put_le(uint8_t* p, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

void write_wav_header() {
    uint8_t h[44];
    const uint32_t data = g_dump_frames * 4;
    std::memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + data, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2); // PCM
    put_le(h + 22, 2, 2);
    put_le(h + 24, kAudioRate, 4);
    put_le(h + 28, kAudioRate * 4, 4);
    put_le(h + 32, 4, 2);
    put_le(h + 34, 16, 2);
    std::memcpy(h + 36, "data", 4);
    put_le(h + 40, data, 4);
    std::fseek(g_dump, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, g_dump);
    std::fseek(g_dump, 0, SEEK_END);
}

void dump(const float* frames, uint32_t count) {
    std::lock_guard lock(g_dump_mutex);
    int16_t samples[2 * 512];
    for (uint32_t done = 0; done < count;) {
        const uint32_t n = std::min<uint32_t>(count - done, 512);
        for (uint32_t i = 0; i < 2 * n; ++i) {
            const float s = frames[2 * done + i] * 32768.0f;
            samples[i] = static_cast<int16_t>(s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : s);
        }
        std::fwrite(samples, 4, n, g_dump);
        const uint32_t before = g_dump_frames / kAudioRate;
        g_dump_frames += n;
        if (g_dump_frames / kAudioRate != before) {
            write_wav_header();
            std::fflush(g_dump);
        }
        done += n;
    }
}

} // namespace

void open_audio() {
    if (const char* path = std::getenv("TTT2_AUDIO_DUMP")) {
        g_dump = std::fopen(path, "wb");
        if (g_dump) write_wav_header();
        else std::fprintf(stderr, "ttt2: audio: cannot write %s\n", path);
    }
    if (const char* volume = std::getenv("TTT2_AUDIO_VOLUME")) {
        g_volume = std::clamp(static_cast<float>(std::atof(volume)) / 100.0f, 0.0f, 1.0f);
    }
    if (const char* setting = std::getenv("TTT2_AUDIO"); setting && std::strcmp(setting, "0") == 0) return;
    // Without a window there is no event loop to act on SDL's quit event:
    // leave SIGINT/SIGTERM to their default action.
    if (SDL_WasInit(0) == 0) SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "ttt2: audio: %s; no sound\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(kAudioRate)};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_stream == nullptr) {
        std::fprintf(stderr, "ttt2: audio: %s; no sound\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    std::fprintf(stderr, "ttt2: audio: %s\n", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_stream)));
}

bool audio_device_active() { return g_stream != nullptr; }

void queue_audio(const float* frames, uint32_t count) {
    if (g_stream) {
        // A stalled device: drop the backlog rather than let it grow.
        const uint32_t queued = queued_audio_frames();
        if (queued > kAudioRate / 4) {
            std::fprintf(stderr, "ttt2: audio: output backlog over 250 ms, dropped\n");
            SDL_ClearAudioStream(g_stream);
        } else if (queued < count) {
            // About to run dry: refill to the target with silence.
            static const float silence[2 * kAudioQueueTarget] = {};
            SDL_PutAudioStreamData(g_stream, silence, static_cast<int>((kAudioQueueTarget - queued) * 2 * sizeof(float)));
        }
        if (g_volume == 1.0f) {
            SDL_PutAudioStreamData(g_stream, frames, static_cast<int>(count * 2 * sizeof(float)));
        } else {
            float scaled[2 * 512];
            for (uint32_t done = 0; done < count;) {
                const uint32_t n = std::min<uint32_t>(count - done, 512);
                for (uint32_t i = 0; i < 2 * n; ++i) scaled[i] = frames[2 * done + i] * g_volume;
                SDL_PutAudioStreamData(g_stream, scaled, static_cast<int>(n * 2 * sizeof(float)));
                done += n;
            }
        }
    }
    if (g_dump) dump(frames, count);
}

uint32_t queued_audio_frames() {
    if (g_stream == nullptr) return 0;
    const int bytes = SDL_GetAudioStreamQueued(g_stream);
    return bytes > 0 ? static_cast<uint32_t>(bytes) / (2 * sizeof(float)) : 0;
}

} // namespace cafe::host
