#pragma once

// The NIST/SEC curve sect233r1 (y^2 + xy = x^3 + x^2 + b over GF(2^233)),
// only as far as the console's device certificate needs: the public key of
// a private one. Big-endian 30-byte values, as Wii U certificates hold them.

#include <array>
#include <cstdint>

namespace cafe::os::sect233 {

using Bytes30 = std::array<uint8_t, 30>;

// x and y of private * G.
void public_key(const Bytes30& private_key, Bytes30& x, Bytes30& y);

} // namespace cafe::os::sect233
