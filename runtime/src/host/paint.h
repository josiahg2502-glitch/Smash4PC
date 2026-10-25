#pragma once

// Drawing for Smash 4 PC's menus, the launcher's and the in-game overlay's:
// shapes and text on an SDL renderer (a window's, or a software one into a
// surface). Text uses stb_truetype; one file of each program includes this,
// then compiles stb_truetype's implementation (STB_TRUETYPE_IMPLEMENTATION).

#include <SDL3/SDL.h>

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cafe::host::paint {

struct Color {
    float r, g, b, a = 1;
};
constexpr Color kWhite{1, 1, 1};
constexpr Color kDim{0.78f, 0.80f, 0.86f};
constexpr Color kGood{0.80f, 0.88f, 0.46f};
constexpr Color kBad{1.0f, 0.47f, 0.42f};
constexpr Color kGold{0.80f, 0.68f, 0.38f};
constexpr Color kBadge{0.86f, 0.70f, 0.24f};

float ease(float current, float target, float dt, float rate) { return target + (current - target) * std::exp(-rate * dt); }

struct Image {
    SDL_Texture* texture = nullptr;
    float width = 0, height = 0;
};

class Painter {
public:
    explicit Painter(SDL_Renderer* r) : r_(r) {}

    // A quad with a colour per corner (top left, top right, bottom right, bottom left).
    void quad(SDL_FPoint p[4], const Color c[4]) {
        SDL_Vertex v[4];
        for (int i = 0; i < 4; ++i) v[i] = {p[i], {c[i].r, c[i].g, c[i].b, c[i].a}, {0, 0}};
        static const int indices[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(r_, nullptr, v, 4, indices, 6);
    }
    void rect(float x, float y, float w, float h, Color tl, Color tr, Color br, Color bl) {
        SDL_FPoint p[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
        const Color c[4] = {tl, tr, br, bl};
        quad(p, c);
    }
    void rect(float x, float y, float w, float h, Color c) { rect(x, y, w, h, c, c, c, c); }
    void line(float x0, float y0, float x1, float y1, float thickness, Color c) {
        const float dx = x1 - x0, dy = y1 - y0, len = std::max(0.001f, std::hypot(dx, dy));
        const float nx = -dy / len * thickness / 2, ny = dx / len * thickness / 2;
        // A little longer than the segment, so that joints close.
        const float ex = dx / len * thickness / 2, ey = dy / len * thickness / 2;
        SDL_FPoint p[4] = {{x0 - ex + nx, y0 - ey + ny}, {x1 + ex + nx, y1 + ey + ny}, {x1 + ex - nx, y1 + ey - ny},
                           {x0 - ex - nx, y0 - ey - ny}};
        const Color cs[4] = {c, c, c, c};
        quad(p, cs);
    }
    void round_rect(float x, float y, float w, float h, float radius, Color c) {
        radius = std::min({radius, w / 2, h / 2});
        std::vector<SDL_Vertex> v;
        std::vector<int> idx;
        const SDL_FColor fc{c.r, c.g, c.b, c.a};
        v.push_back({{x + w / 2, y + h / 2}, fc, {0, 0}});
        const float cx[4] = {x + w - radius, x + radius, x + radius, x + w - radius};
        const float cy[4] = {y + radius, y + radius, y + h - radius, y + h - radius};
        constexpr int kSteps = 8;
        for (int corner = 0; corner < 4; ++corner) {
            for (int s = 0; s <= kSteps; ++s) {
                const float a = (corner * 90.0f + s * 90.0f / kSteps) * float(M_PI) / 180.0f;
                v.push_back({{cx[corner] + std::cos(a) * radius, cy[corner] - std::sin(a) * radius}, fc, {0, 0}});
            }
        }
        const int n = static_cast<int>(v.size()) - 1;
        for (int i = 1; i <= n; ++i) idx.insert(idx.end(), {0, i, i % n + 1});
        SDL_RenderGeometry(r_, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
    }
    void image(const Image& img, SDL_FRect dst, float alpha = 1, const SDL_FRect* src = nullptr) {
        if (!img.texture) return;
        SDL_SetTextureAlphaModFloat(img.texture, alpha);
        SDL_RenderTexture(r_, img.texture, src, &dst);
    }
    // A chevron pointing left (dir -1) or right (1), centred at x, y.
    void chevron(float x, float y, float size, int dir, float thickness, Color c) {
        line(x + dir * size * 0.25f, y, x - dir * size * 0.25f, y - size * 0.5f, thickness, c);
        line(x + dir * size * 0.25f, y, x - dir * size * 0.25f, y + size * 0.5f, thickness, c);
    }
    void check(float x, float y, float size, float thickness, Color c) {
        line(x, y + size * 0.55f, x + size * 0.36f, y + size * 0.88f, thickness, c);
        line(x + size * 0.36f, y + size * 0.88f, x + size, y + size * 0.12f, thickness, c);
    }
    void cross(float x, float y, float size, float thickness, Color c) {
        line(x, y, x + size, y + size, thickness, c);
        line(x + size, y, x, y + size, thickness, c);
    }
    void spinner(float cx, float cy, float radius, float thickness, float t, Color c) {
        constexpr int kSegments = 18;
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = t * 5.0f + i * 0.26f, a1 = a0 + 0.27f;
            Color fade = c;
            fade.a *= float(i + 1) / kSegments;
            line(cx + std::cos(a0) * radius, cy + std::sin(a0) * radius, cx + std::cos(a1) * radius,
                 cy + std::sin(a1) * radius, thickness, fade);
        }
    }

private:
    SDL_Renderer* r_;
};

// Text: the font's printable ASCII baked per pixel size into a texture.
class Font {
public:
    bool open(const std::vector<std::filesystem::path>& candidates) {
        for (const std::filesystem::path& p : candidates) {
            std::ifstream in(p, std::ios::binary);
            if (!in) continue;
            data_.assign(std::istreambuf_iterator<char>(in), {});
            if (stbtt_InitFont(&info_, data_.data(), stbtt_GetFontOffsetForIndex(data_.data(), 0))) return true;
        }
        data_.clear();
        return false;
    }

    // Draws `text` with its baseline at y; returns its width.
    float draw(SDL_Renderer* r, float x, float y, const std::string& text, float px, Color c, bool shadow = true) {
        Baked* b = baked(r, px);
        if (!b) return 0;
        if (shadow) draw_run(b, r, x + px * 0.05f, y + px * 0.06f, text, {0, 0, 0, c.a * 0.55f});
        return draw_run(b, r, x, y, text, c);
    }
    float width(SDL_Renderer* r, const std::string& text, float px) {
        Baked* b = baked(r, px);
        float w = 0;
        for (unsigned char ch : text) w += b && ch >= 32 && ch < 127 ? b->chars[ch - 32].xadvance : 0;
        return w;
    }

private:
    struct Baked {
        SDL_Texture* texture = nullptr;
        int side = 0;
        stbtt_packedchar chars[95];
    };

    float draw_run(Baked* b, SDL_Renderer* r, float x, float y, const std::string& text, Color c) {
        SDL_SetTextureColorModFloat(b->texture, c.r, c.g, c.b);
        SDL_SetTextureAlphaModFloat(b->texture, c.a);
        const float start = x;
        for (unsigned char ch : text) {
            if (ch < 32 || ch >= 127) continue;
            stbtt_aligned_quad q;
            stbtt_GetPackedQuad(b->chars, b->side, b->side, ch - 32, &x, &y, &q, 0);
            const SDL_FRect src{q.s0 * b->side, q.t0 * b->side, (q.s1 - q.s0) * b->side, (q.t1 - q.t0) * b->side};
            const SDL_FRect dst{q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
            SDL_RenderTexture(r, b->texture, &src, &dst);
        }
        return x - start;
    }

    Baked* baked(SDL_Renderer* r, float px) {
        if (data_.empty()) return nullptr;
        const int key = std::max(6, static_cast<int>(std::lround(px)));
        if (auto it = cache_.find(key); it != cache_.end()) return it->second.get();
        auto b = std::make_unique<Baked>();
        const int oversample = key < 40 ? 2 : 1;
        for (b->side = 256; b->side <= 4096; b->side *= 2) {
            std::vector<uint8_t> alpha(size_t(b->side) * b->side);
            stbtt_pack_context pc;
            if (!stbtt_PackBegin(&pc, alpha.data(), b->side, b->side, 0, 1, nullptr)) return nullptr;
            stbtt_PackSetOversampling(&pc, oversample, oversample);
            const bool ok = stbtt_PackFontRange(&pc, data_.data(), 0, float(key), 32, 95, b->chars);
            stbtt_PackEnd(&pc);
            if (!ok) continue;
            std::vector<uint8_t> rgba(alpha.size() * 4, 255);
            for (size_t i = 0; i < alpha.size(); ++i) rgba[i * 4 + 3] = alpha[i];
            b->texture = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, b->side, b->side);
            SDL_UpdateTexture(b->texture, nullptr, rgba.data(), b->side * 4);
            SDL_SetTextureBlendMode(b->texture, SDL_BLENDMODE_BLEND);
            break;
        }
        if (!b->texture) return nullptr;
        return (cache_[key] = std::move(b)).get();
    }

    std::vector<uint8_t> data_;
    stbtt_fontinfo info_{};
    std::map<int, std::unique_ptr<Baked>> cache_;
};

} // namespace cafe::host::paint
