// Controllers: the Wii U GamePad (vpad), its state from the host (set_gamepad_state, fed by the
// window layer), and Wii U Pro Controllers (padscore: KPAD/WPAD, set_pro_pad_state). No Wii
// Remotes or GameCube controllers are connected.

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/input.h"
#include "cafe/vfs.h"

#include <array>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace cafe::os {
namespace {

std::mutex g_input_mutex;
GamepadState g_gamepad{};
uint32_t g_previous_hold = 0;

// TTT2_INPUT_SCRIPT="15:plus,20.5:a,22:down" presses each button at the given
// second after start-up, for unattended runs: for a fifth of a second and at
// least kHeldReads reads of the pad, then released for kReleasedReads reads
// before the same button is pressed again, so a press is seen even when a frame
// stalls (loading). An item "@3dbb1771" waits until the title reads the archive
// entry with that name hash (the character select screen's layout, say): the
// times after it count from then, and presses before it that have not come yet
// are dropped, so a script does not drift with loading times.
// "40:stick=-1/0.5/0.8" holds the left stick at x -1, y 0.5 for 0.8 s from
// second 40 (same segments as presses), for menus driven by a cursor. "18:b=1.5" holds B 1.5 s.
constexpr int kHeldReads = 4;
constexpr int kReleasedReads = 4;

struct ScriptedPress {
    double at;
    uint32_t button;
    size_t segment; // the anchors before it
    double hold = 0.2; // "15:b=1.5" holds B for 1.5 s
    int reads = -1; // reads of the pad while held; -1: not pressed yet
};

struct StickHold {
    double at, x, y, duration;
    size_t segment;
};

struct Script {
    std::vector<ScriptedPress> presses;
    std::vector<StickHold> sticks;
    std::vector<uint32_t> anchors;
    std::mutex mutex;
    std::vector<double> starts; // each segment's start, after start-up; < 0 while waiting
    std::vector<std::pair<uint32_t, int>> released; // button -> reads since its last scripted release
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

Script& load_script() {
    static Script script;
    static bool parsed = false; // several readers share one script
    if (parsed) return script;
    parsed = true;
    const char* text_env = std::getenv("TTT2_INPUT_SCRIPT");
    if (text_env == nullptr) return script;
    static const std::pair<const char*, uint32_t> kNames[] = {
        {"a", vpad::kA}, {"b", vpad::kB}, {"x", vpad::kX}, {"y", vpad::kY}, {"left", vpad::kLeft},
        {"right", vpad::kRight}, {"up", vpad::kUp}, {"down", vpad::kDown}, {"zl", vpad::kZL}, {"zr", vpad::kZR},
        {"l", vpad::kL}, {"r", vpad::kR}, {"plus", vpad::kPlus}, {"minus", vpad::kMinus}, {"home", vpad::kHome},
    };
    std::string text(text_env);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find(',', pos) == std::string::npos ? text.size() : text.find(',', pos);
        const std::string item = text.substr(pos, end - pos);
        const size_t colon = item.find(':');
        if (!item.empty() && item[0] == '@') {
            script.anchors.push_back(static_cast<uint32_t>(std::strtoul(item.c_str() + 1, nullptr, 16)));
        } else if (colon != std::string::npos) {
            const std::string name = item.substr(colon + 1);
            if (name.rfind("stick=", 0) == 0) {
                double x = 0, y = 0, duration = 0.2;
                std::sscanf(name.c_str() + 6, "%lf/%lf/%lf", &x, &y, &duration);
                script.sticks.push_back({std::atof(item.substr(0, colon).c_str()), x, y, duration, script.anchors.size()});
            }
            const size_t equals = name.find('=');
            for (const auto& [n, bit] : kNames) {
                if (name.substr(0, equals) == n) {
                    script.presses.push_back({std::atof(item.substr(0, colon).c_str()), bit, script.anchors.size()});
                    if (equals != std::string::npos) script.presses.back().hold = std::atof(name.c_str() + equals + 1);
                }
            }
        }
        pos = end + 1;
    }
    script.starts.assign(script.anchors.size() + 1, -1.0);
    script.starts[0] = 0.0;
    if (!script.anchors.empty()) {
        vfs::watch_entry_reads([&s = script](uint32_t hash) {
            std::lock_guard lock(s.mutex);
            for (size_t k = 0; k < s.anchors.size(); ++k) {
                if (s.starts[k + 1] >= 0.0) continue;
                if (s.starts[k] >= 0.0 && s.anchors[k] == hash) s.starts[k + 1] = seconds_since(s.start);
                break;
            }
        });
    }
    return script;
}

uint32_t scripted_buttons() {
    static Script& script = load_script();
    if (script.presses.empty()) return 0;
    const double t = seconds_since(script.start);
    std::lock_guard lock(script.mutex);
    auto released_reads = [&](uint32_t button) -> int& {
        for (auto& [b, n] : script.released) {
            if (b == button) return n;
        }
        script.released.push_back({button, kReleasedReads});
        return script.released.back().second;
    };
    uint32_t buttons = 0;
    for (ScriptedPress& p : script.presses) {
        if (p.reads >= kHeldReads) continue; // done
        const double start = script.starts[p.segment];
        const bool next_reached = p.segment + 1 < script.starts.size() && script.starts[p.segment + 1] >= 0.0;
        if (p.reads < 0) {
            if (start < 0.0 || next_reached || t < start + p.at) continue;
            if (released_reads(p.button) < kReleasedReads || (buttons & p.button)) continue; // wait for a release
            p.reads = 0;
        }
        buttons |= p.button;
        if (++p.reads >= kHeldReads && t < start + p.at + p.hold) p.reads = kHeldReads - 1; // hold it that long
        if (p.reads >= kHeldReads) released_reads(p.button) = -1;
    }
    for (auto& [b, n] : script.released) {
        if (!(buttons & b) && n < kReleasedReads) ++n;
    }
    return buttons;
}

// The scripted left stick, if a hold is under way.
bool scripted_stick(float& x, float& y) {
    static Script& script = load_script();
    if (script.sticks.empty()) return false;
    const double t = seconds_since(script.start);
    std::lock_guard lock(script.mutex);
    for (const StickHold& h : script.sticks) {
        const double start = script.starts[h.segment];
        if (start < 0.0 || t < start + h.at || t >= start + h.at + h.duration) continue;
        x = static_cast<float>(h.x);
        y = static_cast<float>(h.y);
        return true;
    }
    return false;
}

// ------------------------------------------------------------ the + chord
// See arm_start_chord. A press of + waits kChordFrames armings (the mode's
// frames, so slow frames wait longer): the mode sees a button two or three
// frames after the pad has it, and the other button may come a little after
// +. A press of + let go meanwhile still reaches the game, late, for
// kHeldReads reads.
using Clock = std::chrono::steady_clock;
constexpr uint32_t kChordFrames = 8;                         // how many armings a press of + waits for the partner
constexpr auto kChordArmed = std::chrono::milliseconds(250); // how long an arming lasts (frames may be slow)
constexpr auto kChordTaken = std::chrono::milliseconds(500); // how long a chord waits to be taken

struct StartChord {
    enum Gate { kOpen, kWaiting, kChord } gate = kOpen;
    Clock::time_point armed_until{}, chord_at{};
    uint32_t armings = 0, pressed_armings = 0; // armings so far, and at the press of +
    bool partner = false;
    bool raw = false;     // + held, as the host has it
    bool chord = false;   // a chord not taken yet
    int late_reads = 0;
};
StartChord g_chord;

uint32_t gate_start(uint32_t hold) {
    StartChord& c = g_chord;
    const Clock::time_point now = Clock::now();
    const bool raw = hold & vpad::kPlus, pressed = raw && !c.raw, armed = now < c.armed_until;
    c.raw = raw;
    auto chord = [&] {
        c.gate = StartChord::kChord;
        c.chord = true;
        c.chord_at = now;
    };
    if (pressed) {
        if (!armed) c.gate = StartChord::kOpen;
        else if (c.partner) chord();
        else {
            c.gate = StartChord::kWaiting;
            c.pressed_armings = c.armings;
        }
    } else if (c.gate == StartChord::kWaiting) {
        if (armed && c.partner) {
            chord();
        } else if (!armed || c.armings - c.pressed_armings >= kChordFrames) {
            c.gate = StartChord::kOpen;
            if (!raw) c.late_reads = kHeldReads;
        }
    }
    switch (c.gate) {
    case StartChord::kWaiting: return hold & ~vpad::kPlus;
    case StartChord::kChord:
        if (!raw) c.gate = StartChord::kOpen;
        return hold & ~vpad::kPlus;
    case StartChord::kOpen: break;
    }
    if (c.late_reads > 0) {
        --c.late_reads;
        return hold | vpad::kPlus;
    }
    return hold;
}

// ------------------------------------------------- recording and replay
// TTT2_INPUT_RECORD=<file> writes the pad as the title reads it: a line
// "read buttons left_x left_y right_x right_y" whenever it changes, `read`
// counting the title's VPADRead calls. TTT2_INPUT_REPLAY=<file> plays such a
// file back exactly, read by read, in place of the pad (so loading, when
// the title does not read the pad, cannot put it out of step); the pad
// works again after its last line.
const auto g_process_start = std::chrono::steady_clock::now();

struct PadLine {
    uint64_t read;
    GamepadState state;
};

struct Recording {
    FILE* out = nullptr;
    GamepadState last{};
    bool written = false;
    std::vector<PadLine> replay;
    size_t next = 0;
    uint64_t reads = 0;
};

Recording& recording() {
    static Recording r = [] {
        Recording made;
        if (const char* path = std::getenv("TTT2_INPUT_RECORD"); path && *path) made.out = std::fopen(path, "w");
        if (const char* path = std::getenv("TTT2_INPUT_REPLAY"); path && *path) {
            if (FILE* in = std::fopen(path, "r")) {
                PadLine line{};
                unsigned long long read = 0;
                unsigned buttons = 0;
                while (std::fscanf(in, "%llu %x %f %f %f %f", &read, &buttons, &line.state.left_x, &line.state.left_y,
                                   &line.state.right_x, &line.state.right_y) == 6) {
                    line.read = read;
                    line.state.buttons = buttons;
                    made.replay.push_back(line);
                }
                std::fclose(in);
                std::fprintf(stderr, "ttt2: input: replaying %zu changes from %s\n", made.replay.size(), path);
            }
        }
        return made;
    }();
    return r;
}

// The pad for this read: the replay's while it lasts, else `pad`, which is
// recorded if asked.
GamepadState pad_for_read(const GamepadState& pad) {
    Recording& r = recording();
    const uint64_t read = r.reads++;
    if (!r.replay.empty() && read == r.replay.back().read + 1) {
        std::fprintf(stderr, "ttt2: input: replay finished at read %llu, %.2f s after start-up\n",
                     static_cast<unsigned long long>(read),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - g_process_start).count());
    }
    if (!r.replay.empty() && read <= r.replay.back().read + 1) {
        while (r.next + 1 < r.replay.size() && r.replay[r.next + 1].read <= read) ++r.next;
        return r.replay[r.next].read <= read ? r.replay[r.next].state : GamepadState{};
    }
    if (r.out != nullptr) {
        const GamepadState& l = r.last;
        if (!r.written || pad.buttons != l.buttons || pad.left_x != l.left_x || pad.left_y != l.left_y ||
            pad.right_x != l.right_x || pad.right_y != l.right_y) {
            std::fprintf(r.out, "%llu %x %.4f %.4f %.4f %.4f\n", static_cast<unsigned long long>(read), pad.buttons,
                         pad.left_x, pad.left_y, pad.right_x, pad.right_y);
            std::fflush(r.out);
            r.last = pad;
            r.written = true;
        }
    }
    return pad;
}

// ------------------------------------------------------------------- vpad
constexpr int32_t kVpadOk = 0;
constexpr int32_t kVpadNoController = -2;

struct Vec2 { be<float> x, y; };
struct Vec3 { be<float> x, y, z; };
struct TouchData { be<uint16_t> x, y, touched, validity; };
struct VPADStatus {
    be<uint32_t> hold, trigger, release;
    Vec2 left_stick, right_stick;
    Vec3 acceleration;
    be<float> acc_magnitude, acc_variation;
    Vec2 acc_xy;
    Vec3 gyro, angle;
    int8_t error;
    uint8_t pad1;
    TouchData touch, touch_processed1, touch_processed2;
    uint8_t pad2[2];
    Vec3 direction[3];
    uint8_t headphone;
    uint8_t pad3[3];
    Vec3 magnet;
    uint8_t volume, battery, mic, volume_ex;
    uint8_t pad4[8];
};
static_assert(sizeof(VPADStatus) == 0xAC);

void VPADInit() {}

void deliver_connects();

int32_t VPADRead(int32_t channel, VPADStatus* status, uint32_t count, be<int32_t>* error) {
    deliver_connects();
    if (channel != 0 || count == 0 || status == nullptr) {
        if (error) *error = kVpadNoController;
        return 0;
    }
    std::lock_guard lock(g_input_mutex);
    std::memset(status, 0, sizeof(VPADStatus));
    const GamepadState pad = pad_for_read(g_gamepad);
    const uint32_t hold = gate_start(pad.buttons | scripted_buttons());
    status->hold = hold;
    status->trigger = hold & ~g_previous_hold;
    status->release = g_previous_hold & ~hold;
    g_previous_hold = hold;
    float left_x = pad.left_x, left_y = pad.left_y;
    scripted_stick(left_x, left_y);
    status->left_stick = {left_x, left_y};
    status->right_stick = {pad.right_x, pad.right_y};
    // A GamePad lying flat and still: gravity on -Z (in g), identity frame.
    status->acceleration = {0.0f, 0.0f, -1.0f};
    status->acc_magnitude = 1.0f;
    status->direction[0] = {1.0f, 0.0f, 0.0f};
    status->direction[1] = {0.0f, 1.0f, 0.0f};
    status->direction[2] = {0.0f, 0.0f, 1.0f};
    status->battery = 0xC0;
    status->volume = 0xFF;
    status->volume_ex = 0xFF;
    if (error) *error = kVpadOk;
    return 1;
}

void VPADSetBtnRepeat(int32_t, float, float) {}
void VPADSetAccParam(int32_t, float, float) {}
int32_t VPADControlMotor(int32_t, uint32_t, uint32_t) { return 0; }
void VPADStopMotor(int32_t) {}

// Touch coordinates come in raw; calibrated output is the 1280x720 screen.
void VPADGetTPCalibratedPoint(int32_t, TouchData* out, const TouchData* in) {
    if (out && in) *out = *in;
}

// --------------------------------------------------------------- padscore
// Wii U Pro Controllers on channels 0-6, from the host (set_pro_pad_state). The title learns of
// one through its connect callback, run from its next VPADRead (the hardware runs it from the
// controller library). WPADDisconnect drops one until it presses a button, as on hardware.
constexpr int32_t kWpadNoController = -1;
constexpr uint8_t kExtProController = 0x1F, kNoDevice = 0xFD;

struct ProChannel {
    ProPadState host{};     // as the host has it
    bool connected = false; // as the title has it
    bool dropped = false;   // the title disconnected it
    uint32_t previous = 0;
    uint32_t callback = 0;
};
ProChannel g_pro[pro::kChannels];
std::vector<std::pair<int32_t, int32_t>> g_pending_connects; // channel, status (0 connected, -1 gone)

// The connect callbacks due, on the title's thread that reads the GamePad.
void deliver_connects() {
    std::vector<std::array<uint32_t, 3>> calls;
    {
        std::lock_guard lock(g_input_mutex);
        std::erase_if(g_pending_connects, [&](const std::pair<int32_t, int32_t>& p) {
            const uint32_t callback = g_pro[p.first].callback;
            if (callback == 0) return false; // until the title sets one
            calls.push_back({callback, static_cast<uint32_t>(p.first), static_cast<uint32_t>(p.second)});
            return true;
        });
    }
    for (const auto& [callback, channel, status] : calls) call_guest(current_thread()->ctx, callback, {channel, status});
}

struct KPADStatus {
    be<uint32_t> hold, trigger, release;
    uint8_t motion[0x50]; // accelerometer and pointer: a Pro Controller has none
    uint8_t extension_type;
    int8_t error, pos_valid;
    uint8_t format;
    be<uint32_t> pro_hold, pro_trigger, pro_release;
    Vec2 left_stick, right_stick;
    be<int32_t> charging, wired;
    uint8_t rest[0x6C];
};
static_assert(sizeof(KPADStatus) == 0xF0);

uint32_t g_wpad_alloc = 0, g_wpad_free = 0;

void WPADRegisterAllocator(uint32_t alloc, uint32_t free) {
    g_wpad_alloc = alloc;
    g_wpad_free = free;
}
void KPADInit() {}
void KPADShutdown() {}
uint32_t KPADSetConnectCallback(uint32_t channel, uint32_t callback) {
    if (channel >= pro::kChannels) return 0;
    std::lock_guard lock(g_input_mutex);
    const uint32_t previous = g_pro[channel].callback;
    g_pro[channel].callback = callback;
    return previous;
}
int32_t KPADReadEx(int32_t channel, KPADStatus* status, uint32_t count, be<int32_t>* error) {
    std::lock_guard lock(g_input_mutex);
    if (channel < 0 || channel >= pro::kChannels || !g_pro[channel].connected || status == nullptr || count == 0) {
        if (error) *error = kWpadNoController;
        return 0;
    }
    ProChannel& c = g_pro[channel];
    std::memset(status, 0, sizeof(KPADStatus));
    status->extension_type = kExtProController;
    status->pro_hold = c.host.buttons;
    status->pro_trigger = c.host.buttons & ~c.previous;
    status->pro_release = c.previous & ~c.host.buttons;
    c.previous = c.host.buttons;
    status->left_stick = {c.host.left_x, c.host.left_y};
    status->right_stick = {c.host.right_x, c.host.right_y};
    if (error) *error = 0;
    return 1;
}
int32_t WPADProbe(int32_t channel, be<uint32_t>* type) {
    std::lock_guard lock(g_input_mutex);
    const bool connected = channel >= 0 && channel < pro::kChannels && g_pro[channel].connected;
    if (type) *type = connected ? kExtProController : kNoDevice;
    return connected ? 0 : kWpadNoController;
}
void WPADRead(int32_t, uint8_t* status) {
    if (status) std::memset(status, 0, 0x2A);
}
int32_t WPADSetDataFormat(int32_t, uint32_t) { return kWpadNoController; }
uint32_t WPADGetLatestIndexInBuf(int32_t) { return 0; }
void WPADSetAutoSamplingBuf(int32_t, uint32_t, uint32_t) {}
void WPADControlMotor(int32_t, uint32_t) {}
bool WPADIsMotorEnabled() { return true; }
int32_t WPADEnableURCC(int32_t) { return 0; }
void WPADDisconnect(int32_t channel) {
    if (channel < 0 || channel >= pro::kChannels) return;
    std::lock_guard lock(g_input_mutex);
    ProChannel& c = g_pro[channel];
    if (!c.connected) return;
    c.connected = false;
    c.dropped = true;
    g_pending_connects.push_back({channel, kWpadNoController});
}
int32_t WPADControlDpd(int32_t, uint32_t, uint32_t) { return kWpadNoController; }
void KPADEnableDPD(int32_t) {}
void KPADDisableDPD(int32_t) {}
void WPADClampStick(int32_t, uint32_t, uint32_t) {}
void WPADClampTrigger(int32_t, uint32_t, uint32_t) {}

} // namespace

void set_gamepad_state(const GamepadState& state) {
    std::lock_guard lock(g_input_mutex);
    g_gamepad = state;
}

void set_pro_pad_state(int channel, const ProPadState& state) {
    if (channel < 0 || channel >= pro::kChannels) return;
    std::lock_guard lock(g_input_mutex);
    ProChannel& c = g_pro[channel];
    c.host = state;
    if (!state.connected) {
        c.dropped = false;
        if (c.connected) g_pending_connects.push_back({channel, kWpadNoController});
        c.connected = false;
    } else if (!c.connected && (!c.dropped || state.buttons != 0)) {
        c.connected = true;
        c.dropped = false;
        c.previous = state.buttons; // the press that reconnects it is not the title's
        g_pending_connects.push_back({channel, 0});
    }
}

bool pro_controllers_in_use() {
    std::lock_guard lock(g_input_mutex);
    for (const ProChannel& c : g_pro) {
        if (c.callback != 0) return true;
    }
    return false;
}

void arm_start_chord(bool partner_held) {
    std::lock_guard lock(g_input_mutex);
    g_chord.armed_until = Clock::now() + kChordArmed;
    g_chord.partner = partner_held;
    ++g_chord.armings;
}

bool take_start_chord() {
    std::lock_guard lock(g_input_mutex);
    const bool chord = g_chord.chord && Clock::now() - g_chord.chord_at < kChordTaken;
    g_chord.chord = false;
    return chord;
}

CAFE_EXPORT(vpad, VPADInit, VPADInit);
CAFE_EXPORT(vpad, VPADRead, VPADRead);
CAFE_EXPORT(vpad, VPADSetBtnRepeat, VPADSetBtnRepeat);
CAFE_EXPORT(vpad, VPADSetAccParam, VPADSetAccParam);
CAFE_EXPORT(vpad, VPADControlMotor, VPADControlMotor);
CAFE_EXPORT(vpad, VPADStopMotor, VPADStopMotor);
CAFE_EXPORT(vpad, VPADGetTPCalibratedPoint, VPADGetTPCalibratedPoint);

CAFE_EXPORT(padscore, WPADRegisterAllocator, WPADRegisterAllocator);
CAFE_EXPORT(padscore, KPADInit, KPADInit);
CAFE_EXPORT(padscore, KPADShutdown, KPADShutdown);
CAFE_EXPORT(padscore, KPADSetConnectCallback, KPADSetConnectCallback);
CAFE_EXPORT(padscore, KPADReadEx, KPADReadEx);
CAFE_EXPORT(padscore, WPADProbe, WPADProbe);
CAFE_EXPORT(padscore, WPADRead, WPADRead);
CAFE_EXPORT(padscore, WPADSetDataFormat, WPADSetDataFormat);
CAFE_EXPORT(padscore, WPADGetLatestIndexInBuf, WPADGetLatestIndexInBuf);
CAFE_EXPORT(padscore, WPADSetAutoSamplingBuf, WPADSetAutoSamplingBuf);
CAFE_EXPORT(padscore, WPADControlMotor, WPADControlMotor);
CAFE_EXPORT(padscore, WPADIsMotorEnabled, WPADIsMotorEnabled);
CAFE_EXPORT(padscore, WPADEnableURCC, WPADEnableURCC);
CAFE_EXPORT(padscore, WPADDisconnect, WPADDisconnect);
CAFE_EXPORT(padscore, WPADControlDpd, WPADControlDpd);
CAFE_EXPORT(padscore, KPADEnableDPD, KPADEnableDPD);
CAFE_EXPORT(padscore, KPADDisableDPD, KPADDisableDPD);
CAFE_EXPORT(padscore, WPADClampStick, WPADClampStick);
CAFE_EXPORT(padscore, WPADClampTrigger, WPADClampTrigger);

} // namespace cafe::os
