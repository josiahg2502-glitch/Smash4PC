// snd_user: sound-effect processors (AXFX reverb/chorus/delay), the MIX
// channel mixer and AXART articulation. The effects are modelled for the
// title's control flow only: set-up succeeds and memory sizes are reported;
// AX does not run aux buses (the title sends nothing to them), so their
// callbacks are never called. Generated from the title's import list.

#include "ax.h"

#include "cafe/export.h"
#include "cafe/guest.h"

#include <cmath>
#include <mutex>

namespace cafe::os {

// Working memory an effect asks the title to allocate for it.
constexpr uint32_t kEffectMemory = 0x100;

CAFE_EXPORT_RAW(snd_user, AXARTInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXARTQuit) { ctx.r[3] = 0; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusExpShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXChorusShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayExpShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelaySettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXDelayShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiExpShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbHiShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpCallback) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpCallbackDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpGetMemSizeDpl2) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpInitDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpSettingsDpl2) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpShutdown) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdExpShutdownDpl2) { (void)ctx; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdGetMemSize) { ctx.r[3] = kEffectMemory; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdInit) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdSettings) { ctx.r[3] = 1; }
CAFE_EXPORT_RAW(snd_user, AXFXReverbStdShutdown) { (void)ctx; }
// ------------------------------------------------------------------ MIX
// The channel mixer drives voice volumes from input levels in 0.1 dB
// (-90.4 dB and below is silence). The title assigns its voices, sets their
// input level and calls MIXUpdateSettings every audio frame; it programs
// device mixes (pan, fader) directly with AXSetVoiceDeviceMix, so only the
// input-level path is modelled. A level change ramps over one frame.

namespace {

constexpr uint32_t kMixInputChanged = 0x10000000; // input level set
constexpr uint32_t kMixRamping = 0x20000000;      // VE ramping to the target
constexpr uint32_t kMixMute = 0x8;

struct MixChannel {
    uint32_t voice = 0;
    uint32_t mode = 0;
    int16_t input = 0;
    uint16_t volume = 0, target = 0;
};

std::mutex g_mix_mutex;
MixChannel g_mix_channels[96];
bool g_mix_initialized = false;

// 0.1 dB steps to AX volume (0x8000 = 0 dB), as MIX's table has them.
uint16_t translate_volume(int32_t level) {
    if (level <= -904) return 0;
    if (level >= 60) return 0xFF64;
    if (level == 0) return 0x7FFF;
    return static_cast<uint16_t>(32768.0 * std::pow(10.0, level / 200.0));
}

MixChannel* mix_channel(uint32_t voice) {
    const uint32_t index = voice ? uint32_t{*guest<be<uint32_t>>(voice)} : ~0u;
    return index < 96 ? &g_mix_channels[index] : nullptr;
}

void MIXInit() {
    std::lock_guard lock(g_mix_mutex);
    if (g_mix_initialized) return;
    for (MixChannel& c : g_mix_channels) c = MixChannel{};
    g_mix_initialized = true;
}

void MIXQuit() {
    std::lock_guard lock(g_mix_mutex);
    g_mix_initialized = false;
}

void MIXAssignChannel(uint32_t voice) {
    std::lock_guard lock(g_mix_mutex);
    if (MixChannel* c = mix_channel(voice)) {
        *c = MixChannel{};
        c->voice = voice;
    }
}

void MIXReleaseChannel(uint32_t voice) {
    std::lock_guard lock(g_mix_mutex);
    if (MixChannel* c = mix_channel(voice)) c->voice = 0;
}

void MIXInitInputControl(uint32_t voice, int16_t input, uint32_t mode) {
    std::lock_guard lock(g_mix_mutex);
    if (MixChannel* c = mix_channel(voice)) {
        c->mode = (mode & kMixMute) | kMixInputChanged;
        c->input = input;
    }
}

void MIXUpdateSettings() {
    std::lock_guard lock(g_mix_mutex);
    if (!g_mix_initialized) return;
    for (MixChannel& c : g_mix_channels) {
        if (c.voice == 0) continue;
        bool settled = false;
        if (c.mode & kMixRamping) {
            c.volume = c.target;
            c.mode &= ~kMixRamping;
            settled = true;
        }
        if (c.mode & kMixInputChanged) {
            c.target = (c.mode & kMixMute) ? 0 : translate_volume(c.input);
            c.mode = (c.mode & ~kMixInputChanged) | kMixRamping;
        } else if (!settled) {
            continue;
        }
        set_voice_ve(c.voice, c.volume, static_cast<int16_t>((int32_t{c.target} - int32_t{c.volume}) / 96));
    }
}

void MIXSetDeviceSoundMode(uint32_t, uint32_t) {}

} // namespace

CAFE_EXPORT(snd_user, MIXInit, MIXInit);
CAFE_EXPORT(snd_user, MIXQuit, MIXQuit);
CAFE_EXPORT(snd_user, MIXAssignChannel, MIXAssignChannel);
CAFE_EXPORT(snd_user, MIXReleaseChannel, MIXReleaseChannel);
CAFE_EXPORT(snd_user, MIXInitInputControl, MIXInitInputControl);
CAFE_EXPORT(snd_user, MIXUpdateSettings, MIXUpdateSettings);
CAFE_EXPORT(snd_user, MIXSetDeviceSoundMode, MIXSetDeviceSoundMode);

} // namespace cafe::os
