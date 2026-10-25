// A backend that renders nothing. Used until the Vulkan renderer exists,
// and to run the game headless. Reports what the GPU was asked to do every
// ten seconds so progress through the title is visible.

#include "gpu/backend.h"

#include <chrono>
#include <cstdio>

namespace cafe::gpu {
namespace {

class NullBackend final : public Backend {
public:
    const char* name() const override { return "null (no rendering)"; }
    void draw(const Registers&, const Draw&) override { ++draws_; }
    void clear_color(const Registers&, const gx2::ColorBuffer&, const float[4]) override { ++clears_; }
    void clear_depth_stencil(const Registers&, const gx2::DepthBuffer&, uint32_t, float, uint32_t) override {
        ++clears_;
    }
    void copy_surface(const gx2::Surface&, uint32_t, uint32_t, const gx2::Surface&, uint32_t, uint32_t) override {
        ++copies_;
    }
    void resolve_color(const gx2::ColorBuffer&, const gx2::Surface&, uint32_t, uint32_t) override { ++copies_; }
    void expand_depth(const gx2::DepthBuffer&) override {}
    void convert_depth(const gx2::DepthBuffer&, const gx2::Surface&, uint32_t, uint32_t) override { ++copies_; }
    void copy_to_scan_buffer(const gx2::ColorBuffer&, uint32_t) override {}
    void invalidate(uint32_t, uint32_t, uint32_t) override {}

    void swap() override {
        ++frames_;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - window_start_;
        if (elapsed >= std::chrono::seconds(10)) {
            const double seconds = std::chrono::duration<double>(elapsed).count();
            std::fprintf(stderr, "ttt2: gpu: %.1f frames/s, %.0f draws, %.0f clears, %.0f copies per frame\n",
                         frames_ / seconds, double(draws_) / frames_, double(clears_) / frames_,
                         double(copies_) / frames_);
            window_start_ = now;
            frames_ = draws_ = clears_ = copies_ = 0;
        }
    }

private:
    std::chrono::steady_clock::time_point window_start_ = std::chrono::steady_clock::now();
    uint64_t frames_ = 0, draws_ = 0, clears_ = 0, copies_ = 0;
};

} // namespace

std::unique_ptr<Backend> make_null_backend() { return std::make_unique<NullBackend>(); }

} // namespace cafe::gpu
