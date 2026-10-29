#include "host/window.h"

#include "cafe/input.h"
#include "cafe/runtime.h"
#include "host/display_settings.h"
#include "host/keymap.h"
#include "host/netplay.h"
#include "gpu/gpu.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace cafe::os {
void request_profile(); // os/thread.cpp
} // namespace cafe::os

namespace cafe::host {
namespace {

SDL_Window* g_window = nullptr;
std::atomic<uint32_t> g_width{1280}, g_height{720};
std::atomic<uint32_t> g_display_width{1280}, g_display_height{720};
std::atomic<float> g_display_refresh{60.0f};
std::atomic<float> g_window_refresh{60.0f};

// A display's current refresh rate, as exactly as SDL knows it.
float refresh_of(SDL_DisplayID display) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (mode == nullptr) return 0.0f;
    if (mode->refresh_rate_denominator) {
        return static_cast<float>(double(mode->refresh_rate_numerator) / mode->refresh_rate_denominator);
    }
    return mode->refresh_rate;
}

// The window's display: it moves with the window (and its mode may change).
void update_window_refresh() {
    const SDL_DisplayID display = g_window ? SDL_GetDisplayForWindow(g_window) : 0;
    const float hz = display ? refresh_of(display) : 0.0f;
    if (hz > 0.0f) g_window_refresh = hz;
}

// The keyboard's keys (host/keymap.h): the player's from keyboard.ini, read
// again when the launcher's Settings change it, so new keys apply at once.
// Main thread only.
keymap::Keys g_keymap = keymap::default_keys();
std::optional<keymap::Watcher> g_keymap_file;

// The keymap's numbers are SDL's scancodes: the defaults are these keys.
static_assert(keymap::kKeyCount == SDL_SCANCODE_COUNT);
constexpr SDL_Scancode kDefaultKeys[] = {
    SDL_SCANCODE_W,      SDL_SCANCODE_S,         SDL_SCANCODE_A,    SDL_SCANCODE_D,     // D-pad
    SDL_SCANCODE_I,      SDL_SCANCODE_K,         SDL_SCANCODE_J,    SDL_SCANCODE_L,     // X, B, Y, A
    SDL_SCANCODE_H,      SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_U,    SDL_SCANCODE_O,     // L, R, ZL, ZR
    SDL_SCANCODE_UP,     SDL_SCANCODE_DOWN,      SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, // left stick
    SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_HOME,                     // +, -, HOME
};
static_assert([] {
    for (size_t i = 0; i < keymap::kInputCount; ++i) {
        if (keymap::kInputs[i].default_key != kDefaultKeys[i]) return false;
    }
    return std::size(kDefaultKeys) == keymap::kInputCount;
}());
static_assert(keymap::kReserved ==
              std::array<uint16_t, 4>{SDL_SCANCODE_F11, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI});

void reload_keymap() {
    if (!g_keymap_file) return;
    const std::optional<keymap::Loaded> loaded = g_keymap_file->reload();
    if (!loaded) return;
    for (const std::string& line : loaded->log) std::fprintf(stderr, "ttt2: %s\n", line.c_str());
    g_keymap = loaded->keys;
}

// Gamepads by button position: the Wii U's A is on the right, B at the bottom. The same
// buttons as the GamePad's and as a Pro Controller's.
struct PadButton {
    SDL_GamepadButton button;
    uint32_t vpad, pro;
};
constexpr PadButton kPadButtons[] = {
    {SDL_GAMEPAD_BUTTON_EAST, os::vpad::kA, os::pro::kA},
    {SDL_GAMEPAD_BUTTON_SOUTH, os::vpad::kB, os::pro::kB},
    {SDL_GAMEPAD_BUTTON_NORTH, os::vpad::kX, os::pro::kX},
    {SDL_GAMEPAD_BUTTON_WEST, os::vpad::kY, os::pro::kY},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, os::vpad::kUp, os::pro::kUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, os::vpad::kDown, os::pro::kDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, os::vpad::kLeft, os::pro::kLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, os::vpad::kRight, os::pro::kRight},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, os::vpad::kL, os::pro::kL},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, os::vpad::kR, os::pro::kR},
    {SDL_GAMEPAD_BUTTON_START, os::vpad::kPlus, os::pro::kPlus},
    {SDL_GAMEPAD_BUTTON_BACK, os::vpad::kMinus, os::pro::kMinus},
    {SDL_GAMEPAD_BUTTON_GUIDE, os::vpad::kHome, os::pro::kHome},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, os::vpad::kStickL, os::pro::kStickL},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, os::vpad::kStickR, os::pro::kStickR},
};

// ------------------------------------------------------------ controller kind

std::atomic<Controller> g_controller{Controller::kOther};
bool g_controller_fixed = false;
bool g_controller_used = false; // a button or stick moved: added pads no longer count
std::function<void(Controller)> g_controller_callback;

Controller kind_of(SDL_JoystickID id) {
    switch (SDL_GetGamepadTypeForID(id)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return Controller::kPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return Controller::kNintendo;
    default: return Controller::kOther;
    }
}

// The keyboard, as the window's key events tell it. Steam controllers also
// type (Steam's desktop layout, or the controller's own keyboard mode: A is
// Enter, which is the Wii U's Plus here, opening the pause menu while the
// same press is the gamepad's button): keys from a keyboard whose name says
// it is a controller's are left out.
std::array<bool, SDL_SCANCODE_COUNT> g_keys{};

bool controller_keyboard(SDL_KeyboardID id) {
    const char* name = id ? SDL_GetKeyboardNameForID(id) : nullptr;
    if (name == nullptr) return false;
    const std::string n = name;
    return n.find("Steam Controller") != std::string::npos || n.find("Steam Deck") != std::string::npos ||
           n.find("Valve Software") != std::string::npos;
}

void note_key(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event.key.scancode < SDL_SCANCODE_COUNT && !controller_keyboard(event.key.which)) {
            g_keys[event.key.scancode] = event.type == SDL_EVENT_KEY_DOWN;
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: g_keys.fill(false); break;
    default: break;
    }
}

void set_controller(Controller kind) {
    if (g_controller_fixed || g_controller.exchange(kind) == kind) return;
    if (g_controller_callback) g_controller_callback(kind);
}

// Which controller the player picked up: the one with the latest input; a
// pad plugged in counts until anything is pressed.
void note_controller(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        std::fprintf(stderr, "ttt2: input: pad connected: %s\n", SDL_GetGamepadNameForID(event.gdevice.which));
        if (!g_controller_used) set_controller(kind_of(event.gdevice.which));
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        g_controller_used = true;
        set_controller(kind_of(event.gbutton.which));
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(event.gaxis.value) > 16000) {
            g_controller_used = true;
            set_controller(kind_of(event.gaxis.which));
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (controller_keyboard(event.key.which)) break;
        g_controller_used = true;
        set_controller(Controller::kOther);
        break;
    default: break;
    }
}

float axis(SDL_Gamepad* pad, SDL_GamepadAxis a) {
    const float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
    return std::fabs(v) < 0.15f ? 0.0f : std::clamp(v, -1.0f, 1.0f);
}

OverlayHooks g_overlay;
std::atomic<bool> g_quit_requested{false};

os::ProPadState pro_pad(SDL_Gamepad* pad) {
    os::ProPadState s{true};
    for (const PadButton& b : kPadButtons) {
        if (SDL_GetGamepadButton(pad, b.button)) s.buttons |= b.pro;
    }
    if (axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f) s.buttons |= os::pro::kZL;
    if (axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f) s.buttons |= os::pro::kZR;
    s.left_x = axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    s.left_y = -axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    s.right_x = axis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    s.right_y = -axis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    return s;
}

// Which host pad is which controller once the title takes Pro Controllers: slot 0 is the GamePad
// (with the keyboard), slots 1-7 Pro Controllers, as a Wii U has one GamePad and up to seven
// others. A pad keeps its slot while plugged in; a new one takes the lowest free slot. Before
// that (and in titles without them) every pad drives the GamePad.
std::array<SDL_JoystickID, 1 + os::pro::kChannels> g_pad_slots{};

// A netplay guest's slot (host/netplay.h): no host pad takes it.
constexpr SDL_JoystickID kGuestSlot = 0xFFFFFFFFu;

void assign_pad_slots(const SDL_JoystickID* ids, int count) {
    for (size_t slot = 1; slot < g_pad_slots.size(); ++slot) {
        os::ProPadState unused;
        const bool guest = netplay::remote_pad(static_cast<int>(slot - 1), unused);
        if (guest) g_pad_slots[slot] = kGuestSlot; // (a host pad there moves below)
        else if (g_pad_slots[slot] == kGuestSlot) g_pad_slots[slot] = 0;
    }
    for (SDL_JoystickID& slot : g_pad_slots) {
        if (slot != 0 && slot != kGuestSlot && std::find(ids, ids + count, slot) == ids + count) slot = 0;
    }
    for (int i = 0; i < count; ++i) {
        if (std::find(g_pad_slots.begin(), g_pad_slots.end(), ids[i]) != g_pad_slots.end()) continue;
        const auto free = std::find(g_pad_slots.begin(), g_pad_slots.end(), SDL_JoystickID{0});
        if (free != g_pad_slots.end()) *free = ids[i];
    }
    // Until the GamePad's pad presses something, the first pad that does becomes the GamePad: a device
    // that never sends input (a virtual pad driver, a wheel) would otherwise hold it, and screens that
    // read only the GamePad (the "Joins the Battle!" unlocks) would wait forever.
    static SDL_JoystickID used = 0;
    if (g_pad_slots[0] != 0 && g_pad_slots[0] == used) return;
    for (size_t slot = 0; slot < g_pad_slots.size(); ++slot) {
        SDL_Gamepad* pad = g_pad_slots[slot] && g_pad_slots[slot] != kGuestSlot ? SDL_GetGamepadFromID(g_pad_slots[slot]) : nullptr;
        if (pad == nullptr) continue;
        bool pressed = false;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !pressed; ++b) {
            pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
        }
        if (!pressed) continue;
        used = g_pad_slots[slot];
        std::swap(g_pad_slots[0], g_pad_slots[slot]);
        break;
    }
}

// SMASH_VIRTUAL_PADS=N[@S] (1-8): N stand-in gamepads that press A for a tenth of a second every two
// seconds, one after another, from S seconds after start-up, to try several players without the pads.
void update_virtual_pads() {
    static const char* env = std::getenv("SMASH_VIRTUAL_PADS");
    static const uint64_t from_ms = env && std::strchr(env, '@') ? std::atoi(std::strchr(env, '@') + 1) * 1000ull : 0;
    static std::vector<SDL_Joystick*> pads = [] {
        std::vector<SDL_Joystick*> made;
        const int n = env ? std::clamp(std::atoi(env), 0, 8) : 0;
        for (int i = 0; i < n; ++i) {
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            desc.name = "Virtual pad";
            if (SDL_Joystick* pad = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&desc))) made.push_back(pad);
        }
        if (n) std::fprintf(stderr, "ttt2: input: %zu virtual pads\n", made.size());
        return made;
    }();
    const uint64_t ms = SDL_GetTicks();
    for (size_t i = 0; i < pads.size(); ++i) {
        const uint64_t phase = (ms + 2000 - 250 * i) % 2000;
        SDL_SetJoystickVirtualButton(pads[i], SDL_GAMEPAD_BUTTON_EAST, ms >= from_ms && phase < 100);
    }
}

void update_input() {
    update_virtual_pads();
    os::GamepadState state{};
    const bool pros = os::pro_controllers_in_use();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (pros) assign_pad_slots(ids, count);
    os::ProPadState channels[os::pro::kChannels]{};
    for (int c = 0; c < os::pro::kChannels; ++c) channels[c].connected = g_pad_slots[c + 1] != 0;
    if (g_overlay.captures && g_overlay.captures()) { // still connected, nothing pressed
        SDL_free(ids);
        os::set_gamepad_state(state);
        for (int c = 0; pros && c < os::pro::kChannels; ++c) os::set_pro_pad_state(c, channels[c]);
        return;
    }
    for (size_t i = 0; i < keymap::kInputCount; ++i) {
        if (!g_keys[g_keymap[i]]) continue;
        state.buttons |= keymap::kInputs[i].button;
        state.left_x += keymap::kInputs[i].stick_x;
        state.left_y += keymap::kInputs[i].stick_y;
    }

    for (int i = 0; i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) pad = SDL_OpenGamepad(ids[i]);
        if (pad == nullptr) continue;
        if (pros) {
            const auto slot = std::find(g_pad_slots.begin(), g_pad_slots.end(), ids[i]) - g_pad_slots.begin();
            if (slot >= static_cast<ptrdiff_t>(g_pad_slots.size())) continue; // a ninth pad
            if (slot > 0) {
                channels[slot - 1] = pro_pad(pad);
                continue;
            }
        }
        for (const PadButton& b : kPadButtons) {
            if (SDL_GetGamepadButton(pad, b.button)) state.buttons |= b.vpad;
        }
        if (axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f) state.buttons |= os::vpad::kZL;
        if (axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f) state.buttons |= os::vpad::kZR;
        state.left_x = std::clamp(state.left_x + axis(pad, SDL_GAMEPAD_AXIS_LEFTX), -1.0f, 1.0f);
        state.left_y = std::clamp(state.left_y - axis(pad, SDL_GAMEPAD_AXIS_LEFTY), -1.0f, 1.0f);
        state.right_x = std::clamp(state.right_x + axis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -1.0f, 1.0f);
        state.right_y = std::clamp(state.right_y - axis(pad, SDL_GAMEPAD_AXIS_RIGHTY), -1.0f, 1.0f);
    }
    SDL_free(ids);
    for (int c = 0; pros && c < os::pro::kChannels; ++c) netplay::remote_pad(c, channels[c]);
    os::set_gamepad_state(state);
    for (int c = 0; pros && c < os::pro::kChannels; ++c) os::set_pro_pad_state(c, channels[c]);
}

void update_size() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    g_width = static_cast<uint32_t>(std::max(w, 1));
    g_height = static_cast<uint32_t>(std::max(h, 1));
}

} // namespace

os::ProPadState pad_as_pro(SDL_Gamepad* pad) { return pro_pad(pad); }

bool open_window(const char* title) {
    // The identifier is the Wayland app id: ttt2.desktop's (ttt2 --install).
    SDL_SetAppMetadata(title, nullptr, "ttt2");
    // The window takes the executable's icon resource 1 when it has one.
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "ttt2: no window: %s\n", SDL_GetError());
        return false;
    }
    // The player's display settings (display.ini, which the launcher's
    // Settings write) with the environment's values over them, for this run:
    // the renderer takes the scale and the frame rate when it starts.
    const display::Loaded loaded = display::load(display::file_path());
    std::vector<std::string> log = loaded.log;
    display::set_active(display::resolve(loaded.settings, std::getenv("TTT2_FULLSCREEN"), std::getenv("TTT2_SCALE"),
                                         std::getenv("TTT2_FPS"), log));
    for (const std::string& line : log) std::fprintf(stderr, "ttt2: %s\n", line.c_str());
    const display::Active& active = display::active();
    // The window starts at the render size when that fits on the desktop
    // with room to spare, else at the largest multiple of 640x360 that does
    // (1280x720 on 1080p, 1920x1080 on 1440p, 3200x1800 on 4K at 100 %).
    int window_w = 1280, window_h = 720;
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        g_display_width = static_cast<uint32_t>(mode->w * mode->pixel_density);
        g_display_height = static_cast<uint32_t>(mode->h * mode->pixel_density);
        if (const float hz = refresh_of(SDL_GetPrimaryDisplay()); hz > 0.0f) g_display_refresh = g_window_refresh = hz;
        std::tie(window_w, window_h) = display::window_size(mode->w, mode->h, mode->pixel_density, active.scale);
    }
    // Fullscreen keeps the desktop's mode (F11 switches it in the game; that is not saved).
    g_window = SDL_CreateWindow(title, window_w, window_h,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                    (active.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (g_window == nullptr) {
        std::fprintf(stderr, "ttt2: no window: %s\n", SDL_GetError());
        return false;
    }
    update_size();
    update_window_refresh();
    std::fprintf(stderr, "ttt2: window: %ux%u pixels, %s\n", g_width.load(), g_height.load(),
                 active.fullscreen ? "fullscreen" : "windowed");
    g_keymap_file.emplace(keymap::file_path(), [](const std::string& name) {
        return static_cast<uint16_t>(SDL_GetScancodeFromName(name.c_str()));
    });
    reload_keymap();
    return true;
}

bool window_open() { return g_window != nullptr; }

void set_window_icon(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    if (g_window == nullptr || rgba.size() < size_t{width} * height * 4) return;
    SDL_Surface* icon = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32,
                                              const_cast<uint8_t*>(rgba.data()), static_cast<int>(width * 4));
    if (icon == nullptr) return;
    SDL_SetWindowIcon(g_window, icon);
    SDL_DestroySurface(icon);
}

Controller active_controller() { return g_controller; }

void on_controller_change(std::function<void(Controller)> callback) {
    g_controller_callback = std::move(callback);
    if (const char* forced = std::getenv("TTT2_CONTROLLER")) {
        const std::string kind = forced;
        g_controller = kind == "playstation" ? Controller::kPlayStation
                       : kind == "nintendo"  ? Controller::kNintendo
                                             : Controller::kOther;
        g_controller_fixed = true;
    }
    if (g_controller_callback) g_controller_callback(g_controller);
}

void set_overlay_hooks(OverlayHooks hooks) { g_overlay = std::move(hooks); }

bool fullscreen() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0; }

void set_fullscreen(bool on) {
    if (g_window) SDL_SetWindowFullscreen(g_window, on);
}

void request_quit() { g_quit_requested = true; }

bool run_event_loop(const std::function<bool()>& finished) {
    using namespace std::chrono_literals;
    auto keymap_check = std::chrono::steady_clock::now();
    while (!finished()) {
        // A stat about once a second; the file is read only when it changed.
        if (const auto now = std::chrono::steady_clock::now(); now - keymap_check >= 1s) {
            keymap_check = now;
            reload_keymap();
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            note_controller(event);
            note_key(event);
            if (g_overlay.event && g_overlay.event(&event)) continue;
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return false;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED: update_size(); break;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: update_window_refresh(); break;
            case SDL_EVENT_KEY_DOWN:
                // (F11 held repeats its press: the first one toggles, once.)
                if (event.key.scancode == SDL_SCANCODE_F11 && !event.key.repeat &&
                    !controller_keyboard(event.key.which)) {
                    const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(g_window, !full);
                }
                // F12: trace the next frame (gpu::request_frame_trace).
                if (event.key.scancode == SDL_SCANCODE_F12 && !event.key.repeat) gpu::request_frame_trace();
                // F10: profile the next ten seconds (profile-<n>.txt beside the executable).
                if (event.key.scancode == SDL_SCANCODE_F10 && !event.key.repeat) os::request_profile();
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                // Both sticks clicked: F12 from a pad, mid-fight.
                const SDL_GamepadButton other = event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK ? SDL_GAMEPAD_BUTTON_RIGHT_STICK
                                              : event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK ? SDL_GAMEPAD_BUTTON_LEFT_STICK
                                                                                                      : SDL_GAMEPAD_BUTTON_INVALID;
                SDL_Gamepad* pad = SDL_GetGamepadFromID(event.gbutton.which);
                if (other != SDL_GAMEPAD_BUTTON_INVALID && pad && SDL_GetGamepadButton(pad, other)) gpu::request_frame_trace();
                break;
            }
            default: break;
            }
        }
        if (g_quit_requested) return false;
        if (g_overlay.frame) g_overlay.frame();
        update_input();
        std::this_thread::sleep_for(4ms);
    }
    return true;
}

std::vector<const char*> vulkan_instance_extensions() {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (names == nullptr) fatal("SDL cannot present with Vulkan: %s", SDL_GetError());
    return std::vector<const char*>(names, names + count);
}

VkSurfaceKHR create_vulkan_surface(VkInstance instance) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(g_window, instance, nullptr, &surface)) {
        fatal("cannot create a Vulkan surface for the window: %s", SDL_GetError());
    }
    return surface;
}

float display_refresh_rate() { return g_display_refresh; }
float window_refresh_rate() {
    // TTT2_DISPLAY_HZ: report another rate (testing how presentation copes
    // with a display that refreshes otherwise than reported).
    static const float assumed = [] {
        const char* v = std::getenv("TTT2_DISPLAY_HZ");
        return v ? static_cast<float>(std::atof(v)) : 0.0f;
    }();
    return assumed > 0.0f ? assumed : g_window_refresh.load();
}

void display_size(uint32_t& width, uint32_t& height) {
    width = g_display_width;
    height = g_display_height;
}

void drawable_size(uint32_t& width, uint32_t& height) {
    width = g_width;
    height = g_height;
}

} // namespace cafe::host
