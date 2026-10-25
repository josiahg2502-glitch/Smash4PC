#pragma once

// The game window: SDL3 on the process's main thread, which owns the event
// loop; keyboard (keys from keyboard.ini, host/keymap.h) and gamepads feed
// the Wii U GamePad state. The renderer presents into it from the GPU
// thread through a Vulkan surface. Whether it starts fullscreen, its size
// and the renderer's scale and frame rate come from display.ini
// (host/display_settings.h), read when it opens.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace cafe::host {

// Opens the window; false if there is no display (the port then runs
// headless). Sets display::active() for the run first: the renderer reads
// it. Main thread only.
bool open_window(const char* title);
bool window_open();
// The window's icon, RGBA rows top first. Main thread only.
void set_window_icon(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);

// The kind of controller the player uses: the last one pressed (gamepads
// by their maker's button names; the keyboard counts as kOther).
// TTT2_CONTROLLER=playstation, nintendo or xbox sets it for good.
enum class Controller { kOther, kPlayStation, kNintendo };
Controller active_controller();
// Called on the event loop's thread when the kind changes.
void on_controller_change(std::function<void(Controller)> callback);

// An in-game overlay (Smash 4 PC's menu), on the event loop's thread:
// `event` sees each SDL_Event first (true: the game does not get it),
// `frame` runs once per loop, and while `captures` is true the game's
// controls read as released.
struct OverlayHooks {
    std::function<bool(const void* sdl_event)> event;
    std::function<void()> frame;
    std::function<bool()> captures;
};
void set_overlay_hooks(OverlayHooks hooks);
// Event loop thread.
bool fullscreen();
void set_fullscreen(bool on);
// The event loop ends as if the window were closed.
void request_quit();

// Runs the event loop until `finished` returns true or the window closes.
// Returns false if the user closed the window.
bool run_event_loop(const std::function<bool()>& finished);

// For the renderer (any thread).
std::vector<const char*> vulkan_instance_extensions();
VkSurfaceKHR create_vulkan_surface(VkInstance instance);
void drawable_size(uint32_t& width, uint32_t& height);
// The desktop resolution of the primary display, in pixels.
void display_size(uint32_t& width, uint32_t& height);
// The primary display's refresh rate, in Hz.
float display_refresh_rate();
// The refresh rate of the display the window is on, in Hz (it follows the
// window to other displays; the primary's before the window opens).
float window_refresh_rate();

} // namespace cafe::host
