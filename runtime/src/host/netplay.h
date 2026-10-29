#pragma once

// Netplay by streaming, like Steam's Remote Play Together: the host's game is the only one running; each guest
// sees its picture and hears its sound, and plays on it with their controller as one of the host's Wii U Pro
// Controllers (players 2-8). Nothing to keep in step, so every mode, 8 players, items and mods all work, and
// guests need no game files.
//
// Rooms: a hosting game lists itself on the room server (server/worker: a Cloudflare Worker) and guests pick it
// from the launcher's list. Both learn their public UDP address from a STUN server and the room server introduces
// them, then they send to each other until their routers let the packets through (hole punching): no port
// forwarding. Everything after that goes directly between the two PCs, over UDP.

#include "cafe/input.h"
#include "host/rooms.h"

#include <cstdint>
#include <functional>
#include <string>

namespace cafe::host::netplay {

// The UDP port a host takes (any free one if it is in use); guests use any.
constexpr uint16_t kDefaultPort = 52700;
// The stream's picture: 1080p.
constexpr uint32_t kStreamWidth = 1920, kStreamHeight = 1080;

// ------------------------------------------------------------------ host
// Starts taking guests and lists the room as `name`'s; false (and a message) if it cannot.
bool start_host(const std::string& name);
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
// smash.exe --join room:ID|HOST[:PORT] [NAME]: the viewer window until it closes or the host goes; the exit code.
int run_guest(const std::string& target, const std::string& name);

} // namespace cafe::host::netplay
