// snd_core (AX): voices, their mixing, and the audio frame clock.
//
// Every 3 ms audio frame AX mixes each playing voice into the device
// buses, runs the title's frame callbacks, passes each device's output
// through the device final-mix callbacks, and plays the TV output
// (host/audio.h). The frame clock follows the playback device, so the
// title produces sound exactly as fast as it is played.
//
// A voice reads PCM16, PCM8 or DSP-ADPCM samples from guest memory at its
// sample-rate ratio (16.16, relative to AX's 32 kHz renderer) with linear
// interpolation, scaled by its volume envelope (VE) and mixed into each
// device channel and bus by its device mix. Output is 48 kHz, 144 samples
// a frame; final-mix buffers hold 32-bit samples in 16-bit range.
//
// This title's CRI middleware mixes in software and outputs 5.1 at
// 44.1 kHz through six looping PCM16 voices whose device mixes fold them
// down to TV stereo; the VE comes from the MIX library (snd_user.cpp). Only
// the main bus is mixed: the title gives its voices no aux (effect) sends.
// Filters (LPF, biquad) are accepted and not applied.
//
// AXVPB (guest, 0x58 bytes, Cemu's layout): +0x00 index, +0x04 state,
// +0x1C priority, +0x20 drop callback, +0x24 user data, +0x34 offsets
// (format u16, loop flag u16, loop/end/current u32, samples pointer).
// TTT2_TRACE_AX=1 logs voice set-up and output levels.

#include "ax.h"
#include "kernel.h"

#include "cafe/export.h"
#include "cafe/sysmem.h"
#include "host/audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace cafe::os {
namespace {

constexpr int kVoiceCount = 96;
constexpr uint32_t kVpbSize = 0x58;
constexpr uint32_t kRendererSamples = 96; // 3 ms at the 32 kHz renderer
constexpr uint32_t kOutputSamples = 144;  // 3 ms at 48 kHz
constexpr auto kFramePeriod = std::chrono::microseconds(3000);
constexpr int kTvChannels = 6, kDrcChannels = 4, kBuses = 4;
enum Format : uint16_t { kAdpcm = 0x00, kPcm16 = 0x0A, kPcm8 = 0x19 };

const bool g_trace = std::getenv("TTT2_TRACE_AX") != nullptr;
#define AX_TRACE(...)                                                                                                  \
    do {                                                                                                               \
        if (g_trace) std::fprintf(stderr, "ax: " __VA_ARGS__);                                                         \
    } while (0)

// One channel and bus of a device mix: 1.0 is 0x8000, delta per 32 kHz sample.
struct ChannelMix {
    float volume = 0, delta = 0;
};

struct Voice {
    uint32_t vpb = 0;
    bool acquired = false;
    bool running = false;
    uint16_t format = kPcm16;
    bool loop = false;
    uint32_t samples = 0; // guest address of the sample data
    // In samples (ADPCM: decoded samples, frame headers excluded).
    uint64_t loop_start = 0, end = 0;
    uint64_t cursor = 0; // the next sample to read
    uint32_t ratio = 0x10000;
    // Interpolation between the last two samples read.
    double phase = 0;
    float previous = 0, current = 0;
    float ve = 0, ve_delta = 0;
    ChannelMix tv[kTvChannels][kBuses];
    ChannelMix drc[2][kDrcChannels][kBuses];
    // DSP-ADPCM decoder state and loop context.
    int16_t coefs[16]{};
    uint16_t pred_scale = 0;
    int32_t yn1 = 0, yn2 = 0;
    uint16_t loop_pred_scale = 0;
    int32_t loop_yn1 = 0, loop_yn2 = 0;
};

std::recursive_mutex g_ax_mutex;
Voice g_voices[kVoiceCount];
bool g_initialized = false;
uint32_t g_frame_callback = 0;
uint32_t g_frame_callback2 = 0;
uint32_t g_aux_callbacks[3][2]{};
uint32_t g_final_mix_callbacks[2]{};
uint16_t g_master_volume = 0x8000;
Thread* g_ax_thread = nullptr;

// Main-bus output of the frame being mixed, in 16-bit sample units.
float g_tv_bus[kTvChannels][kOutputSamples];
float g_drc_bus[2][kDrcChannels][kOutputSamples];

// AXFINALMIXCBPARAM: +0x00 channel pointer array, +0x04 input channels,
// +0x06 samples, +0x08 devices, +0x0A output channels (u16 each).
struct FinalMix {
    uint32_t channels, devices;
    uint32_t param = 0, pointers = 0, samples = 0;
};
FinalMix g_final_mix[2] = {{kTvChannels, 1}, {kDrcChannels, 2}};

// ADPCM addresses count nibbles; every 16-nibble frame starts with a 2-nibble
// header, leaving 14 samples.
uint64_t nibble_to_sample(uint32_t nibble) { return uint64_t{nibble / 16} * 14 + (nibble % 16 < 2 ? 0 : nibble % 16 - 2); }
uint32_t sample_to_nibble(uint64_t sample) { return static_cast<uint32_t>(sample / 14 * 16 + sample % 14 + 2); }

uint64_t to_sample(const Voice& v, uint32_t offset) {
    return v.format == kAdpcm ? nibble_to_sample(offset) : offset;
}
uint32_t from_sample(const Voice& v, uint64_t sample) {
    return v.format == kAdpcm ? sample_to_nibble(sample) : static_cast<uint32_t>(sample);
}

Voice* voice_of(uint32_t vpb) {
    if (vpb == 0) return nullptr;
    const uint32_t index = field<uint32_t>(vpb, 0x00);
    return index < kVoiceCount && g_voices[index].vpb == vpb ? &g_voices[index] : nullptr;
}

void publish_state(const Voice& v) {
    field<uint32_t>(v.vpb, 0x04) = v.running ? 1u : 0u;
    field<uint32_t>(v.vpb, 0x34 + 0x0C) = from_sample(v, v.cursor);
}

// ------------------------------------------------------------- mixing

float decode_adpcm(Voice& v) {
    const uint32_t frame = static_cast<uint32_t>(v.cursor / 14), index = static_cast<uint32_t>(v.cursor % 14);
    const uint32_t frame_address = v.samples + frame * 8;
    if (index == 0) v.pred_scale = *guest<uint8_t>(frame_address);
    const uint8_t byte = *guest<uint8_t>(frame_address + 1 + index / 2);
    int32_t nibble = index % 2 == 0 ? byte >> 4 : byte & 0xF;
    if (nibble >= 8) nibble -= 16;
    const uint32_t predictor = (v.pred_scale >> 4) & 7;
    const int32_t c1 = v.coefs[predictor * 2], c2 = v.coefs[predictor * 2 + 1];
    int32_t sample = ((nibble * (1 << (v.pred_scale & 0xF))) * 2048 + c1 * v.yn1 + c2 * v.yn2 + 1024) >> 11;
    sample = std::clamp(sample, -32768, 32767);
    v.yn2 = v.yn1;
    v.yn1 = sample;
    return static_cast<float>(sample);
}

// Reads the sample at the cursor and moves on: past the end a looping voice
// continues at its loop start, any other voice stops.
float next_sample(Voice& v) {
    if (!v.running) return 0.0f;
    float s;
    switch (v.format) {
    case kPcm16:
        s = static_cast<int16_t>(uint16_t{*guest<be<uint16_t>>(v.samples + static_cast<uint32_t>(v.cursor) * 2)});
        break;
    case kPcm8: s = static_cast<float>(static_cast<int8_t>(*guest<uint8_t>(v.samples + static_cast<uint32_t>(v.cursor))) * 256); break;
    case kAdpcm: s = decode_adpcm(v); break;
    default: s = 0.0f; break;
    }
    if (v.cursor >= v.end) {
        if (v.loop) {
            v.cursor = v.loop_start;
            if (v.format == kAdpcm) {
                v.pred_scale = v.loop_pred_scale;
                v.yn1 = v.loop_yn1;
                v.yn2 = v.loop_yn2;
            }
        } else {
            v.running = false;
        }
    } else {
        ++v.cursor;
    }
    return s;
}

// Mixes `in` into `out` with a ramping volume; returns the volume at the end.
float mix_into(float* out, const float* in, const ChannelMix& m) {
    float volume = m.volume;
    const float delta = m.delta * (float{kRendererSamples} / kOutputSamples);
    if (delta == 0.0f) {
        for (uint32_t i = 0; i < kOutputSamples; ++i) out[i] += in[i] * volume;
        return volume;
    }
    for (uint32_t i = 0; i < kOutputSamples; ++i) {
        volume += delta;
        out[i] += in[i] * volume;
    }
    return volume;
}

// Advances a mix's ramp by one frame without mixing (unmixed buses).
void ramp(ChannelMix& m) {
    if (m.delta != 0.0f) m.volume = std::clamp(m.volume + m.delta * kRendererSamples, 0.0f, 2.0f);
}

void mix_voice(Voice& v) {
    float samples[kOutputSamples];
    const double step = v.ratio / 65536.0 * (double{kRendererSamples} / kOutputSamples);
    float gain = v.ve;
    const float gain_delta = v.ve_delta * (float{kRendererSamples} / kOutputSamples);
    for (uint32_t i = 0; i < kOutputSamples; ++i) {
        v.phase += step;
        while (v.phase >= 1.0) {
            v.previous = v.current;
            v.current = next_sample(v);
            v.phase -= 1.0;
        }
        gain += gain_delta;
        samples[i] = (v.previous + (v.current - v.previous) * static_cast<float>(v.phase)) * gain;
    }
    v.ve = std::clamp(v.ve + v.ve_delta * kRendererSamples, 0.0f, 2.0f);
    for (int c = 0; c < kTvChannels; ++c) {
        ChannelMix& m = v.tv[c][0];
        if (m.volume != 0.0f || m.delta != 0.0f) m.volume = std::clamp(mix_into(g_tv_bus[c], samples, m), 0.0f, 2.0f);
        for (int b = 1; b < kBuses; ++b) ramp(v.tv[c][b]);
    }
    for (int d = 0; d < 2; ++d) {
        for (int c = 0; c < kDrcChannels; ++c) {
            ChannelMix& m = v.drc[d][c][0];
            if (m.volume != 0.0f || m.delta != 0.0f) {
                m.volume = std::clamp(mix_into(g_drc_bus[d][c], samples, m), 0.0f, 2.0f);
            }
            for (int b = 1; b < kBuses; ++b) ramp(v.drc[d][c][b]);
        }
    }
}

void mix_voices() {
    std::memset(g_tv_bus, 0, sizeof g_tv_bus);
    std::memset(g_drc_bus, 0, sizeof g_drc_bus);
    for (Voice& v : g_voices) {
        if (!v.acquired || !v.running) continue;
        mix_voice(v);
        publish_state(v);
    }
}

// Hands a device's output to its final-mix callback; `out` (TV only)
// receives the result as stereo frames in [-1, 1].
void run_final_mix(Thread* t, int device, uint32_t callback, float* out) {
    const FinalMix& mix = g_final_mix[device];
    const uint32_t buffers = mix.channels * mix.devices;
    for (uint32_t c = 0; c < buffers; ++c) {
        const float* bus = device == 0 ? g_tv_bus[c] : g_drc_bus[c / kDrcChannels][c % kDrcChannels];
        auto* samples = guest<be<int32_t>>(mix.samples + c * kOutputSamples * 4);
        for (uint32_t i = 0; i < kOutputSamples; ++i) {
            samples[i] = static_cast<int32_t>(std::clamp(bus[i], -2147483648.0f, 2147483520.0f));
        }
        field<uint32_t>(mix.pointers, c * 4) = mix.samples + c * kOutputSamples * 4;
    }
    field<uint32_t>(mix.param, 0x00) = mix.pointers;
    field<uint16_t>(mix.param, 0x04) = static_cast<uint16_t>(mix.channels);
    field<uint16_t>(mix.param, 0x06) = static_cast<uint16_t>(kOutputSamples);
    field<uint16_t>(mix.param, 0x08) = static_cast<uint16_t>(mix.devices);
    field<uint16_t>(mix.param, 0x0A) = static_cast<uint16_t>(mix.channels);
    if (callback) call_guest(t->ctx, callback, {mix.param});
    if (out == nullptr) return;
    // The TV is a stereo device (AXGetDeviceMode): channels 0 and 1.
    const float master = g_master_volume / 32768.0f / 32768.0f;
    const auto* left = guest<be<int32_t>>(mix.samples);
    const auto* right = guest<be<int32_t>>(mix.samples + kOutputSamples * 4);
    for (uint32_t i = 0; i < kOutputSamples; ++i) {
        out[2 * i] = std::clamp(static_cast<float>(int32_t{left[i]}) * master, -1.0f, 1.0f);
        out[2 * i + 1] = std::clamp(static_cast<float>(int32_t{right[i]}) * master, -1.0f, 1.0f);
    }
    if (g_trace) {
        static uint64_t frames;
        if (++frames % 333 == 0) {
            float peak = 0;
            for (uint32_t i = 0; i < 2 * kOutputSamples; ++i) peak = std::max(peak, std::fabs(out[i]));
            AX_TRACE("output peak %.3f\n", peak);
        }
    }
}

// Waits for the next frame. Frames come every 3 ms, as on the console: the
// title's sound engine fills its voices from its own threads and expects AX
// to consume them at that pace (frames run in bursts, at the device's
// demand, read audio it has not written yet: half the output was silence).
// With a device, the period is nudged by at most 0.5% to keep the output
// queued ahead of the device near its target, following the device's clock
// (real drift needs a few hundredths of a percent; no audible pitch change).
void wait_for_frame(std::chrono::steady_clock::time_point& next) {
    std::chrono::duration<double, std::micro> period = kFramePeriod;
    if (host::audio_device_active()) {
        static double level = host::kAudioQueueTarget; // smoothed over ~0.3 s
        const uint32_t queued = host::queued_audio_frames();
        level += (queued - level) * 0.01;
        const double error = (level - host::kAudioQueueTarget) / host::kAudioQueueTarget;
        period *= 1.0 + std::clamp(error * 0.02, -0.005, 0.005);
        if (g_trace) {
            static uint32_t lowest = UINT32_MAX, frames = 0;
            lowest = std::min(lowest, queued);
            if (++frames == 333) {
                AX_TRACE("queue: average %.0f, lowest %u frames; period %+.3f%%\n", level, lowest,
                         (period / kFramePeriod - 1.0) * 100.0);
                lowest = UINT32_MAX;
                frames = 0;
            }
        }
    }
    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
    std::this_thread::sleep_until(next);
    // Running far behind (debugger, heavy load): skip, don't burst.
    if (std::chrono::steady_clock::now() - next > kFramePeriod * 10) next = std::chrono::steady_clock::now();
}

void* ax_main(void*) {
    Thread* t = g_ax_thread;
    bind_current_thread(t);
    auto next = std::chrono::steady_clock::now();
    float output[2 * kOutputSamples];
    for (;;) {
        wait_for_frame(next);
        uint32_t callback, callback2, final_mix[2];
        {
            std::lock_guard lock(g_ax_mutex);
            if (!g_initialized) continue;
            mix_voices();
            callback = g_frame_callback;
            callback2 = g_frame_callback2;
            final_mix[0] = g_final_mix_callbacks[0];
            final_mix[1] = g_final_mix_callbacks[1];
        }
        // Frame callbacks run as AX's interrupt handler would.
        KernelLock lock(kernel_mutex());
        acquire_interrupt_lock(lock, t);
        lock.unlock();
        if (callback) call_guest(t->ctx, callback, {});
        if (callback2) call_guest(t->ctx, callback2, {});
        lock.lock();
        bool held = false;
        release_interrupt_lock_for_wait(t, held);
        lock.unlock();
        // Final mix callbacks run on AX's own thread, outside the interrupt.
        run_final_mix(t, 0, final_mix[0], output);
        run_final_mix(t, 1, final_mix[1], nullptr);
        host::queue_audio(output, kOutputSamples);
    }
    return nullptr;
}

// ------------------------------------------------------------- exports

void AXInit() {
    std::lock_guard lock(g_ax_mutex);
    if (g_initialized) return;
    for (int i = 0; i < kVoiceCount; ++i) {
        g_voices[i] = Voice{};
        g_voices[i].vpb = system_alloc(kVpbSize, 32);
        field<uint32_t>(g_voices[i].vpb, 0x00) = static_cast<uint32_t>(i);
    }
    for (FinalMix& mix : g_final_mix) {
        const uint32_t buffers = mix.channels * mix.devices;
        mix.param = system_alloc(0x10, 32);
        mix.pointers = system_alloc(buffers * 4, 32);
        mix.samples = system_alloc(buffers * kOutputSamples * 4, 64);
    }
    g_initialized = true;
    g_ax_thread = create_internal_thread("cafe audio", 0);
    pthread_t host;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_create(&host, &attr, ax_main, nullptr);
    pthread_attr_destroy(&attr);
    pthread_detach(host);
}

void AXQuit() {
    std::lock_guard lock(g_ax_mutex);
    for (Voice& v : g_voices) v.running = false;
    g_frame_callback = g_frame_callback2 = 0;
}

bool AXIsInit() { return g_initialized; }

// Lowest priority value is dropped first; a free voice is used before any.
uint32_t AXAcquireVoice(PPCContext& ctx, uint32_t priority, uint32_t drop_callback, uint32_t user) {
    Voice* chosen = nullptr;
    uint32_t dropped_callback = 0, dropped_vpb = 0;
    {
        std::lock_guard lock(g_ax_mutex);
        for (Voice& v : g_voices) {
            if (!v.acquired) {
                chosen = &v;
                break;
            }
        }
        if (chosen == nullptr) {
            for (Voice& v : g_voices) {
                const uint32_t p = field<uint32_t>(v.vpb, 0x1C);
                if (p < priority && (chosen == nullptr || p < field<uint32_t>(chosen->vpb, 0x1C))) chosen = &v;
            }
            if (chosen == nullptr) return 0;
            dropped_callback = field<uint32_t>(chosen->vpb, 0x20);
            dropped_vpb = chosen->vpb;
        }
        const uint32_t vpb = chosen->vpb;
        const uint32_t index = field<uint32_t>(vpb, 0x00);
        *chosen = Voice{};
        chosen->vpb = vpb;
        chosen->acquired = true;
        std::memset(guest<uint8_t>(vpb), 0, kVpbSize);
        field<uint32_t>(vpb, 0x00) = index;
        field<uint32_t>(vpb, 0x1C) = priority;
        field<uint32_t>(vpb, 0x20) = drop_callback;
        field<uint32_t>(vpb, 0x24) = user;
    }
    AX_TRACE("acquire priority %u -> %08X\n", priority, chosen->vpb);
    if (dropped_callback) call_guest(ctx, dropped_callback, {dropped_vpb});
    return chosen->vpb;
}

void AXFreeVoice(uint32_t vpb) {
    std::lock_guard lock(g_ax_mutex);
    if (Voice* v = voice_of(vpb)) {
        v->acquired = false;
        v->running = false;
        publish_state(*v);
    }
}

void AXSetVoiceState(uint32_t vpb, uint32_t state) {
    std::lock_guard lock(g_ax_mutex);
    if (Voice* v = voice_of(vpb)) {
        v->running = state != 0;
        publish_state(*v);
    }
}

struct Offsets {
    be<uint16_t> format, loop_flag;
    be<uint32_t> loop_offset, end_offset, current_offset, samples;
};

void AXSetVoiceOffsets(uint32_t vpb, const Offsets* o) {
    std::lock_guard lock(g_ax_mutex);
    Voice* v = voice_of(vpb);
    if (v == nullptr || o == nullptr) return;
    AX_TRACE("offsets %08X format %u loop %u 0x%X..0x%X current 0x%X data %08X\n", vpb, uint32_t{o->format},
             uint32_t{o->loop_flag}, uint32_t{o->loop_offset}, uint32_t{o->end_offset}, uint32_t{o->current_offset},
             uint32_t{o->samples});
    v->format = o->format;
    v->loop = o->loop_flag != 0;
    v->samples = o->samples;
    v->loop_start = to_sample(*v, o->loop_offset);
    v->end = to_sample(*v, o->end_offset);
    v->cursor = to_sample(*v, o->current_offset);
    v->phase = 0;
    v->previous = v->current = 0;
    if (v->format != kPcm16 && v->format != kPcm8 && v->format != kAdpcm) {
        std::fprintf(stderr, "ttt2: AX: voice sample format 0x%X is not supported (silent)\n", v->format);
    }
    std::memcpy(guest<uint8_t>(vpb + 0x34), o, sizeof(Offsets));
    publish_state(*v);
}

void AXGetVoiceOffsets(uint32_t vpb, Offsets* o) {
    std::lock_guard lock(g_ax_mutex);
    Voice* v = voice_of(vpb);
    if (v == nullptr || o == nullptr) return;
    o->format = v->format;
    o->loop_flag = v->loop ? 1 : 0;
    o->loop_offset = from_sample(*v, v->loop_start);
    o->end_offset = from_sample(*v, v->end);
    o->current_offset = from_sample(*v, v->cursor);
    o->samples = v->samples;
}

bool AXCheckVoiceOffsets(const Offsets* o) {
    return o != nullptr && o->end_offset >= o->current_offset;
}

int32_t AXSetVoiceSrcRatio(uint32_t vpb, float ratio) {
    if (!(ratio >= 0.0f) || ratio > 255.0f) return -1;
    std::lock_guard lock(g_ax_mutex);
    if (Voice* v = voice_of(vpb)) v->ratio = static_cast<uint32_t>(ratio * 65536.0f);
    return 0;
}

// AXPBSRC: 16.16 resampling ratio, then fraction and filter history.
void AXSetVoiceSrc(uint32_t vpb, const be<uint16_t>* src) {
    std::lock_guard lock(g_ax_mutex);
    if (Voice* v = voice_of(vpb); v != nullptr && src != nullptr) v->ratio = (uint32_t{src[0]} << 16) | src[1];
}

// AXPBADPCM: 16 coefficients, gain, predictor/scale, yn1, yn2.
void AXSetVoiceAdpcm(uint32_t vpb, const be<int16_t>* adpcm) {
    std::lock_guard lock(g_ax_mutex);
    Voice* v = voice_of(vpb);
    if (v == nullptr || adpcm == nullptr) return;
    for (int i = 0; i < 16; ++i) v->coefs[i] = adpcm[i];
    v->pred_scale = static_cast<uint16_t>(int16_t{adpcm[17]});
    v->yn1 = adpcm[18];
    v->yn2 = adpcm[19];
}

// AXPBADPCMLOOP: the decoder state at the loop start.
void AXSetVoiceAdpcmLoop(uint32_t vpb, const be<int16_t>* loop) {
    std::lock_guard lock(g_ax_mutex);
    Voice* v = voice_of(vpb);
    if (v == nullptr || loop == nullptr) return;
    v->loop_pred_scale = static_cast<uint16_t>(int16_t{loop[0]});
    v->loop_yn1 = loop[1];
    v->loop_yn2 = loop[2];
}

// AXPBVE: volume (0x8000 = 1.0) and its change per 32 kHz sample.
struct Ve {
    be<uint16_t> volume;
    be<int16_t> delta;
};
void AXSetVoiceVe(uint32_t vpb, const Ve* ve) {
    if (ve) set_voice_ve(vpb, ve->volume, ve->delta);
}

// AXCHMIX: per channel, per bus (main, aux A, B, C): volume, delta.
int32_t AXSetVoiceDeviceMix(uint32_t vpb, uint32_t device, uint32_t index, const be<uint16_t>* mix) {
    if (mix == nullptr) return -3;
    std::lock_guard lock(g_ax_mutex);
    Voice* v = voice_of(vpb);
    if (v == nullptr) return -4;
    ChannelMix* target;
    int channels;
    if (device == 0 && index == 0) {
        target = &v->tv[0][0];
        channels = kTvChannels;
    } else if (device == 1 && index < 2) {
        target = &v->drc[index][0][0];
        channels = kDrcChannels;
    } else if (device == 2 && index < 4) {
        return 0; // Wii Remote speakers: none connected
    } else {
        return -2;
    }
    for (int i = 0; i < channels * kBuses; ++i) {
        target[i].volume = mix[2 * i] / 32768.0f;
        target[i].delta = static_cast<int16_t>(uint16_t{mix[2 * i + 1]}) / 32768.0f;
    }
    if (g_trace) {
        std::fprintf(stderr, "ax: device mix %08X device %u.%u main", vpb, device, index);
        for (int c = 0; c < channels; ++c) std::fprintf(stderr, " %.3f", target[c * kBuses].volume);
        std::fprintf(stderr, "\n");
    }
    return 0;
}

void AXSetVoiceSrcType(uint32_t, uint32_t) {}
void AXSetVoiceType(uint32_t, uint32_t) {}
void AXSetVoiceLpf(uint32_t, uint32_t) {}
void AXSetVoiceLpfCoefs(uint32_t, uint32_t, uint32_t) {}
void AXSetVoiceBiquad(uint32_t, uint32_t) {}
void AXSetVoiceBiquadCoefs(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}

// One-pole low-pass coefficients for a cutoff frequency at 32 kHz.
void AXComputeLpfCoefs(uint32_t frequency, be<uint16_t>* a0, be<uint16_t>* b0) {
    const double b = 2.0 - std::cos(2.0 * M_PI * frequency / 32000.0);
    const double c = b - std::sqrt(b * b - 1.0);
    if (a0) *a0 = static_cast<uint16_t>((1.0 - c) * 32768.0);
    if (b0) *b0 = static_cast<uint16_t>(c * 32768.0);
}

// AXVoiceBegin/End bracket edits of one voice against the frame update.
int32_t AXVoiceBegin(uint32_t) {
    g_ax_mutex.lock();
    return 0;
}
int32_t AXVoiceEnd(uint32_t) {
    g_ax_mutex.unlock();
    return 0;
}

uint32_t AXRegisterCallback(uint32_t callback) {
    std::lock_guard lock(g_ax_mutex);
    const uint32_t previous = g_frame_callback;
    g_frame_callback = callback;
    return previous;
}
uint32_t AXRegisterFrameCallback(uint32_t callback) {
    std::lock_guard lock(g_ax_mutex);
    const uint32_t previous = g_frame_callback2;
    g_frame_callback2 = callback;
    return previous;
}
// Effects on the aux buses are not run: no voice of this title sends to them.
uint32_t aux_register(int bus, uint32_t callback, uint32_t context) {
    std::lock_guard lock(g_ax_mutex);
    AX_TRACE("aux %c callback %08X context %08X\n", 'A' + bus, callback, context);
    const uint32_t previous = g_aux_callbacks[bus][0];
    g_aux_callbacks[bus][0] = callback;
    g_aux_callbacks[bus][1] = context;
    return previous;
}
uint32_t AXRegisterAuxACallback(uint32_t callback, uint32_t context) { return aux_register(0, callback, context); }
uint32_t AXRegisterAuxBCallback(uint32_t callback, uint32_t context) { return aux_register(1, callback, context); }
uint32_t AXRegisterAuxCCallback(uint32_t callback, uint32_t context) { return aux_register(2, callback, context); }
int32_t AXRegisterDeviceFinalMixCallback(uint32_t device, uint32_t callback) {
    if (device > 1) return -1;
    std::lock_guard lock(g_ax_mutex);
    g_final_mix_callbacks[device] = callback;
    return 0;
}

// TV and GamePad outputs, stereo (mode 0 = stereo for TV).
int32_t AXGetDeviceMode(uint32_t device, be<uint32_t>* mode) {
    if (mode) *mode = 0;
    return device <= 1 ? 0 : -1;
}
uint32_t AXGetMode() { return 0; }
int32_t AXSetDRCVSMode(uint32_t) { return 0; }
void AXSetMasterVolume(uint16_t volume) { g_master_volume = volume; }
uint16_t AXGetMasterVolume() { return g_master_volume; }
void AXSetAuxAReturnVolume(uint16_t) {}
void AXSetAuxBReturnVolume(uint16_t) {}
void AXSetAuxCReturnVolume(uint16_t) {}

} // namespace

void set_voice_ve(uint32_t vpb, uint16_t volume, int16_t delta) {
    std::lock_guard lock(g_ax_mutex);
    if (Voice* v = voice_of(vpb)) {
        v->ve = volume / 32768.0f;
        v->ve_delta = delta / 32768.0f;
    }
}

CAFE_EXPORT(snd_core, AXInit, AXInit);
CAFE_EXPORT(snd_core, AXQuit, AXQuit);
CAFE_EXPORT(snd_core, AXIsInit, AXIsInit);
CAFE_EXPORT(snd_core, AXAcquireVoice, AXAcquireVoice);
CAFE_EXPORT(snd_core, AXFreeVoice, AXFreeVoice);
CAFE_EXPORT(snd_core, AXSetVoiceState, AXSetVoiceState);
CAFE_EXPORT(snd_core, AXSetVoiceOffsets, AXSetVoiceOffsets);
CAFE_EXPORT(snd_core, AXGetVoiceOffsets, AXGetVoiceOffsets);
CAFE_EXPORT(snd_core, AXCheckVoiceOffsets, AXCheckVoiceOffsets);
CAFE_EXPORT(snd_core, AXSetVoiceSrcRatio, AXSetVoiceSrcRatio);
CAFE_EXPORT(snd_core, AXSetVoiceSrc, AXSetVoiceSrc);
CAFE_EXPORT(snd_core, AXSetVoiceAdpcm, AXSetVoiceAdpcm);
CAFE_EXPORT(snd_core, AXSetVoiceAdpcmLoop, AXSetVoiceAdpcmLoop);
CAFE_EXPORT(snd_core, AXSetVoiceVe, AXSetVoiceVe);
CAFE_EXPORT(snd_core, AXSetVoiceSrcType, AXSetVoiceSrcType);
CAFE_EXPORT(snd_core, AXSetVoiceType, AXSetVoiceType);
CAFE_EXPORT(snd_core, AXSetVoiceDeviceMix, AXSetVoiceDeviceMix);
CAFE_EXPORT(snd_core, AXSetVoiceLpf, AXSetVoiceLpf);
CAFE_EXPORT(snd_core, AXSetVoiceLpfCoefs, AXSetVoiceLpfCoefs);
CAFE_EXPORT(snd_core, AXSetVoiceBiquad, AXSetVoiceBiquad);
CAFE_EXPORT(snd_core, AXSetVoiceBiquadCoefs, AXSetVoiceBiquadCoefs);
CAFE_EXPORT(snd_core, AXComputeLpfCoefs, AXComputeLpfCoefs);
CAFE_EXPORT(snd_core, AXVoiceBegin, AXVoiceBegin);
CAFE_EXPORT(snd_core, AXVoiceEnd, AXVoiceEnd);
CAFE_EXPORT(snd_core, AXRegisterCallback, AXRegisterCallback);
CAFE_EXPORT(snd_core, AXRegisterFrameCallback, AXRegisterFrameCallback);
CAFE_EXPORT(snd_core, AXRegisterAuxACallback, AXRegisterAuxACallback);
CAFE_EXPORT(snd_core, AXRegisterAuxBCallback, AXRegisterAuxBCallback);
CAFE_EXPORT(snd_core, AXRegisterAuxCCallback, AXRegisterAuxCCallback);
CAFE_EXPORT(snd_core, AXRegisterDeviceFinalMixCallback, AXRegisterDeviceFinalMixCallback);
CAFE_EXPORT(snd_core, AXGetDeviceMode, AXGetDeviceMode);
CAFE_EXPORT(snd_core, AXGetMode, AXGetMode);
CAFE_EXPORT(snd_core, AXSetDRCVSMode, AXSetDRCVSMode);
CAFE_EXPORT(snd_core, AXSetMasterVolume, AXSetMasterVolume);
CAFE_EXPORT(snd_core, AXGetMasterVolume, AXGetMasterVolume);
CAFE_EXPORT(snd_core, AXSetAuxAReturnVolume, AXSetAuxAReturnVolume);
CAFE_EXPORT(snd_core, AXSetAuxBReturnVolume, AXSetAuxBReturnVolume);
CAFE_EXPORT(snd_core, AXSetAuxCReturnVolume, AXSetAuxCReturnVolume);

} // namespace cafe::os
