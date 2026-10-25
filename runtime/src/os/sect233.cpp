#include "os/sect233.h"

#include <cstring>

namespace cafe::os::sect233 {
namespace {

// GF(2^233) elements: 233 bits in four 64-bit words, least significant first.
// Reduction polynomial x^233 + x^74 + 1.
struct F {
    uint64_t w[4]{};
};

bool zero(const F& a) { return (a.w[0] | a.w[1] | a.w[2] | a.w[3]) == 0; }
bool equal(const F& a, const F& b) { return std::memcmp(a.w, b.w, sizeof a.w) == 0; }
F add(const F& a, const F& b) { return {{a.w[0] ^ b.w[0], a.w[1] ^ b.w[1], a.w[2] ^ b.w[2], a.w[3] ^ b.w[3]}}; }

bool bit(const uint64_t* w, int i) { return (w[i >> 6] >> (i & 63)) & 1; }

// Reduces a product of up to 465 bits (eight words) modulo the polynomial.
F reduce(uint64_t (&p)[8]) {
    for (int i = 464; i >= 233; --i) {
        if (!bit(p, i)) continue;
        p[i >> 6] ^= 1ull << (i & 63);          // x^i = x^(i-233) * (x^74 + 1)
        const int j = i - 233;
        p[j >> 6] ^= 1ull << (j & 63);
        p[(j + 74) >> 6] ^= 1ull << ((j + 74) & 63);
    }
    return {{p[0], p[1], p[2], p[3] & ((1ull << 41) - 1)}};
}

F mul(const F& a, const F& b) {
    uint64_t p[8]{};
    for (int i = 0; i < 233; ++i) {
        if (!bit(b.w, i)) continue;
        const int words = i >> 6, shift = i & 63;
        for (int k = 0; k < 4; ++k) {
            p[k + words] ^= a.w[k] << shift;
            if (shift) p[k + words + 1] ^= a.w[k] >> (64 - shift);
        }
    }
    return reduce(p);
}

int degree(const uint64_t* w, int words) {
    for (int k = words - 1; k >= 0; --k) {
        if (w[k]) return k * 64 + 63 - __builtin_clzll(w[k]);
    }
    return -1;
}

// a^-1 by the binary extended Euclidean algorithm (a != 0).
F inverse(const F& a) {
    uint64_t u[5] = {a.w[0], a.w[1], a.w[2], a.w[3], 0};
    uint64_t v[5] = {1, 0, 0, 0, 0};
    v[74 >> 6] |= 1ull << (74 & 63);
    v[233 >> 6] |= 1ull << (233 & 63);
    uint64_t g1[5] = {1, 0, 0, 0, 0}, g2[5] = {};
    const auto shifted_xor = [](uint64_t* dst, const uint64_t* src, int j) {
        const int words = j >> 6, shift = j & 63;
        for (int k = 4; k >= 0; --k) {
            uint64_t s = 0;
            if (k - words >= 0) s = src[k - words] << shift;
            if (shift && k - words - 1 >= 0) s |= src[k - words - 1] >> (64 - shift);
            dst[k] ^= s;
        }
    };
    while (degree(u, 5) != 0) {
        int j = degree(u, 5) - degree(v, 5);
        if (j < 0) {
            for (int k = 0; k < 5; ++k) {
                std::swap(u[k], v[k]);
                std::swap(g1[k], g2[k]);
            }
            j = -j;
        }
        shifted_xor(u, v, j);
        shifted_xor(g1, g2, j);
    }
    uint64_t p[8] = {g1[0], g1[1], g1[2], g1[3], g1[4], 0, 0, 0};
    return reduce(p);
}

F from_bytes(const Bytes30& b) {
    F f;
    for (int i = 0; i < 30; ++i) f.w[i / 8] |= uint64_t(b[29 - i]) << (8 * (i % 8));
    return f;
}
Bytes30 to_bytes(const F& f) {
    Bytes30 b{};
    for (int i = 0; i < 30; ++i) b[29 - i] = static_cast<uint8_t>(f.w[i / 8] >> (8 * (i % 8)));
    return b;
}

struct Point {
    F x, y;
    bool infinity = true;
};

const F kOne{{1, 0, 0, 0}};

Point twice(const Point& p) {
    if (p.infinity || zero(p.x)) return {};
    const F l = add(p.x, mul(p.y, inverse(p.x)));
    Point r;
    r.infinity = false;
    r.x = add(add(mul(l, l), l), kOne); // a = 1
    r.y = add(mul(p.x, p.x), mul(add(l, kOne), r.x));
    return r;
}

Point sum(const Point& p, const Point& q) {
    if (p.infinity) return q;
    if (q.infinity) return p;
    if (equal(p.x, q.x)) return equal(p.y, q.y) ? twice(p) : Point{};
    const F l = mul(add(p.y, q.y), inverse(add(p.x, q.x)));
    Point r;
    r.infinity = false;
    r.x = add(add(add(add(mul(l, l), l), p.x), q.x), kOne);
    r.y = add(add(mul(l, add(p.x, r.x)), r.x), p.y);
    return r;
}

constexpr Bytes30 kGx = {0x00, 0xFA, 0xC9, 0xDF, 0xCB, 0xAC, 0x83, 0x13, 0xBB, 0x21, 0x39, 0xF1, 0xBB, 0x75, 0x5F,
                         0xEF, 0x65, 0xBC, 0x39, 0x1F, 0x8B, 0x36, 0xF8, 0xF8, 0xEB, 0x73, 0x71, 0xFD, 0x55, 0x8B};
constexpr Bytes30 kGy = {0x01, 0x00, 0x6A, 0x08, 0xA4, 0x19, 0x03, 0x35, 0x06, 0x78, 0xE5, 0x85, 0x28, 0xBE, 0xBF,
                         0x8A, 0x0B, 0xEF, 0xF8, 0x67, 0xA7, 0xCA, 0x36, 0x71, 0x6F, 0x7E, 0x01, 0xF8, 0x10, 0x52};

} // namespace

void public_key(const Bytes30& private_key, Bytes30& x, Bytes30& y) {
    Point g;
    g.x = from_bytes(kGx);
    g.y = from_bytes(kGy);
    g.infinity = false;
    Point r;
    for (int i = 0; i < 240; ++i) { // most significant bit first
        r = twice(r);
        if ((private_key[i / 8] >> (7 - i % 8)) & 1) r = sum(r, g);
    }
    x = to_bytes(r.x);
    y = to_bytes(r.y);
}

} // namespace cafe::os::sect233
