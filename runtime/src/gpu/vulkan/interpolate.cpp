// Frame interpolation: frames shown between the frames the title renders.
//
// The title's logic advances 60 steps a second and renders one frame per
// step; physics, hitboxes and timing belong to those steps and are left
// alone. With frames_per_frame_ = K > 1 the renderer shows K frames per
// step: the command processor hands it each finished frame's commands
// again (Backend::begin_replay) K - 1 times, and every replayed draw reads
// its constants (uniform blocks and buffers: camera, object transforms, the
// characters' bone matrices) blended between the previous frame's values
// and this frame's. The frames go to the display in order, the extra ones
// first, the real one last (present.cpp).
//
// While a frame renders, every draw records a key (its shaders, targets,
// vertex count and first texture) and a copy of the constant data it
// reads. A replay matches each draw to the previous frame's draw with the
// same key and the nearest data (begin_replay), and blends word by
// word: values that look like ordinary floats in both frames move
// linearly, anything else (integers, flags, NaNs) takes this frame's
// value, and so do small values on a coarse grid in both frames (whole
// numbers and multiples of 1/64: indices kept as floats, and the cell of a
// sprite sheet a flipbook shows, such as the school stage's crowd), which
// step from one to the next rather than move. Each block is read in the byte
// order in which its values look like floats. Only depth-tested draws, the
// 3D scene, blend. When too few draws match (a new scene), the extra
// frames show this frame as it is.

#include "gpu/vulkan/renderer.h"

#include "gpu/latte.h"
#include "host/display_settings.h"
#include "host/window.h"

#include "cafe/runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>

namespace cafe::gpu::vk {
namespace {

using namespace latte;

constexpr size_t kMaxRecordedBytes = 64u << 20; // per frame

uint64_t fnv(const uint32_t* words, size_t count) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < count; ++i) h = (h ^ words[i]) * 0x100000001B3ull;
    return h;
}

// A float worth blending: zero, or a normal number of moderate magnitude.
bool ordinary(uint32_t bits) {
    const uint32_t exponent = (bits >> 23) & 0xFF;
    return (bits & 0x7FFFFFFF) == 0 || (exponent >= 0x60 && exponent <= 0x9F);
}

// Whether a block's words read as floats more often byte-swapped.
bool big_endian(const uint8_t* data, uint32_t size) {
    int le = 0, be = 0;
    for (uint32_t i = 0; i + 4 <= size && i < 4096; i += 4) {
        uint32_t w;
        std::memcpy(&w, data + i, 4);
        le += ordinary(w) && w != 0;
        be += ordinary(__builtin_bswap32(w)) && w != 0;
    }
    return be > le;
}

// On the grid of whole numbers (up to 4096) or their 64ths (up to 64): only
// where a moving value lands on it by chance about once in 4096 frames or
// less, so that both frames do once in 16 million. Farther out floats are
// coarse and moving values land there often: a camera far from the origin
// (an infinite stage) has a matrix column in the tens of thousands that
// changes by hundreds a frame when it turns, and stepping one of its values
// while the rest moved threw the scene off screen (black extra frames, a
// black half of the sky).
bool on_grid(float x) {
    const float a = std::fabs(x);
    if (a <= 4096.0f && x == std::trunc(x)) return true;
    return a <= 64.0f && x * 64.0f == std::trunc(x * 64.0f);
}

void blend(uint8_t* out, const uint8_t* previous, const uint8_t* current, uint32_t size, float t, bool swapped) {
    for (uint32_t i = 0; i + 4 <= size; i += 4) {
        uint32_t a, b;
        std::memcpy(&a, previous + i, 4);
        std::memcpy(&b, current + i, 4);
        uint32_t v = b;
        if (a != b) {
            const uint32_t fa = swapped ? __builtin_bswap32(a) : a, fb = swapped ? __builtin_bswap32(b) : b;
            float x, y;
            std::memcpy(&x, &fa, 4);
            std::memcpy(&y, &fb, 4);
            if (ordinary(fa) && ordinary(fb) && !(on_grid(x) && on_grid(y))) {
                const float r = x + (y - x) * t;
                uint32_t fr;
                std::memcpy(&fr, &r, 4);
                v = swapped ? __builtin_bswap32(fr) : fr;
            }
        }
        std::memcpy(out + i, &v, 4);
    }
    if (size % 4) std::memcpy(out + size / 4 * 4, current + size / 4 * 4, size % 4);
}

} // namespace

// The slot of `b` that holds what `a`'s slot `i` holds: slots are recorded in
// a fixed order (bank by bank), so draws of the same shaders line up.
static const Renderer::SlotRecord* counterpart(const Renderer::FrameRecord& b_frame, const Renderer::DrawRecord& b,
                                               const Renderer::SlotRecord& sa, uint32_t i) {
    const auto same = [&](const Renderer::SlotRecord& sb) {
        return sb.stage == sa.stage && sb.kind == sa.kind && sb.index == sa.index && sb.size == sa.size;
    };
    if (i < b.slot_count && same(b_frame.slots[b.first_slot + i])) return &b_frame.slots[b.first_slot + i];
    for (uint32_t j = 0; j < b.slot_count; ++j) {
        if (same(b_frame.slots[b.first_slot + j])) return &b_frame.slots[b.first_slot + j];
    }
    return nullptr;
}

// How much a draw's constants changed between two frames, over the first
// 256 bytes of each block (enough to tell): words that differ, words that
// changed a lot (more than a quarter of their magnitude), differing words
// that are not ordinary floats (integers, flags), and how far the floats
// moved in all (the sum of their changes).
Renderer::Difference Renderer::difference(const FrameRecord& a_frame, const DrawRecord& a, const FrameRecord& b_frame,
                                          const DrawRecord& b) {
    Difference d{};
    if (a.hash == b.hash) return d;
    for (uint32_t i = 0; i < a.slot_count; ++i) {
        const SlotRecord& sa = a_frame.slots[a.first_slot + i];
        const SlotRecord* sb = counterpart(b_frame, b, sa, i);
        if (sb == nullptr) continue;
        // Most draws share some blocks (the camera's): each pair of blocks
        // is compared once a frame.
        auto [it, fresh] = block_differences_.try_emplace((uint64_t{sb->offset} << 32) | sa.offset);
        Difference& s = it->second;
        if (fresh) {
            const uint32_t words = std::min(sa.size, 256u) / 4;
            s.compared = words;
            const uint8_t* pa = a_frame.data.mapped + sa.offset;
            const uint8_t* pb = b_frame.data.mapped + sb->offset;
            for (uint32_t w = 0; w < words && sa.hash != sb->hash; ++w) {
                uint32_t x, y;
                std::memcpy(&x, pa + w * 4, 4);
                std::memcpy(&y, pb + w * 4, 4);
                if (x == y) continue;
                ++s.changed;
                if (sa.big_endian) {
                    x = __builtin_bswap32(x);
                    y = __builtin_bswap32(y);
                }
                if (!ordinary(x) || !ordinary(y)) {
                    ++s.odd;
                    ++s.large;
                    continue;
                }
                float fx, fy;
                std::memcpy(&fx, &x, 4);
                std::memcpy(&fy, &y, 4);
                const float change = std::fabs(fx - fy);
                s.distance += change;
                if (change > 0.25f * (std::fabs(fx) + std::fabs(fy)) + 1e-3f) ++s.large;
            }
        }
        d.changed += s.changed;
        d.large += s.large;
        d.compared += s.compared;
        d.odd += s.odd;
        d.distance += s.distance;
    }
    return d;
}

void Renderer::FrameRecord::clear() {
    draws.clear();
    slots.clear();
    used = 0;
    snapshots.clear();
    overflow = false;
}

void Renderer::init_interpolation() {
    // The run's frame rate (TTT2_FPS, or 60 when display.ini turns
    // interpolation off), else smoother than the title's 60 only on displays
    // that refresh faster (the window's, or the primary one it is likely
    // moved to).
    uint32_t fps = host::display::active().fps;
    if (fps == 0) {
        const float hz = std::max(host::display_refresh_rate(), host::window_refresh_rate());
        fps = hz >= host::display::kInterpolationHz ? 120 : 60;
    }
    // Uncapped: the most extra frames, each rendered only while the GPU keeps up (want_another_replay), and
    // presented as soon as they finish, without vsync.
    uncapped_ = fps >= host::display::kUncapped;
    frames_per_frame_ = std::clamp((fps + 30) / 60, 1u, 4u);
    if (frames_per_frame_ > 1) {
        for (FrameRecord& f : frame_records_) {
            // Cached: the CPU reads it back to match and blend frames
            // (write-combined memory reads slowly).
            f.data = ctx_.create_buffer(kMaxRecordedBytes,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
    }
    if (uncapped_) {
        std::fprintf(stderr, "ttt2: GPU: uncapped (up to %u frames per frame the title renders)\n", frames_per_frame_);
    } else {
        std::fprintf(stderr, "ttt2: GPU: %u frames/s (%u per frame the title renders)\n", 60 * frames_per_frame_,
                     frames_per_frame_);
    }
}

// Every draw of a frame (or of a replay) gets the next index; a frame's
// draw remembers its key: what it is rather than where its data is (titles
// put per-frame vertex data in alternating buffers).
uint32_t Renderer::begin_draw_record(const Registers& regs, const Draw& d) {
    if (replaying_) return replay_draw_++;
    FrameRecord& f = frame_records_[current_record_];
    const auto r = [&](uint32_t address) { return regs[address]; };
    // The first pixel shader texture tells apart the many sprites drawn
    // with the same shaders and vertex counts (menus, the HUD).
    const uint32_t key_words[] = {
        r(reg::SQ_PGM_START_VS), r(reg::SQ_PGM_START_PS), r(reg::SQ_PGM_START_FS), r(reg::CB_COLOR0_BASE),
        r(reg::DB_DEPTH_BASE), r(reg::VGT_PRIMITIVE_TYPE), d.count, d.num_instances,
        r(kResourceBase + resource::kPsTexture * resource::kWords * 4 + 8),
    };
    // Only the 3D scene blends. Flat (not depth-tested) draws, menus and the
    // HUD, are shown as the frame has them: they often place text and parts
    // of a widget with vertex data the CPU writes each frame, which a replay
    // cannot blend, so blending the rest would pull a widget apart.
    static const bool flat_too = std::getenv("TTT2_INTERP_2D") != nullptr;
    const bool depth_tested = (r(reg::DB_DEPTH_CONTROL) & 2) && (r(reg::DB_DEPTH_INFO) & 7) && r(reg::DB_DEPTH_BASE);
    f.draws.push_back({fnv(key_words, std::size(key_words)), static_cast<uint32_t>(f.slots.size()), 0,
                       0xCBF29CE484222325ull, depth_tested || flat_too});
    return static_cast<uint32_t>(f.draws.size() - 1);
}

// Copies guest bytes into the frame's record, once per frame and range:
// the CPU announces what it writes for the GPU (GX2Invalidate), which drops
// the copies it overlaps (forget_snapshots).
namespace {
uint64_t content_hash(const uint8_t* data, uint32_t size) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ size;
    uint32_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t v;
        std::memcpy(&v, data + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
    }
    for (; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}
} // namespace

bool Renderer::snapshot(uint32_t address, uint32_t size, uint32_t& offset, bool& swapped, uint64_t& hash) {
    FrameRecord& f = frame_records_[current_record_];
    if (size == 0 || !guest_memory_committed(address, size)) return false;
    const uint8_t* source = guest_pointer(address);
    const uint64_t key = (uint64_t{address} << 32) | size;
    if (const auto it = f.snapshots.find(key); it != f.snapshots.end()) {
        offset = it->second.offset;
        swapped = it->second.big_endian;
        hash = it->second.hash;
        return true;
    }
    const size_t start = (f.used + 255) & ~size_t{255};
    if (start + size > f.data.size) {
        f.overflow = true;
        return false;
    }
    offset = static_cast<uint32_t>(start);
    std::memcpy(f.data.mapped + offset, source, size);
    f.used = start + size;
    swapped = big_endian(f.data.mapped + offset, size);
    hash = content_hash(f.data.mapped + offset, size);
    f.snapshots[key] = {offset, swapped, hash};
    return true;
}

namespace {
abi::Buffer* constant_table(abi::DrawConstants& dc, uint8_t stage, uint8_t kind) {
    return kind == 0 ? (stage == 0 ? dc.vs_cb : dc.ps_cb) : (stage == 0 ? dc.vs_buf : dc.ps_buf);
}
abi::Buffer descriptor(VkDeviceAddress a, uint32_t size, uint32_t stride) {
    return {static_cast<uint32_t>(a), static_cast<uint32_t>(a >> 32), size, stride};
}
} // namespace

void Renderer::record_constants(uint32_t draw_index, const ConstantSources& sources, abi::DrawConstants& dc) {
    FrameRecord& f = frame_records_[current_record_];
    if (draw_index >= f.draws.size()) return;
    DrawRecord& d = f.draws[draw_index];
    d.first_slot = static_cast<uint32_t>(f.slots.size());
    for (const ConstantSource& s : sources) {
        uint32_t offset = 0;
        bool swapped = false;
        uint64_t hash = 0;
        if (!snapshot(s.address, s.size, offset, swapped, hash)) continue;
        f.slots.push_back({s.stage, s.kind, s.index, offset, s.size, s.stride, swapped, hash});
        constant_table(dc, s.stage, s.kind)[s.index] = descriptor(f.data.address + offset, s.size, s.stride);
        d.hash = (d.hash ^ hash) * 0x100000001B3ull;
    }
    d.slot_count = static_cast<uint32_t>(f.slots.size()) - d.first_slot;
}

// The replayed draw's constants: this frame's recorded data, blended with
// the matching draw's data from the previous frame.
void Renderer::replace_constants(uint32_t draw_index, abi::DrawConstants& dc) {
    const FrameRecord& cur = frame_records_[current_record_];
    const FrameRecord& prev = frame_records_[current_record_ ^ 1];
    if (draw_index >= cur.draws.size()) return;
    const DrawRecord& d = cur.draws[draw_index];
    const bool blends = replay_blend_ && d.blend;
    const int32_t m = blends && draw_index < match_.size() ? match_[draw_index] : -1;
    for (uint32_t i = 0; i < d.slot_count; ++i) {
        const SlotRecord& s = cur.slots[d.first_slot + i];
        const uint8_t* current = cur.data.mapped + s.offset;
        const uint8_t* previous = nullptr;
        if (m >= 0) {
            const SlotRecord* ps = counterpart(prev, prev.draws[m], s, i);
            if (ps != nullptr && ps->hash != s.hash) previous = prev.data.mapped + ps->offset;
        } else if (blends) {
            // A draw without a counterpart (new in this frame) stays where
            // this frame has it, but seen as the rest is: blocks it shares
            // with draws that pair (the camera's) blend as theirs do.
            if (const auto it = shared_blocks_.find(s.offset); it != shared_blocks_.end()) {
                previous = prev.data.mapped + it->second;
            }
        }
        abi::Buffer* table = constant_table(dc, s.stage, s.kind);
        if (previous == nullptr) {
            table[s.index] = descriptor(cur.data.address + s.offset, s.size, s.stride);
            continue;
        }
        // Draws sharing data (the parts of a character) share its blend.
        const uint64_t pair = (uint64_t{static_cast<uint32_t>(previous - prev.data.mapped)} << 32) | s.offset;
        auto [it, fresh] = blended_.try_emplace(pair, Blend{0, 0});
        if (fresh) {
            VkDeviceSize offset = 0;
            uint8_t* out = upload(s.size, 256, offset);
            blend(out, previous, current, s.size, replay_t_, s.big_endian);
            it->second = {ring_.address() + offset, ring_.chunk()};
        }
        // (A submit since the upload left its chunk to the older command buffer.)
        chunks_touched_ |= 1u << it->second.chunk;
        table[s.index] = descriptor(it->second.address, s.size, s.stride);
    }
}

void Renderer::forget_snapshots(uint32_t address, uint32_t size) {
    FrameRecord& f = frame_records_[current_record_];
    if (f.snapshots.empty()) return;
    // Snapshots are at most kMaxSnapshot bytes: only those starting from
    // there before `address` can overlap.
    const uint64_t end = uint64_t{address} + size;
    const uint64_t from = address > kMaxSnapshot ? uint64_t{address - kMaxSnapshot} << 32 : 0;
    for (auto it = f.snapshots.lower_bound(from); it != f.snapshots.end() && (it->first >> 32) < end;) {
        const uint32_t a = static_cast<uint32_t>(it->first >> 32), n = static_cast<uint32_t>(it->first);
        if (uint64_t{a} + n > address) it = f.snapshots.erase(it);
        else ++it;
    }
}

// ------------------------------------------------- targets carried between frames

void Renderer::note_target_written(const Target* t) {
    if (frames_per_frame_ > 1 && !replaying_) const_cast<Target*>(t)->written_frame = frame_number_;
    queue_write_back(const_cast<Target*>(t));
}

void Renderer::note_target_read(const Target* c) {
    if (frames_per_frame_ == 1 || replaying_ || c->written_frame == frame_number_ || c->carried) return;
    // Read before this frame writes it: the previous frames' content, kept
    // for the replays.
    Target* t = const_cast<Target*>(c);
    t->carried = true;
    carried_.push_back(t);
    // Most are only read (last frame's results sampled): their start needs
    // keeping only if they were rewritten after being carried before.
    t->start_copied = t->rewrites;
    if (!t->start_copied) return;
    if (t->start_copy.image == VK_NULL_HANDLE) {
        // (Sampled only because every image gets a view.)
        const VkImageUsageFlags usage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        t->start_copy = create_image(t->image.format, t->image.aspect, t->image.width, t->image.height, 1, 1, usage,
                                     VK_IMAGE_VIEW_TYPE_2D);
        t->end_copy = create_image(t->image.format, t->image.aspect, t->image.width, t->image.height, 1, 1, usage,
                                   VK_IMAGE_VIEW_TYPE_2D);
    }
    copy_image(t->image, t->start_copy);
}

void Renderer::copy_image(const Image& src, const Image& dst) {
    end_rendering();
    barrier();
    VkImageCopy region{};
    region.srcSubresource = {src.aspect, 0, 0, 1};
    region.dstSubresource = {dst.aspect, 0, 0, 1};
    region.extent = {std::min(src.width, dst.width), std::min(src.height, dst.height), 1};
    vkCmdCopyImage(cmd(), src.image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier();
}

void Renderer::begin_replay(uint32_t n) {
    const auto started = std::chrono::steady_clock::now();
    replay_started_ = started;
    end_rendering();
    replaying_ = true;
    replayed_ = true;
    replay_draw_ = 0;
    blended_.clear();
    replay_t_ = static_cast<float>(n) / static_cast<float>(frames_per_frame_);
    // Carried targets the frame then wrote: the real frame's result aside,
    // the frame's start back. (Swaps happen after swap(), which counted the
    // frame: it is frame_number_ - 1.)
    for (Target* t : carried_) {
        if (t->written_frame + 1 != frame_number_ || !t->start_copied) continue;
        if (n == 1) copy_image(t->image, t->end_copy);
        copy_image(t->start_copy, t->image);
        t->written = stamp();
    }
    if (n != 1) return; // the match holds for every extra frame of this frame
    const FrameRecord& cur = frame_records_[current_record_];
    const FrameRecord& prev = frame_records_[current_record_ ^ 1];
    match_.assign(cur.draws.size(), -1);
    block_differences_.clear();
    shared_blocks_.clear();
    // Draws pair among those with the same key (only those that blend: the
    // 3D scene). Many share one (the patches of the tulip field, scenery
    // drawn many times over; sprites), and they come and go as the camera
    // turns, so pairing them in order would pair neighbours and draw each
    // halfway to the next. Draws whose constants did not change pair first.
    // The others pair by how far their constants moved, near their place
    // among those with their key, and only with each other's nearest: a draw
    // new in this frame has no counterpart, and pairing it with the nearest
    // other one drew it halfway between the two, and each draw down the line
    // then took its neighbour's counterpart (flickering tulips on the
    // Netherlands stage while the camera swung round). A pair whose constants
    // mostly changed a lot is not the same thing (or a cut) and is shown as
    // it is.
    struct Group {
        std::vector<uint32_t> prev;  // previous frame's draws with this key, in order
        std::vector<uint8_t> paired; // per entry of prev
        std::vector<uint32_t> cur;   // this frame's
    };
    std::unordered_map<uint64_t, Group> groups;
    std::unordered_map<uint64_t, std::deque<uint32_t>> unchanged; // (key ^ constants) -> place in its group
    for (uint32_t j = 0; j < prev.draws.size(); ++j) {
        if (!prev.draws[j].blend) continue;
        Group& g = groups[prev.draws[j].key];
        unchanged[prev.draws[j].key ^ (prev.draws[j].hash * 0x9E3779B97F4A7C15ull)].push_back(
            static_cast<uint32_t>(g.prev.size()));
        g.prev.push_back(j);
        g.paired.push_back(0);
    }
    // Counted over the draws that blend (the 3D scene): flat ones never do.
    uint32_t scene = 0, matched = 0, rejected = 0;
    for (uint32_t i = 0; i < cur.draws.size(); ++i) {
        if (!cur.draws[i].blend) continue;
        ++scene;
        const auto it = groups.find(cur.draws[i].key);
        if (it == groups.end()) continue;
        Group& g = it->second;
        g.cur.push_back(i);
        const auto same = unchanged.find(cur.draws[i].key ^ (cur.draws[i].hash * 0x9E3779B97F4A7C15ull));
        if (same == unchanged.end()) continue;
        while (!same->second.empty() && g.paired[same->second.front()]) same->second.pop_front();
        if (same->second.empty()) continue;
        const uint32_t k = same->second.front();
        same->second.pop_front();
        g.paired[k] = 1;
        match_[i] = static_cast<int32_t>(g.prev[k]);
        ++matched;
    }
    constexpr uint32_t kWindow = 16; // places searched either side
    struct Candidate {
        Difference d;
        uint32_t c, k, distance; // places in the group: this frame's draw, the previous frame's, apart
    };
    std::vector<Candidate> candidates;
    std::vector<Difference> nearest_cur, nearest_prev;
    std::vector<uint8_t> has_candidate, prev_has, done;
    for (auto& [key, g] : groups) {
        candidates.clear();
        nearest_cur.assign(g.cur.size(), Difference{});
        nearest_prev.assign(g.prev.size(), Difference{});
        has_candidate.assign(g.cur.size(), 0);
        prev_has.assign(g.prev.size(), 0);
        for (uint32_t c = 0; c < g.cur.size(); ++c) {
            const uint32_t i = g.cur[c];
            if (match_[i] >= 0) continue;
            const uint32_t from = c > kWindow ? c - kWindow : 0;
            const uint32_t to = std::min<uint32_t>(static_cast<uint32_t>(g.prev.size()), c + kWindow + 1);
            for (uint32_t k = from; k < to; ++k) {
                if (g.paired[k]) continue;
                const Difference d = difference(cur, cur.draws[i], prev, prev.draws[g.prev[k]]);
                if (!has_candidate[c] || d < nearest_cur[c]) nearest_cur[c] = d;
                if (!prev_has[k] || d < nearest_prev[k]) nearest_prev[k] = d;
                has_candidate[c] = prev_has[k] = 1;
                candidates.push_back({d, c, k, k > c ? k - c : c - k});
            }
        }
        if (candidates.empty()) continue;
        for (const uint8_t h : has_candidate) matched += h;
        // Nearest pairs first; among equals (a thing drawn twice), in order.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.d < b.d || b.d < a.d) return a.d < b.d;
            return a.distance != b.distance ? a.distance < b.distance : a.c < b.c;
        });
        done.assign(g.cur.size(), 0);
        for (const Candidate& p : candidates) {
            if (done[p.c] || g.paired[p.k]) continue;
            if (nearest_cur[p.c] < p.d || nearest_prev[p.k] < p.d) continue; // not each other's nearest
            done[p.c] = 1;
            if (p.d.large * 10 > p.d.compared) {
                ++rejected;
                continue;
            }
            g.paired[p.k] = 1;
            match_[g.cur[p.c]] = static_cast<int32_t>(g.prev[p.k]);
        }
    }
    // The blocks the paired draws blend, for draws without a counterpart
    // that share them.
    for (uint32_t i = 0; i < cur.draws.size(); ++i) {
        if (match_[i] < 0) continue;
        const DrawRecord& d = cur.draws[i];
        const DrawRecord& p = prev.draws[match_[i]];
        for (uint32_t s = 0; s < d.slot_count; ++s) {
            const SlotRecord& cs = cur.slots[d.first_slot + s];
            const SlotRecord* ps = counterpart(prev, p, cs, s);
            if (ps != nullptr && ps->hash != cs.hash) shared_blocks_.try_emplace(cs.offset, ps->offset);
        }
    }
    static const bool test = std::getenv("TTT2_INTERP_TEST") != nullptr; // replay without blending
    replay_blend_ = !test && !cur.overflow && !prev.overflow && scene > 0 && matched * 10 >= scene * 8 &&
                    rejected * 4 <= matched;
    ++replays_;
    if (replay_blend_) ++blended_replays_;
    match_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    static const bool trace = std::getenv("TTT2_TRACE_INTERP") != nullptr;
    if (trace && replays_ % 120 == 1) {
        std::fprintf(stderr, "interp: %zu draws (%u 3D), %zu in the previous frame, %u 3D matched, %u too different%s%s\n",
                     cur.draws.size(), scene, prev.draws.size(), matched, rejected, cur.overflow ? ", overflow" : "",
                     replay_blend_ ? ", blending" : "");
    }
}

// With the GPU still on the previous frame (other programs share it, or the
// frames cost more than a frame's time), the frame goes out without extra
// frames, the previous image staying up through their part of its slot.
bool Renderer::want_replays() {
    uint64_t done = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(ctx_.device, timeline_, &done));
    if (done >= previous_frame_ready_) return true;
    ++skipped_replays_;
    return false;
}

// Uncapped: another extra frame only while the GPU has at most the last one still to do.
bool Renderer::want_another_replay() {
    if (!uncapped_) return true;
    uint64_t done = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(ctx_.device, timeline_, &done));
    if (done + 1 >= submitted_) return true;
    ++skipped_replays_;
    return false;
}

void Renderer::end_replay() {
    end_rendering();
    replaying_ = false;
    show_extra_frame();
    replay_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - replay_started_).count();
}

void Renderer::frame_shown() {
    finish_frame_presentation();
    // The next frame sees the real frame's result in carried targets.
    if (!carried_.empty()) {
        size_t swapped = 0;
        for (Target* t : carried_) {
            const bool rewritten = t->written_frame + 1 == frame_number_;
            // (Without replays the target still holds the frame's result.)
            if (rewritten && t->start_copied && replayed_) {
                copy_image(t->end_copy, t->image);
                t->written = stamp();
                ++swapped;
            }
            t->rewrites = rewritten;
            t->carried = t->start_copied = false;
        }
        static const bool trace = std::getenv("TTT2_TRACE_INTERP") != nullptr;
        static uint64_t frames = 0;
        if (trace && frames++ % 120 == 0) {
            std::fprintf(stderr, "interp: %zu targets carried between frames, %zu of them rewritten\n", carried_.size(), swapped);
        }
        carried_.clear();
    }
    replayed_ = false;
    // This frame's draws and replays read its record until they finish.
    record_value_[current_record_] = recording_ ? submitted_ + 1 : submitted_;
    current_record_ ^= 1;
    if (recording_ && record_value_[current_record_] > submitted_) submit(false);
    wait_value(record_value_[current_record_]);
    frame_records_[current_record_].clear();
}

} // namespace cafe::gpu::vk
