#pragma once

// Netplay by streaming, like Steam's Remote Play Together: the host's game is the only one running; each guest
// sees its picture and hears its sound, and plays on it with their controller as one of the host's Wii U Pro
// Controllers (players 2-8). Nothing to keep in step, so every mode, 8 players, items and mods all work, and
// guests need no game files. One TCP connection per guest (port kDefaultPort): the host forwards that port or
// both use a LAN or a VPN such as Tailscale or ZeroTier.

#include "cafe/input.h"

#include <cstdint>
#include <functional>
#include <string>

namespace cafe::host::netplay {

constexpr uint16_t kDefaultPort = 52700;
// The stream's picture: 720p (both sides multiples of 16, as H.264 codes them).
constexpr uint32_t kStreamWidth = 1280, kStreamHeight = 720;

// ------------------------------------------------------------------ host
// Starts taking guests on `port`; false (and a message) if it cannot listen.
bool start_host(uint16_t port);
bool hosting();
// A guest is connected: the renderer sends the TV image (video_frame).
bool wants_video();
// The guest playing as Pro Controller `channel` (0-6): their controller now. False: no guest there.
bool remote_pad(int channel, os::ProPadState& state);
// One frame for the guests, RGBA (or BGRA) rows of kStreamWidth x kStreamHeight at `pixels`, valid once `wait`
// returns and until `release` (the renderer's readback slot). Any thread; dropped if the encoder is busy.
void video_frame(const uint8_t* pixels, bool bgra, std::function<void()> wait, std::function<void()> release);
// The game's sound, 48 kHz stereo (host/audio.cpp, as it goes to the speakers).
void audio(const float* frames, uint32_t count);

// ------------------------------------------------------------------ guest
// smash.exe --join HOST[:PORT] [NAME]: the viewer window until it closes or the host goes; the exit code.
int run_guest(const std::string& address, const std::string& name);

} // namespace cafe::host::netplay
