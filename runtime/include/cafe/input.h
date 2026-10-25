#pragma once

// Host-side GamePad state, set by the window/input layer and read by the
// game through VPADRead.

#include <cstdint>

namespace cafe::os {

// VPAD button bits, as the game sees them.
namespace vpad {
constexpr uint32_t kA = 0x8000, kB = 0x4000, kX = 0x2000, kY = 0x1000;
constexpr uint32_t kLeft = 0x0800, kRight = 0x0400, kUp = 0x0200, kDown = 0x0100;
constexpr uint32_t kZL = 0x0080, kZR = 0x0040, kL = 0x0020, kR = 0x0010;
constexpr uint32_t kPlus = 0x0008, kMinus = 0x0004, kHome = 0x0002;
constexpr uint32_t kStickR = 0x00020000, kStickL = 0x00040000;
} // namespace vpad

struct GamepadState {
    uint32_t buttons;
    float left_x, left_y;   // -1..1, up positive
    float right_x, right_y;
};

void set_gamepad_state(const GamepadState& state);

// Wii U Pro Controllers (padscore): seven channels, beside the one GamePad.
namespace pro {
constexpr int kChannels = 7;
constexpr uint32_t kUp = 0x0001, kLeft = 0x0002, kZR = 0x0004, kX = 0x0008, kA = 0x0010, kY = 0x0020;
constexpr uint32_t kB = 0x0040, kZL = 0x0080, kR = 0x0200, kPlus = 0x0400, kHome = 0x0800;
constexpr uint32_t kMinus = 0x1000, kL = 0x2000, kDown = 0x4000, kRight = 0x8000;
constexpr uint32_t kStickR = 0x00010000, kStickL = 0x00020000;
} // namespace pro

struct ProPadState {
    bool connected;
    uint32_t buttons; // pro::k*
    float left_x, left_y, right_x, right_y; // -1..1, up positive
};

void set_pro_pad_state(int channel, const ProPadState& state);
// The title set a Pro Controller/Wii Remote connect callback: extra host pads become its controllers.
bool pro_controllers_in_use();

// + pressed together with another button, for a mode that gives the chord a
// meaning of its own (title/practice.cpp: Practice's position reset). The
// mode arms it on every frame the chord can happen, saying whether the other
// button is held. While armed, a press of + waits eight of those frames
// before the game sees it; if the other button is held by then, the press is
// the chord's and the game never sees it. take_start_chord() is true once
// for each chord.
void arm_start_chord(bool partner_held);
bool take_start_chord();

} // namespace cafe::os
