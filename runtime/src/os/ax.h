#pragma once

// AX internals shared with the snd_user libraries (MIX).

#include <cstdint>

namespace cafe::os {

// A voice's volume envelope (AXSetVoiceVe): 0x8000 = 1.0, delta per
// 32 kHz sample.
void set_voice_ve(uint32_t vpb, uint16_t volume, int16_t delta);

} // namespace cafe::os
