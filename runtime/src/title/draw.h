#pragma once

// Drawing small icons: shapes at sixteen samples per pixel, painted over
// one another, then averaged down to an RGBA image.

#include "title/image.h"

#include <cstdint>
#include <vector>

namespace cafe::title::draw {

struct Point {
    double x = 0, y = 0;
};

struct Colour {
    double r = 0, g = 0, b = 0, a = 1; // 0 to 1
    static Colour rgb(uint32_t rgb, double alpha = 1.0);
};

// A colour, or one from top to bottom (over the shape's own height).
struct Paint {
    Colour top, bottom;
    bool gradient = false;
    Paint(Colour c) : top(c), bottom(c) {} // NOLINT: a plain colour paints
    Paint(Colour t, Colour b) : top(t), bottom(b), gradient(true) {}
};

// Which samples a shape covers.
class Shape {
public:
    Shape(uint32_t width, uint32_t height);
    static Shape circle(uint32_t width, uint32_t height, Point centre, double radius);
    static Shape polygon(uint32_t width, uint32_t height, const std::vector<Point>& points);
    static Shape line(uint32_t width, uint32_t height, Point a, Point b, double thickness);
    static Shape rounded_rect(uint32_t width, uint32_t height, Point top_left, Point bottom_right, double radius);
    // A band `thickness` wide along the shape's edge, inside it.
    Shape outline(double thickness) const;
    Shape operator|(const Shape& other) const;
    Shape operator-(const Shape& other) const;

private:
    friend class Canvas;
    uint32_t width_, height_; // in samples
    std::vector<uint8_t> covered_;
};

class Canvas {
public:
    Canvas(uint32_t width, uint32_t height); // in pixels
    void fill(const Shape& shape, const Paint& paint);
    // A soft glow around the shape, `radius` pixels.
    void glow(const Shape& shape, double radius, const Colour& colour);
    // `image` scaled into the box at (x, y), w x h pixels.
    void picture(const Rgba& image, double x, double y, double w, double h);
    Rgba result() const;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    void paint_sample(size_t i, const Colour& c, double coverage);
    uint32_t width_, height_;
    std::vector<double> rgba_; // premultiplied, per sample
};

} // namespace cafe::title::draw
