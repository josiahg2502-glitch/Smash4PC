// Smash 4 PC's logo on the title screen: title-logo.png (any size,
// transparent background) fitted into the rectangles where the title's
// textures have their logos; the rest of those textures stays the title's.

#include "gpu/replacements.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace smash {
namespace {

// A texture with a logo (2048x2048 BC3, by the MD5 of its level 0): the
// logo's rectangle, the overlay that replaces it with its surroundings
// (from 0, 0) and what fills the overlay around the new logo.
struct Place {
    std::array<uint8_t, 16> md5;
    uint32_t x, y, width, height;
    uint32_t overlay_width, overlay_height;
    bool on_black; // an opaque black texture (drawn additively), else transparent
};
constexpr uint32_t kSize = 2048, kFormat = 0x33;
constexpr Place kPlaces[] = {
    // The title screen's atlas: the logo in the top left, sparkles from row 794.
    {{0x8e, 0x55, 0x7f, 0x09, 0xe5, 0x0f, 0xbb, 0xc2, 0x17, 0x81, 0xdc, 0x74, 0xbb, 0x80, 0xa5, 0xbb},
     4, 3, 1586, 681, 1596, 792, false},
    // The flash that reveals the title screen: the older logo on a black square.
    {{0x05, 0x55, 0x2c, 0x8d, 0x8e, 0x1c, 0xfc, 0x87, 0x05, 0x9c, 0x53, 0xb0, 0x28, 0xcb, 0xcc, 0xb0},
     267, 711, 1386, 584, 1922, 1922, true},
};

struct Rgba {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> texels;
};

// Bilinear resampling of `src` (cropped to x0, y0, w, h) to dw x dh, with
// premultiplied alpha so that edges do not darken.
Rgba resample(const Rgba& src, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) {
    Rgba out{dw, dh, std::vector<uint8_t>(size_t{dw} * dh * 4)};
    const auto at = [&](int x, int y, float premultiplied[4]) {
        x = std::clamp(x, 0, static_cast<int>(w) - 1);
        y = std::clamp(y, 0, static_cast<int>(h) - 1);
        const uint8_t* p = &src.texels[((size_t{y0} + y) * src.width + x0 + x) * 4];
        const float a = p[3] / 255.0f;
        for (int k = 0; k < 3; ++k) premultiplied[k] = p[k] * a;
        premultiplied[3] = p[3];
    };
    for (uint32_t y = 0; y < dh; ++y) {
        const float sy = (y + 0.5f) * h / dh - 0.5f;
        const int iy = static_cast<int>(std::floor(sy));
        const float fy = sy - iy;
        for (uint32_t x = 0; x < dw; ++x) {
            const float sx = (x + 0.5f) * w / dw - 0.5f;
            const int ix = static_cast<int>(std::floor(sx));
            const float fx = sx - ix;
            float a[4], b[4], c[4], d[4], v[4];
            at(ix, iy, a);
            at(ix + 1, iy, b);
            at(ix, iy + 1, c);
            at(ix + 1, iy + 1, d);
            for (int k = 0; k < 4; ++k) {
                v[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
            }
            uint8_t* o = &out.texels[(size_t{y} * dw + x) * 4];
            const float alpha = v[3] / 255.0f;
            for (int k = 0; k < 3; ++k) o[k] = alpha > 0 ? static_cast<uint8_t>(std::clamp(v[k] / alpha, 0.0f, 255.0f) + 0.5f) : 0;
            o[3] = static_cast<uint8_t>(std::clamp(v[3], 0.0f, 255.0f) + 0.5f);
        }
    }
    return out;
}

} // namespace

void register_title_logo(const std::filesystem::path& png) {
    int w = 0, h = 0, channels = 0;
    std::unique_ptr<stbi_uc, void (*)(void*)> pixels(stbi_load(png.string().c_str(), &w, &h, &channels, 4), stbi_image_free);
    if (!pixels) {
        std::fprintf(stderr, "smash: no title logo (%s), the original shows\n", png.string().c_str());
        return;
    }
    Rgba logo{static_cast<uint32_t>(w), static_cast<uint32_t>(h),
              std::vector<uint8_t>(pixels.get(), pixels.get() + size_t{static_cast<uint32_t>(w)} * h * 4)};
    // The visible part of the image, fitted into the logo's rectangle and centred.
    uint32_t left = logo.width, top = logo.height, right = 0, bottom = 0;
    for (uint32_t y = 0; y < logo.height; ++y) {
        for (uint32_t x = 0; x < logo.width; ++x) {
            if (logo.texels[(size_t{y} * logo.width + x) * 4 + 3] > 8) {
                left = std::min(left, x), right = std::max(right, x + 1);
                top = std::min(top, y), bottom = std::max(bottom, y + 1);
            }
        }
    }
    if (right <= left || bottom <= top) return;
    for (const Place& place : kPlaces) {
        const float scale = std::min(float(place.width) / (right - left), float(place.height) / (bottom - top));
        const uint32_t dw = std::max(1u, static_cast<uint32_t>((right - left) * scale));
        const uint32_t dh = std::max(1u, static_cast<uint32_t>((bottom - top) * scale));
        const Rgba fitted = resample(logo, left, top, right - left, bottom - top, dw, dh);

        auto overlay = std::make_shared<std::vector<uint8_t>>(size_t{place.overlay_width} * place.overlay_height * 4, 0);
        if (place.on_black) {
            for (size_t i = 3; i < overlay->size(); i += 4) (*overlay)[i] = 255;
        }
        const uint32_t ox = place.x + (place.width - dw) / 2, oy = place.y + (place.height - dh) / 2;
        for (uint32_t y = 0; y < dh; ++y) {
            const uint8_t* from = &fitted.texels[size_t{y} * dw * 4];
            uint8_t* to = &(*overlay)[((size_t{oy} + y) * place.overlay_width + ox) * 4];
            if (!place.on_black) {
                std::copy_n(from, size_t{dw} * 4, to);
                continue;
            }
            for (uint32_t x = 0; x < dw; ++x) { // over black: premultiplied, opaque
                for (int k = 0; k < 3; ++k) to[x * 4 + k] = static_cast<uint8_t>(from[x * 4 + k] * from[x * 4 + 3] / 255);
            }
        }
        cafe::gpu::TextureReplacement r;
        r.width = r.height = kSize;
        r.format = kFormat;
        r.md5 = place.md5;
        r.contents = [overlay] { return overlay.get(); };
        r.overlay_width = place.overlay_width;
        r.overlay_height = place.overlay_height;
        cafe::gpu::add_texture_replacement(std::move(r));
    }
    std::fprintf(stderr, "smash: title logo %s\n", png.string().c_str());
}

} // namespace smash
