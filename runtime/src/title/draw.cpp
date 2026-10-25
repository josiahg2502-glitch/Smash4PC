#include "title/draw.h"

#include <algorithm>
#include <cmath>

namespace cafe::title::draw {
namespace {

constexpr uint32_t kSamples = 4; // per pixel, each way

Point sample_point(uint32_t sx, uint32_t sy) { return {(sx + 0.5) / kSamples, (sy + 0.5) / kSamples}; }

// Box blur of a width x height field, in place.
void blur(std::vector<double>& field, uint32_t width, uint32_t height, int radius) {
    std::vector<double> line;
    const auto pass = [&](uint32_t count, uint32_t length, size_t step, size_t stride) {
        line.resize(length);
        for (uint32_t c = 0; c < count; ++c) {
            double* v = field.data() + c * stride;
            for (uint32_t i = 0; i < length; ++i) line[i] = v[i * step];
            for (uint32_t i = 0; i < length; ++i) {
                double sum = 0;
                for (int k = -radius; k <= radius; ++k) {
                    const int64_t j = int64_t{i} + k;
                    if (j >= 0 && j < length) sum += line[static_cast<size_t>(j)];
                }
                v[i * step] = sum / (2 * radius + 1);
            }
        }
    };
    pass(height, width, 1, width);
    pass(width, height, width, 1);
}

} // namespace

Colour Colour::rgb(uint32_t rgb, double alpha) {
    return {((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha};
}

Shape::Shape(uint32_t width, uint32_t height)
    : width_(width * kSamples), height_(height * kSamples), covered_(size_t{width_} * height_, 0) {}

Shape Shape::circle(uint32_t width, uint32_t height, Point c, double radius) {
    Shape s(width, height);
    for (uint32_t y = 0; y < s.height_; ++y) {
        for (uint32_t x = 0; x < s.width_; ++x) {
            const Point p = sample_point(x, y);
            s.covered_[size_t{y} * s.width_ + x] = std::hypot(p.x - c.x, p.y - c.y) <= radius;
        }
    }
    return s;
}

// Even-odd rule.
Shape Shape::polygon(uint32_t width, uint32_t height, const std::vector<Point>& points) {
    Shape s(width, height);
    for (uint32_t y = 0; y < s.height_; ++y) {
        for (uint32_t x = 0; x < s.width_; ++x) {
            const Point p = sample_point(x, y);
            bool inside = false;
            for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
                const Point& a = points[i];
                const Point& b = points[j];
                if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
            }
            s.covered_[size_t{y} * s.width_ + x] = inside;
        }
    }
    return s;
}

// Round-capped.
Shape Shape::line(uint32_t width, uint32_t height, Point a, Point b, double thickness) {
    Shape s(width, height);
    const double dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
    for (uint32_t y = 0; y < s.height_; ++y) {
        for (uint32_t x = 0; x < s.width_; ++x) {
            const Point p = sample_point(x, y);
            const double t = length2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length2, 0.0, 1.0) : 0;
            s.covered_[size_t{y} * s.width_ + x] = std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy)) <= thickness / 2;
        }
    }
    return s;
}

Shape Shape::rounded_rect(uint32_t width, uint32_t height, Point tl, Point br, double radius) {
    Shape s(width, height);
    for (uint32_t y = 0; y < s.height_; ++y) {
        for (uint32_t x = 0; x < s.width_; ++x) {
            const Point p = sample_point(x, y);
            const double qx = std::max({tl.x + radius - p.x, p.x - (br.x - radius), 0.0});
            const double qy = std::max({tl.y + radius - p.y, p.y - (br.y - radius), 0.0});
            const bool inside = p.x >= tl.x && p.x <= br.x && p.y >= tl.y && p.y <= br.y;
            s.covered_[size_t{y} * s.width_ + x] = inside && std::hypot(qx, qy) <= radius;
        }
    }
    return s;
}

Shape Shape::outline(double thickness) const {
    Shape s = *this;
    const int r = static_cast<int>(std::lround(thickness * kSamples));
    for (uint32_t y = 0; y < height_; ++y) {
        for (uint32_t x = 0; x < width_; ++x) {
            if (!covered_[size_t{y} * width_ + x]) continue;
            bool edge = false;
            for (int dy = -r; dy <= r && !edge; ++dy) {
                for (int dx = -r; dx <= r && !edge; ++dx) {
                    if (dx * dx + dy * dy > r * r) continue;
                    const int64_t nx = int64_t{x} + dx, ny = int64_t{y} + dy;
                    edge = nx < 0 || ny < 0 || nx >= width_ || ny >= height_ ||
                           !covered_[static_cast<size_t>(ny) * width_ + static_cast<size_t>(nx)];
                }
            }
            s.covered_[size_t{y} * width_ + x] = edge;
        }
    }
    return s;
}

Shape Shape::operator|(const Shape& other) const {
    Shape s = *this;
    for (size_t i = 0; i < covered_.size(); ++i) s.covered_[i] = covered_[i] || other.covered_[i];
    return s;
}

Shape Shape::operator-(const Shape& other) const {
    Shape s = *this;
    for (size_t i = 0; i < covered_.size(); ++i) s.covered_[i] = covered_[i] && !other.covered_[i];
    return s;
}

Canvas::Canvas(uint32_t width, uint32_t height)
    : width_(width), height_(height), rgba_(size_t{width} * kSamples * height * kSamples * 4, 0.0) {}

void Canvas::paint_sample(size_t i, const Colour& c, double coverage) {
    const double a = c.a * coverage;
    double* p = &rgba_[i * 4];
    p[0] = c.r * a + p[0] * (1 - a);
    p[1] = c.g * a + p[1] * (1 - a);
    p[2] = c.b * a + p[2] * (1 - a);
    p[3] = a + p[3] * (1 - a);
}

void Canvas::fill(const Shape& shape, const Paint& paint) {
    const uint32_t w = width_ * kSamples, h = height_ * kSamples;
    uint32_t top = h, bottom = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            if (shape.covered_[size_t{y} * w + x]) {
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    for (uint32_t y = top; y <= bottom && y < h; ++y) {
        const double t = bottom > top ? static_cast<double>(y - top) / (bottom - top) : 0;
        const Colour c{paint.top.r + (paint.bottom.r - paint.top.r) * t, paint.top.g + (paint.bottom.g - paint.top.g) * t,
                       paint.top.b + (paint.bottom.b - paint.top.b) * t, paint.top.a + (paint.bottom.a - paint.top.a) * t};
        for (uint32_t x = 0; x < w; ++x) {
            if (shape.covered_[size_t{y} * w + x]) paint_sample(size_t{y} * w + x, c, 1.0);
        }
    }
}

void Canvas::glow(const Shape& shape, double radius, const Colour& colour) {
    const uint32_t w = width_ * kSamples, h = height_ * kSamples;
    std::vector<double> field(shape.covered_.begin(), shape.covered_.end());
    const int r = std::max(1, static_cast<int>(std::lround(radius * kSamples / 2)));
    blur(field, w, h, r);
    blur(field, w, h, r);
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] > 0) paint_sample(i, colour, std::min(1.0, field[i] * 1.5));
    }
}

void Canvas::picture(const Rgba& image, double x, double y, double w, double h) {
    const uint32_t sw = static_cast<uint32_t>(std::lround(w * kSamples)), sh = static_cast<uint32_t>(std::lround(h * kSamples));
    if (sw == 0 || sh == 0) return;
    const Rgba scaled = resize(image, sw, sh);
    const int64_t ox = std::lround(x * kSamples), oy = std::lround(y * kSamples);
    const uint32_t cw = width_ * kSamples, ch = height_ * kSamples;
    for (uint32_t j = 0; j < sh; ++j) {
        for (uint32_t i = 0; i < sw; ++i) {
            const int64_t px = ox + i, py = oy + j;
            if (px < 0 || py < 0 || px >= cw || py >= ch) continue;
            const uint8_t* s = scaled.at(i, j);
            paint_sample(static_cast<size_t>(py) * cw + static_cast<size_t>(px),
                         Colour{s[0] / 255.0, s[1] / 255.0, s[2] / 255.0, 1.0}, s[3] / 255.0);
        }
    }
}

Rgba Canvas::result() const {
    Rgba out = blank(width_, height_);
    const uint32_t w = width_ * kSamples;
    for (uint32_t y = 0; y < height_; ++y) {
        for (uint32_t x = 0; x < width_; ++x) {
            double sum[4] = {};
            for (uint32_t j = 0; j < kSamples; ++j) {
                for (uint32_t i = 0; i < kSamples; ++i) {
                    const double* p = &rgba_[((size_t{y} * kSamples + j) * w + x * kSamples + i) * 4];
                    for (int k = 0; k < 4; ++k) sum[k] += p[k];
                }
            }
            const double n = kSamples * kSamples, a = sum[3] / n;
            uint8_t* o = out.at(x, y);
            for (int k = 0; k < 3; ++k) {
                o[k] = static_cast<uint8_t>(std::clamp(a > 0 ? sum[k] / n / a * 255.0 + 0.5 : 0.0, 0.0, 255.0));
            }
            o[3] = static_cast<uint8_t>(std::clamp(a * 255.0 + 0.5, 0.0, 255.0));
        }
    }
    return out;
}

} // namespace cafe::title::draw
