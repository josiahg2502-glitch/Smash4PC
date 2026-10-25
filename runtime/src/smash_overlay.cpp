// Smash 4 PC's in-game overlay: a menu over the game (Esc, or a pad's Home /
// Guide button) and a frame counter in a corner. Both are drawn on the CPU
// (Canvas) on the event loop's thread, in the launcher's colours; the
// renderer lays them over the picture (gpu::set_overlay). While the menu is
// open the game's controls read as released.
//
// overlay.ini beside the executable keeps the counter's mode and corner; the
// frame rate goes to display.ini (it applies from the next start).

#include "host/paint.h" // before the stb_truetype implementation below
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "gpu/gpu.h"
#include "host/display_settings.h"
#include "host/window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace smash {
namespace {

namespace fs = std::filesystem;
namespace host = cafe::host;
namespace display = cafe::host::display;
using namespace cafe::host::paint;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

Color with_alpha(Color c, float a) {
    c.a *= a;
    return c;
}

// A premultiplied RGBA image drawn on the CPU: anti-aliased shapes (signed
// distances per pixel) and text from stb_truetype's packed glyphs. A 4K
// menu draws in a few milliseconds (SDL's software renderer took 150+).
class Canvas {
public:
    std::vector<fs::path> fonts;

    // Cleared to transparent, `w` by `h`.
    bool begin(int w, int h) {
        if (font_data_.empty()) open_font();
        w_ = w;
        h_ = h;
        pixels_.assign(size_t(std::max(w, 0)) * std::max(h, 0), 0);
        return w > 0 && h > 0;
    }

    // Starting from `base` (an earlier image of the same size) instead.
    void begin_from(const std::vector<uint32_t>& base, int w, int h) {
        w_ = w;
        h_ = h;
        pixels_ = base;
    }
    const std::vector<uint32_t>& pixels() const { return pixels_; }

    // What was drawn, as a layer at x, y.
    cafe::gpu::OverlayLayer end(int x, int y) {
        cafe::gpu::OverlayLayer layer;
        layer.x = x;
        layer.y = y;
        layer.width = static_cast<uint32_t>(w_);
        layer.height = static_cast<uint32_t>(h_);
        layer.pixels = std::move(pixels_);
        return layer;
    }

    void rect(float x, float y, float w, float h, Color c) { rect(x, y, w, h, c, c, c, c); }
    // Corner colours top left, top right, bottom right, bottom left; edges cover partial pixels.
    void rect(float x, float y, float w, float h, Color tl, Color tr, Color br, Color bl) {
        const int x0 = std::max(0, int(std::floor(x))), x1 = std::min(w_, int(std::ceil(x + w)));
        const int y0 = std::max(0, int(std::floor(y))), y1 = std::min(h_, int(std::ceil(y + h)));
        for (int py = y0; py < y1; ++py) {
            const float cy = std::clamp(std::min(py + 1.0f, y + h) - std::max(float(py), y), 0.0f, 1.0f);
            const float v = std::clamp((py + 0.5f - y) / h, 0.0f, 1.0f);
            for (int px = x0; px < x1; ++px) {
                const float cx = std::clamp(std::min(px + 1.0f, x + w) - std::max(float(px), x), 0.0f, 1.0f);
                const float u = std::clamp((px + 0.5f - x) / w, 0.0f, 1.0f);
                blend(px, py, mix(mix(tl, tr, u), mix(bl, br, u), v), cx * cy);
            }
        }
    }

    void round_rect(float x, float y, float w, float h, float radius, Color c) { shape(x, y, w, h, radius, 0, c); }
    // A soft shadow: the shape blurred over `blur` pixels.
    void shadow(float x, float y, float w, float h, float radius, float blur, Color c) {
        shape(x - blur, y - blur, w + 2 * blur, h + 2 * blur, radius + blur, 2 * blur, c);
    }

    void line(float x0, float y0, float x1, float y1, float thickness, Color c) {
        const float r = thickness / 2;
        const int bx0 = std::max(0, int(std::floor(std::min(x0, x1) - r - 1)));
        const int bx1 = std::min(w_, int(std::ceil(std::max(x0, x1) + r + 1)));
        const int by0 = std::max(0, int(std::floor(std::min(y0, y1) - r - 1)));
        const int by1 = std::min(h_, int(std::ceil(std::max(y0, y1) + r + 1)));
        const float dx = x1 - x0, dy = y1 - y0, len2 = std::max(dx * dx + dy * dy, 1e-6f);
        for (int py = by0; py < by1; ++py) {
            for (int px = bx0; px < bx1; ++px) {
                const float ux = px + 0.5f - x0, uy = py + 0.5f - y0;
                const float t = std::clamp((ux * dx + uy * dy) / len2, 0.0f, 1.0f);
                const float d = std::hypot(ux - t * dx, uy - t * dy) - r;
                blend(px, py, c, std::clamp(0.5f - d, 0.0f, 1.0f));
            }
        }
    }
    // A chevron pointing left (dir -1) or right (1), centred at x, y.
    void chevron(float x, float y, float size, int dir, float thickness, Color c) {
        line(x + dir * size * 0.25f, y, x - dir * size * 0.25f, y - size * 0.5f, thickness, c);
        line(x + dir * size * 0.25f, y, x - dir * size * 0.25f, y + size * 0.5f, thickness, c);
    }

    // `text` with its baseline at y; returns its width.
    float text(float x, float y, const std::string& text, float px, Color c, bool shadow = true) {
        Glyphs* g = glyphs(px);
        if (g == nullptr) return 0;
        if (shadow) run(*g, x + px * 0.05f, y + px * 0.06f, text, {0, 0, 0, c.a * 0.5f});
        return run(*g, x, y, text, c);
    }
    float width(const std::string& text, float px) {
        Glyphs* g = glyphs(px);
        float w = 0;
        for (unsigned char ch : text) w += g && ch >= 32 && ch < 127 ? g->chars[ch - 32].xadvance : 0;
        return w;
    }

private:
    struct Glyphs {
        int side = 0;
        std::vector<uint8_t> alpha;
        stbtt_packedchar chars[95];
    };

    static Color mix(Color a, Color b, float t) {
        return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    }

    // Colour c, covering `coverage` of the pixel, over what is there.
    void blend(int x, int y, Color c, float coverage) {
        const float a = c.a * coverage;
        if (a <= 0.002f) return;
        uint32_t& p = pixels_[size_t(y) * w_ + x];
        const float keep = 1 - a;
        const float r = c.r * a * 255 + float(p & 0xFF) * keep;
        const float g = c.g * a * 255 + float((p >> 8) & 0xFF) * keep;
        const float b = c.b * a * 255 + float((p >> 16) & 0xFF) * keep;
        const float al = a * 255 + float(p >> 24) * keep;
        p = uint32_t(r + 0.5f) | uint32_t(g + 0.5f) << 8 | uint32_t(b + 0.5f) << 16 | uint32_t(al + 0.5f) << 24;
    }

    // A rounded rectangle by its signed distance; `soft` > 0 fades its edge over that many pixels.
    void shape(float x, float y, float w, float h, float radius, float soft, Color c) {
        radius = std::min({radius, w / 2, h / 2});
        const float cx = x + w / 2, cy = y + h / 2, bx = w / 2 - radius, by = h / 2 - radius;
        const int x0 = std::max(0, int(std::floor(x))), x1 = std::min(w_, int(std::ceil(x + w)));
        const int y0 = std::max(0, int(std::floor(y))), y1 = std::min(h_, int(std::ceil(y + h)));
        for (int py = y0; py < y1; ++py) {
            const float qy = std::fabs(py + 0.5f - cy) - by;
            for (int px = x0; px < x1; ++px) {
                const float qx = std::fabs(px + 0.5f - cx) - bx;
                const float d = (qx > 0 && qy > 0 ? std::sqrt(qx * qx + qy * qy) : std::max(qx, qy)) - radius;
                float coverage;
                if (soft > 0) {
                    const float t = std::clamp(-d / soft, 0.0f, 1.0f);
                    coverage = t * t * (3 - 2 * t);
                } else {
                    coverage = std::clamp(0.5f - d, 0.0f, 1.0f);
                }
                blend(px, py, c, coverage);
            }
        }
    }

    float run(const Glyphs& g, float x, float y, const std::string& text, Color c) {
        const float start = x;
        for (unsigned char ch : text) {
            if (ch < 32 || ch >= 127) continue;
            stbtt_aligned_quad q;
            stbtt_GetPackedQuad(g.chars, g.side, g.side, ch - 32, &x, &y, &q, 0);
            const int qx0 = std::max(0, int(std::floor(q.x0))), qx1 = std::min(w_, int(std::ceil(q.x1)));
            const int qy0 = std::max(0, int(std::floor(q.y0))), qy1 = std::min(h_, int(std::ceil(q.y1)));
            const float su = (q.s1 - q.s0) * g.side / std::max(q.x1 - q.x0, 1e-3f);
            const float sv = (q.t1 - q.t0) * g.side / std::max(q.y1 - q.y0, 1e-3f);
            for (int py = qy0; py < qy1; ++py) {
                const float v = q.t0 * g.side + (py + 0.5f - q.y0) * sv - 0.5f;
                for (int px = qx0; px < qx1; ++px) {
                    const float u = q.s0 * g.side + (px + 0.5f - q.x0) * su - 0.5f;
                    blend(px, py, c, sample(g, u, v));
                }
            }
        }
        return x - start;
    }

    // The glyph atlas's coverage at texel coordinates u, v (bilinear).
    static float sample(const Glyphs& g, float u, float v) {
        const int iu = int(std::floor(u)), iv = int(std::floor(v));
        const float fu = u - iu, fv = v - iv;
        const auto at = [&](int a, int b) {
            return a < 0 || b < 0 || a >= g.side || b >= g.side ? 0.0f : g.alpha[size_t(b) * g.side + a] / 255.0f;
        };
        return (at(iu, iv) * (1 - fu) + at(iu + 1, iv) * fu) * (1 - fv) +
               (at(iu, iv + 1) * (1 - fu) + at(iu + 1, iv + 1) * fu) * fv;
    }

    void open_font() {
        for (const fs::path& p : fonts) {
            std::ifstream in(p, std::ios::binary);
            if (!in) continue;
            font_data_.assign(std::istreambuf_iterator<char>(in), {});
            if (!font_data_.empty()) return;
        }
    }

    Glyphs* glyphs(float px) {
        if (font_data_.empty()) return nullptr;
        const int key = std::max(6, static_cast<int>(std::lround(px)));
        if (auto it = cache_.find(key); it != cache_.end()) return it->second.get();
        auto g = std::make_unique<Glyphs>();
        const int oversample = key < 40 ? 2 : 1;
        for (g->side = 256; g->side <= 4096; g->side *= 2) {
            g->alpha.assign(size_t(g->side) * g->side, 0);
            stbtt_pack_context pc;
            if (!stbtt_PackBegin(&pc, g->alpha.data(), g->side, g->side, 0, 1, nullptr)) return nullptr;
            stbtt_PackSetOversampling(&pc, oversample, oversample);
            const bool ok = stbtt_PackFontRange(&pc, font_data_.data(), 0, float(key), 32, 95, g->chars);
            stbtt_PackEnd(&pc);
            if (ok) return (cache_[key] = std::move(g)).get();
        }
        return nullptr;
    }

    int w_ = 0, h_ = 0;
    std::vector<uint32_t> pixels_;
    std::vector<uint8_t> font_data_;
    std::map<int, std::unique_ptr<Glyphs>> cache_;
};

enum Row { kResume, kCounter, kCorner, kFullscreen, kFrameRate, kQuit, kRows };
constexpr const char* kLabels[kRows] = {"Resume", "FPS counter", "Counter position", "Fullscreen", "Frame rate",
                                        "Quit game"};
constexpr const char* kCounterNames[3] = {"Off", "FPS", "Detailed"};
// The launcher's choices: Automatic, 60 (no interpolation), 120, Uncapped.
constexpr const char* kRateNames[4] = {"Automatic", "60", "120", "Uncapped"};

class Overlay {
public:
    explicit Overlay(const fs::path& dir) : dir_(dir) {
        const std::vector<fs::path> fonts{dir / "fonts" / "launcher.ttf", "C:/Windows/Fonts/bahnschrift.ttf",
                                          "C:/Windows/Fonts/segoeui.ttf",
                                          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                          "/usr/share/fonts/TTF/DejaVuSans.ttf"};
        menu_.fonts = counter_.fonts = fonts;
        load();
        rate_ = rate_index(display::load(display::file_path()).settings);
        started_rate_ = rate_;
        last_ = now();
        if (std::getenv("SMASH_OVERLAY_OPEN")) open(); // screenshots of the menu
        // SMASH_FULLSCREEN_AT=seconds: go fullscreen then (measuring a windowed replay in fullscreen).
        if (const char* at = std::getenv("SMASH_FULLSCREEN_AT")) fullscreen_at_ = last_ + std::atof(at);
    }

    bool event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                if (!e.key.repeat) toggle();
                keyboard_ = true;
                return true;
            }
            if (!open_) return false;
            keyboard_ = true;
            switch (e.key.scancode) {
            case SDL_SCANCODE_UP:
            case SDL_SCANCODE_W: move(-1); return true;
            case SDL_SCANCODE_DOWN:
            case SDL_SCANCODE_S: move(1); return true;
            case SDL_SCANCODE_LEFT:
            case SDL_SCANCODE_A: change(selected_, -1); return true;
            case SDL_SCANCODE_RIGHT:
            case SDL_SCANCODE_D: change(selected_, 1); return true;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
            case SDL_SCANCODE_SPACE:
                if (!e.key.repeat) activate(selected_);
                return true;
            case SDL_SCANCODE_BACKSPACE: close(); return true;
            default: return false; // F11 and the rest still work
            }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                toggle();
                keyboard_ = false;
                return true;
            }
            if (!open_) return false;
            keyboard_ = false;
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: move(-1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move(1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: change(selected_, -1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: change(selected_, 1); break;
            case SDL_GAMEPAD_BUTTON_EAST: activate(selected_); break; // the Wii U's A
            case SDL_GAMEPAD_BUTTON_SOUTH:                            // and B
            case SDL_GAMEPAD_BUTTON_START: close(); break;
            default: break;
            }
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            if (!open_) return false;
            hover(e.motion.x, e.motion.y, e.motion.windowID);
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!open_) return false;
            if (e.button.button == SDL_BUTTON_LEFT) click(e.button.x, e.button.y, e.button.windowID);
            return true;
        default: return false;
        }
    }

    bool captures() const { return open_ || wait_release_; }

    void frame() {
        const double t = now();
        const float dt = static_cast<float>(std::min(t - last_, 0.1));
        last_ = t;
        if (fullscreen_at_ > 0 && t >= fullscreen_at_) {
            fullscreen_at_ = 0;
            host::set_fullscreen(true);
        }
        if (wait_release_ && !anything_held()) wait_release_ = false;
        if (open_) poll_stick(t);

        uint32_t w = 0, h = 0;
        host::drawable_size(w, h);
        bool redraw = dirty_ || w != width_ || h != height_;
        width_ = w;
        height_ = h;
        const float target = open_ ? 1.0f : 0.0f;
        if (std::fabs(shown_ - target) > 0.002f || std::fabs(row_y_ - selected_) > 0.002f) redraw = true;
        shown_ = std::fabs(shown_ - target) > 0.002f ? ease(shown_, target, dt, 16.0f) : target;
        row_y_ = std::fabs(row_y_ - selected_) > 0.002f ? ease(row_y_, float(selected_), dt, 22.0f) : float(selected_);
        if (t - stats_at_ >= 0.5) {
            stats_at_ = t;
            stats_ = cafe::gpu::live_stats();
            if (counter_mode_ != 0 || shown_ > 0) redraw = true;
        }
        if (!redraw) return;
        dirty_ = false;

        std::vector<cafe::gpu::OverlayLayer> layers;
        if (counter_mode_ != 0 && w > 0) {
            if (auto layer = draw_counter(); layer.width) layers.push_back(std::move(layer));
        }
        if (shown_ > 0.002f && w > 0) {
            if (auto layer = draw_menu(); layer.width) layers.push_back(std::move(layer));
        }
        cafe::gpu::set_overlay(0.55f * shown_, std::move(layers));
        if (std::getenv("SMASH_OVERLAY_TIMING")) std::fprintf(stderr, "overlay: drawn in %.1f ms\n", (now() - t) * 1000);
    }

private:
    // ------------------------------------------------------------ controls
    void toggle() { open_ ? close() : open(); }

    void open() {
        open_ = true;
        quit_armed_ = false;
        selected_ = kResume;
        row_y_ = float(selected_);
        dirty_ = true;
    }

    void close() {
        open_ = false;
        quit_armed_ = false;
        wait_release_ = true; // the press that closed it is not the game's
        dirty_ = true;
    }

    void move(int d) {
        selected_ = (selected_ + d + kRows) % kRows;
        quit_armed_ = false;
        dirty_ = true;
    }

    void activate(int row) {
        switch (row) {
        case kResume: close(); break;
        case kQuit:
            if (quit_armed_) host::request_quit();
            quit_armed_ = true;
            break;
        default: change(row, 1); break;
        }
        dirty_ = true;
    }

    void change(int row, int d) {
        switch (row) {
        case kCounter:
            counter_mode_ = (counter_mode_ + d + 3) % 3;
            save();
            break;
        case kCorner:
            right_ = !right_;
            save();
            break;
        case kFullscreen: host::set_fullscreen(!host::fullscreen()); break;
        case kFrameRate: {
            rate_ = (rate_ + d + 4) % 4;
            display::Settings s = display::load(display::file_path()).settings;
            s.interpolation = rate_ != 1;
            s.frame_rate = rate_ == 2 ? 120 : rate_ == 3 ? display::kUncapped : 0;
            std::ofstream(display::file_path(), std::ios::binary) << display::format(s);
            break;
        }
        default: return;
        }
        dirty_ = true;
    }

    static int rate_index(const display::Settings& s) {
        if (!s.interpolation) return 1;
        if (s.frame_rate == 120) return 2;
        if (s.frame_rate == display::kUncapped) return 3;
        return 0;
    }

    // The left stick moves the selection, repeating while held.
    void poll_stick(double t) {
        float y = 0, x = 0;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count; ++i) {
            if (SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i])) {
                const float py = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
                const float px = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
                if (std::fabs(py) > std::fabs(y)) y = py;
                if (std::fabs(px) > std::fabs(x)) x = px;
            }
        }
        SDL_free(ids);
        const int dir = std::fabs(y) > 0.6f && std::fabs(y) >= std::fabs(x) ? (y > 0 ? 2 : -2)
                        : std::fabs(x) > 0.6f                                ? (x > 0 ? 1 : -1)
                                                                              : 0;
        if (dir == 0) {
            stick_dir_ = 0;
            return;
        }
        if (dir == stick_dir_ && t < stick_next_) return;
        stick_next_ = t + (dir == stick_dir_ ? 0.12 : 0.38);
        stick_dir_ = dir;
        keyboard_ = false;
        if (dir == 2 || dir == -2) move(dir / 2);
        else change(selected_, dir);
    }

    static bool anything_held() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        bool held = false;
        for (int i = 0; i < count && !held; ++i) {
            SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
            for (int b = 0; pad && b < SDL_GAMEPAD_BUTTON_COUNT && !held; ++b) {
                held = SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
            }
        }
        SDL_free(ids);
        int keys = 0;
        const bool* state = SDL_GetKeyboardState(&keys);
        for (int k = 0; k < keys && !held; ++k) held = state[k];
        return held;
    }

    // ------------------------------------------------------------ mouse
    // A point in window units to the window's pixels.
    static void to_pixels(SDL_WindowID id, float& x, float& y) {
        if (SDL_Window* w = SDL_GetWindowFromID(id)) {
            const float density = SDL_GetWindowPixelDensity(w);
            x *= density;
            y *= density;
        }
    }

    int row_at(float x, float y) const {
        const float u = unit();
        const float px = panel_x(), py = panel_y();
        if (x < px || x > px + panel_w() || y < py + rows_top() * u) return -1;
        const int row = static_cast<int>((y - py - rows_top() * u) / (kRowHeight * u));
        return row >= 0 && row < kRows ? row : -1;
    }

    void hover(float x, float y, SDL_WindowID id) {
        to_pixels(id, x, y);
        if (const int row = row_at(x, y); row >= 0 && row != selected_) {
            selected_ = row;
            quit_armed_ = false;
            dirty_ = true;
        }
        keyboard_ = true;
    }

    void click(float x, float y, SDL_WindowID id) {
        to_pixels(id, x, y);
        const bool inside = x >= panel_x() && x <= panel_x() + panel_w() && y >= panel_y() && y <= panel_y() + panel_h();
        if (!inside) {
            close();
            return;
        }
        if (const int row = row_at(x, y); row >= 0) {
            selected_ = row;
            // The left half of a value steps back, the rest forward.
            if (row != kResume && row != kQuit && x < panel_x() + panel_w() - 150 * unit()) change(row, -1);
            else activate(row);
        }
    }

    // ------------------------------------------------------------ settings
    void load() {
        std::ifstream in(dir_ / "overlay.ini");
        for (std::string line; std::getline(in, line);) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '#') continue;
            std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            const auto trim = [](std::string& s) {
                s.erase(0, s.find_first_not_of(" \t\r"));
                s.erase(s.find_last_not_of(" \t\r") + 1);
            };
            trim(key);
            trim(value);
            if (key == "fps_counter") counter_mode_ = value == "fps" ? 1 : value == "detailed" ? 2 : 0;
            if (key == "counter_corner") right_ = value == "right";
        }
    }

    void save() const {
        std::ofstream out(dir_ / "overlay.ini", std::ios::binary);
        out << "# Smash 4 PC's in-game overlay (Esc or a pad's Home button), written by the overlay.\n"
            << "fps_counter = " << (counter_mode_ == 1 ? "fps" : counter_mode_ == 2 ? "detailed" : "off")
            << "  # off, fps or detailed\n"
            << "counter_corner = " << (right_ ? "right" : "left") << "  # left or right (top)\n";
    }

    // ------------------------------------------------------------ drawing
    static constexpr float kRowHeight = 62;
    float unit() const { return std::max(0.5f, height_ / 1080.0f); }
    float rows_top() const { return 132; }
    float panel_w() const { return std::min(760 * unit(), width_ - 32.0f); }
    float panel_h() const { return (rows_top() + float(kRows) * kRowHeight + 92) * unit(); }
    float panel_x() const { return std::round((width_ - panel_w()) / 2); }
    float panel_y() const { return std::round((height_ - panel_h()) / 2); }

    Color health() const {
        return stats_.game_fps >= 58.5f ? kGood : stats_.game_fps >= 45.0f ? kGold : kBad;
    }

    cafe::gpu::OverlayLayer draw_counter() {
        const float u = unit();
        const bool detailed = counter_mode_ == 2;
        const int w = static_cast<int>(std::ceil((detailed ? 290 : 150) * u));
        const int h = static_cast<int>(std::ceil((detailed ? 156 : 58) * u));
        if (!counter_.begin(w, h)) return {};
        Canvas& p = counter_;
        p.round_rect(0, 0, float(w), float(h), 12 * u, {0.04f, 0.05f, 0.07f, 0.74f});
        p.rect(12 * u, 0, w - 24 * u, 2.5f * u, with_alpha(kGold, 0), kGold, kGold, with_alpha(kGold, 0));
        const std::string fps = std::to_string(static_cast<int>(std::lround(stats_.shown_fps)));
        const float big = 34 * u;
        const float fw = p.text(16 * u, 42 * u, fps, big, health());
        p.text(16 * u + fw + 8 * u, 42 * u, "FPS", 17 * u, kDim);
        if (detailed) {
            char line[64];
            const float x = 16 * u, size = 17 * u;
            float y = 74 * u;
            const auto row = [&](const char* name, const char* value) {
                p.text(x, y, name, size, kDim, false);
                p.text(w - 16 * u - p.width(value, size), y, value, size, kWhite, false);
                y += 24 * u;
            };
            std::snprintf(line, sizeof line, "%.1f fps", stats_.game_fps);
            row("Game", line);
            std::snprintf(line, sizeof line, "%.1f ms", stats_.worst_ms);
            row("Slowest frame", line);
            std::snprintf(line, sizeof line, "%.1f ms", stats_.gpu_ms);
            row("GPU per frame", line);
            std::snprintf(line, sizeof line, "%.0f", stats_.draws);
            row("Draws", line);
        }
        const int margin = static_cast<int>(20 * u);
        return counter_.end(right_ ? static_cast<int>(width_) - w - margin : margin, margin);
    }

    std::string value_of(int row) const {
        switch (row) {
        case kCounter: return kCounterNames[counter_mode_];
        case kCorner: return right_ ? "Top right" : "Top left";
        case kFullscreen: return host::fullscreen() ? "On" : "Off";
        case kFrameRate: return kRateNames[rate_];
        default: return {};
        }
    }

    // Four buttons in a diamond, `lit` (0 top, 1 right, 2 bottom, 3 left) filled.
    static void button_icon(Canvas& p, float cx, float cy, float u, int lit, float a) {
        const float d = 7 * u, rad = 3.6f * u;
        const float xs[4] = {cx, cx + d, cx, cx - d}, ys[4] = {cy - d, cy, cy + d, cy};
        for (int i = 0; i < 4; ++i) {
            p.round_rect(xs[i] - rad, ys[i] - rad, 2 * rad, 2 * rad, rad, i == lit ? with_alpha(kGold, a) : Color{1, 1, 1, 0.28f * a});
        }
    }

    cafe::gpu::OverlayLayer draw_menu() {
        // Drawn opaque; the renderer fades it in (OverlayLayer::opacity).
        const float u = unit(), a = 1;
        // Room for the drop shadow around the panel.
        const float pad = 24 * u;
        const float pw = panel_w(), ph = panel_h();
        const int w = static_cast<int>(std::ceil(pw + 2 * pad)), h = static_cast<int>(std::ceil(ph + 2 * pad));
        const float x0 = pad, y0 = pad;
        // What does not change while the menu is open, drawn once.
        const auto key = std::make_tuple(w, h, keyboard_);
        if (key != base_key_ || base_.empty()) {
            if (!menu_.begin(w, h)) return {};
            Canvas& p = menu_;
            p.shadow(x0, y0 + 8 * u, pw, ph, 22 * u, 16 * u, {0, 0, 0, 0.5f * a});
            p.round_rect(x0, y0, pw, ph, 22 * u, {0.065f, 0.07f, 0.095f, 0.96f * a});
            p.rect(x0, y0 + 30 * u, pw, 90 * u, {1, 1, 1, 0.025f * a}, {1, 1, 1, 0.025f * a}, {1, 1, 1, 0}, {1, 1, 1, 0});
            p.rect(x0 + 24 * u, y0, pw - 48 * u, 3 * u, with_alpha(kGold, 0), with_alpha(kGold, a), with_alpha(kGold, a),
                   with_alpha(kGold, 0));
            // Header: the name.
            p.text(x0 + 36 * u, y0 + 66 * u, "SMASH 4 PC", 38 * u, with_alpha(kWhite, a));
            p.text(x0 + 38 * u, y0 + 96 * u, "Game menu", 19 * u, with_alpha(kDim, a), false);
            p.rect(x0 + 36 * u, y0 + 116 * u, pw - 72 * u, std::max(1.0f, 1.5f * u), {1, 1, 1, 0.08f * a});
            // Footer: what the buttons do.
            const float fy = y0 + ph - 46 * u;
            p.rect(x0 + 36 * u, fy - 22 * u, pw - 72 * u, std::max(1.0f, 1.5f * u), {1, 1, 1, 0.08f * a});
            float hx = x0 + 40 * u;
            const float hs = 18 * u;
            const auto hint = [&](const std::string& key, int lit, const char* what) {
                if (lit >= 0) {
                    button_icon(p, hx + 8 * u, fy, u, lit, a);
                    hx += 24 * u;
                } else {
                    const float kw = p.width(key, hs) + 16 * u;
                    p.round_rect(hx, fy - 14 * u, kw, 28 * u, 7 * u, {1, 1, 1, 0.10f * a});
                    p.text(hx + 8 * u, fy + 6 * u, key, hs, with_alpha(kWhite, a), false);
                    hx += kw + 10 * u;
                }
                hx += p.text(hx, fy + 6 * u, what, hs, with_alpha(kDim, a), false) + 30 * u;
            };
            if (keyboard_) {
                hint("Enter", -1, "Select");
                hint("Esc", -1, "Close");
            } else {
                hint("", 1, "Select");
                hint("", 2, "Back");
            }
            // Left and right change a setting.
            p.chevron(hx + 4 * u, fy, 12 * u, -1, 2.2f * u, with_alpha(kWhite, 0.8f * a));
            p.chevron(hx + 20 * u, fy, 12 * u, 1, 2.2f * u, with_alpha(kWhite, 0.8f * a));
            p.text(hx + 36 * u, fy + 6 * u, "Change", hs, with_alpha(kDim, a), false);
            base_ = menu_.pixels();
            base_key_ = key;
        }
        menu_.begin_from(base_, w, h);
        Canvas& p = menu_;

        // How the game runs now.
        char chip[48];
        std::snprintf(chip, sizeof chip, "%d FPS", static_cast<int>(std::lround(stats_.shown_fps)));
        const float cw = p.width(chip, 18 * u) + 30 * u;
        const float cx = x0 + pw - 36 * u - cw, cy = y0 + 42 * u;
        p.round_rect(cx, cy, cw, 34 * u, 17 * u, {1, 1, 1, 0.06f * a});
        p.round_rect(cx + 12 * u, cy + 13 * u, 8 * u, 8 * u, 4 * u, with_alpha(health(), a));
        p.text(cx + 24 * u, cy + 24 * u, chip, 18 * u, with_alpha(kWhite, a), false);
        p.rect(x0 + 36 * u, y0 + 116 * u, pw - 72 * u, std::max(1.0f, 1.5f * u), {1, 1, 1, 0.08f * a});

        // Rows, with the highlight gliding to the selected one.
        const float top = y0 + rows_top() * u, rh = kRowHeight * u;
        const float hy = top + row_y_ * rh;
        p.round_rect(x0 + 18 * u, hy + 3 * u, pw - 36 * u, rh - 6 * u, 13 * u, {1, 1, 1, 0.075f * a});
        p.round_rect(x0 + 18 * u, hy + 15 * u, 4 * u, rh - 30 * u, 2 * u, with_alpha(kGold, a));
        for (int row = 0; row < kRows; ++row) {
            const float y = top + row * rh;
            const bool sel = row == selected_;
            std::string label = kLabels[row];
            Color lc = sel ? kWhite : kDim;
            if (row == kQuit && quit_armed_) {
                label = "Press again to quit";
                lc = kBad;
            }
            p.text(x0 + 44 * u, y + rh / 2 + 9 * u, label, 25 * u, with_alpha(lc, a), sel);
            const std::string value = value_of(row);
            if (value.empty()) continue;
            const float vs = 23 * u;
            const float right = x0 + pw - 52 * u;
            const float vw = p.width(value, vs);
            p.text(right - vw, y + rh / 2 + 8 * u, value, vs, with_alpha(sel ? kGold : kWhite, a), sel);
            if (sel) {
                p.chevron(right - vw - 18 * u, y + rh / 2, 13 * u, -1, 2.5f * u, with_alpha(kWhite, 0.8f * a));
                p.chevron(right + 18 * u, y + rh / 2, 13 * u, 1, 2.5f * u, with_alpha(kWhite, 0.8f * a));
            }
            if (row == kFrameRate && rate_ != started_rate_) {
                const char* note = "from the next start";
                p.text(right - vw - 40 * u - p.width(note, 16 * u), y + rh / 2 + 7 * u, note, 16 * u,
                       with_alpha(kDim, 0.8f * a), false);
            }
        }

        auto layer = menu_.end(static_cast<int>(panel_x() - pad),
                               static_cast<int>(panel_y() - pad + (1 - shown_) * 28 * u)); // sliding up as it opens
        layer.opacity = shown_;
        return layer;
    }

    fs::path dir_;
    Canvas menu_, counter_;
    bool open_ = false, wait_release_ = false, quit_armed_ = false, keyboard_ = true, dirty_ = true, right_ = false;
    int selected_ = kResume, counter_mode_ = 0, rate_ = 0, started_rate_ = 0, stick_dir_ = 0;
    float shown_ = 0, row_y_ = 0;
    double last_ = 0, stats_at_ = 0, stick_next_ = 0, fullscreen_at_ = 0;
    uint32_t width_ = 0, height_ = 0;
    cafe::gpu::LiveStats stats_;
    std::vector<uint32_t> base_; // the menu's unchanging part
    std::tuple<int, int, bool> base_key_{};
};

} // namespace

// After the window opens; the event loop then drives it.
void start_overlay(const fs::path& dir) {
    static Overlay overlay(dir);
    host::set_overlay_hooks({
        [](const void* e) { return overlay.event(*static_cast<const SDL_Event*>(e)); },
        [] { overlay.frame(); },
        [] { return overlay.captures(); },
    });
}

} // namespace smash
