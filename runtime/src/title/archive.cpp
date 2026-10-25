#include "title/archive.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace cafe::title::archive {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// A chunk's payload (after its size and tag) in `data`.
struct Chunk {
    size_t at = 0, size = 0;
};
std::optional<Chunk> find_chunk(const std::vector<uint8_t>& data, const char (&tag)[5]) {
    size_t pos = 0;
    while (pos + 8 <= data.size()) {
        const uint32_t size = be32(&data[pos]);
        if (size < 8 || pos + size > data.size()) return std::nullopt;
        if (std::memcmp(&data[pos + 4], tag, 4) == 0) return Chunk{pos + 8, size - 8};
        pos += size;
    }
    return std::nullopt;
}

uint64_t align16(uint64_t n) { return (n + 15) & ~uint64_t{15}; }

// An index's name hashes, and its offsets: entries + 1 words in "ofsd" (at
// `offsets_at` in the file).
struct Layout {
    std::vector<uint32_t> hashes;
    size_t offsets_at = 0;
    std::vector<uint32_t> offsets;
};
std::optional<Layout> layout(const std::vector<uint8_t>& ofs) {
    const auto names = find_chunk(ofs, "ofsi");
    const auto offsets = find_chunk(ofs, "ofsd");
    if (!names || !offsets || names->size % 8 != 0 || offsets->size % 4 != 0) return std::nullopt;
    Layout l;
    for (size_t i = 0; i < names->size; i += 8) l.hashes.push_back(be32(&ofs[names->at + i]));
    l.offsets_at = offsets->at;
    for (size_t i = 0; i < offsets->size; i += 4) l.offsets.push_back(be32(&ofs[offsets->at + i]));
    if (l.offsets.size() != l.hashes.size() + 1) return std::nullopt;
    for (size_t i = 0; i + 1 < l.offsets.size(); ++i) {
        if ((l.offsets[i] & ~0xFu) > (l.offsets[i + 1] & ~0xFu)) return std::nullopt;
    }
    return l;
}

} // namespace

const Entry* Index::find(uint32_t hash) const {
    for (const Entry& e : entries) {
        if (e.hash == hash) return &e;
    }
    return nullptr;
}

std::optional<Index> parse_index(const std::vector<uint8_t>& ofs) {
    const auto l = layout(ofs);
    if (!l) return std::nullopt;
    Index index;
    for (size_t i = 0; i < l->hashes.size(); ++i) {
        const uint64_t start = l->offsets[i] & ~0xFu, end = l->offsets[i + 1] & ~0xFu;
        const uint32_t tail = l->offsets[i] & 0xF;
        Entry e;
        e.hash = l->hashes[i];
        e.offset = start;
        e.size = tail == 0 ? end - start : std::max<uint64_t>(end - start, 16) - 16 + tail;
        index.entries.push_back(e);
    }
    index.payload_size = l->offsets.back() & ~0xFu;
    return index;
}

// Four 32-bit linear congruential streams seeded from the hash, one per
// big-endian word of each 16-byte block, each stepping once per block.
void scramble(uint8_t* data, size_t size, uint32_t hash) {
    constexpr uint32_t kSeedMul = 0x000BDE95, kSeedAdd = 0x008EF345, kStepMul = 1531, kStepAdd = 5011;
    uint32_t state[4];
    uint32_t x = hash;
    for (uint32_t& s : state) {
        x = x * kSeedMul + kSeedAdd;
        s = x;
    }
    size_t pos = 0;
    while (pos < size) {
        uint8_t key[16];
        for (int i = 0; i < 4; ++i) {
            put_be32(&key[4 * i], state[i]);
            state[i] = state[i] * kStepMul + kStepAdd;
        }
        const size_t n = std::min<size_t>(16, size - pos);
        for (size_t i = 0; i < n; ++i) data[pos + i] ^= key[i];
        pos += n;
    }
}

std::optional<std::vector<uint8_t>> unpack(std::vector<uint8_t> stored, uint32_t hash) {
    scramble(stored.data(), stored.size(), hash);
    if (stored.size() < 16 || std::memcmp(stored.data(), "NRCZ", 4) != 0) return stored;
    std::vector<uint8_t> contents(be32(&stored[4]));
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return std::nullopt;
    z.next_in = stored.data() + 16;
    z.avail_in = static_cast<uInt>(stored.size() - 16);
    z.next_out = contents.data();
    z.avail_out = static_cast<uInt>(contents.size());
    const int status = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (status != Z_STREAM_END || z.avail_out != 0) return std::nullopt;
    return contents;
}

std::vector<uint8_t> peek(std::vector<uint8_t> stored, uint32_t hash, size_t size) {
    scramble(stored.data(), stored.size(), hash);
    if (stored.size() < 16 || std::memcmp(stored.data(), "NRCZ", 4) != 0) {
        stored.resize(std::min(stored.size(), size));
        return stored;
    }
    std::vector<uint8_t> contents(size);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return {};
    z.next_in = stored.data() + 16;
    z.avail_in = static_cast<uInt>(stored.size() - 16);
    z.next_out = contents.data();
    z.avail_out = static_cast<uInt>(contents.size());
    inflate(&z, Z_SYNC_FLUSH);
    contents.resize(contents.size() - z.avail_out);
    inflateEnd(&z);
    return contents;
}

void stored_flags(std::vector<uint8_t> stored_start, uint32_t hash, uint8_t (&flags)[8]) {
    std::memset(flags, 0, 8);
    stored_start.resize(std::min<size_t>(stored_start.size(), 16));
    scramble(stored_start.data(), stored_start.size(), hash);
    if (stored_start.size() == 16 && std::memcmp(stored_start.data(), "NRCZ", 4) == 0) {
        std::memcpy(flags, &stored_start[8], 8);
    }
}

std::vector<uint8_t> pack(const std::vector<uint8_t>& contents, uint32_t hash, const uint8_t (&flags)[8]) {
    uLongf bound = compressBound(static_cast<uLong>(contents.size()));
    std::vector<uint8_t> stored(16 + bound);
    std::memcpy(stored.data(), "NRCZ", 4);
    put_be32(&stored[4], static_cast<uint32_t>(contents.size()));
    std::memcpy(&stored[8], flags, 8);
    compress2(stored.data() + 16, &bound, contents.data(), static_cast<uLong>(contents.size()), Z_BEST_SPEED);
    stored.resize(16 + bound);
    scramble(stored.data(), stored.size(), hash);
    return stored;
}

std::optional<Rebuilt> rebuild(const std::vector<uint8_t>& ofs, const std::vector<uint8_t>& bin_header,
                               const std::map<uint32_t, std::vector<uint8_t>>& entries) {
    const auto l = layout(ofs);
    if (!l || bin_header.size() < kPayload || std::memcmp(&bin_header[kPayload - 4], "imge", 4) != 0) {
        return std::nullopt;
    }
    std::vector<uint8_t> names, offsets; // the new "ofsi" and "ofsd" payloads
    Rebuilt out;
    auto header = std::make_shared<std::vector<uint8_t>>(bin_header.begin(), bin_header.begin() + kPayload);
    out.bin.push_back(Piece{0, kPayload, 0, header});
    uint64_t at = 0; // in the new payload
    auto put = [&](uint32_t hash, uint32_t word, uint32_t tail) {
        const size_t n = names.size();
        names.resize(n + 8);
        put_be32(&names[n], hash);
        put_be32(&names[n + 4], word);
        offsets.resize(offsets.size() + 4);
        put_be32(&offsets[offsets.size() - 4], static_cast<uint32_t>(at) | tail);
    };
    auto put_new = [&](uint32_t hash, uint32_t word, const std::vector<uint8_t>& stored) {
        auto data = std::make_shared<std::vector<uint8_t>>(stored);
        put(hash, word, static_cast<uint32_t>(data->size() % 16));
        data->resize(align16(data->size()));
        const uint64_t padded = data->size();
        out.bin.push_back(Piece{kPayload + at, padded, 0, std::move(data)});
        at += padded;
    };
    const auto names_at = find_chunk(ofs, "ofsi");
    for (size_t i = 0; i < l->hashes.size(); ++i) {
        const uint32_t word = be32(&ofs[names_at->at + 8 * i + 4]);
        const auto it = entries.find(l->hashes[i]);
        if (it != entries.end()) {
            put_new(l->hashes[i], word, it->second);
            continue;
        }
        const uint64_t start = l->offsets[i] & ~0xFu, end = l->offsets[i + 1] & ~0xFu;
        put(l->hashes[i], word, l->offsets[i] & 0xF);
        if (end != start) {
            Piece& last = out.bin.back();
            if (!last.data && last.source + last.size == kPayload + start) {
                last.size += end - start;
            } else {
                out.bin.push_back(Piece{kPayload + at, end - start, kPayload + start, nullptr});
            }
        }
        at += end - start;
    }
    // Entries the archive does not hold go after its last one.
    for (const auto& [hash, stored] : entries) {
        if (std::find(l->hashes.begin(), l->hashes.end(), hash) == l->hashes.end()) put_new(hash, 0, stored);
    }
    offsets.resize(offsets.size() + 4);
    put_be32(&offsets[offsets.size() - 4], static_cast<uint32_t>(at));
    put_be32(&(*header)[kPayload - 8], static_cast<uint32_t>(at + 8));
    out.bin_size = kPayload + at;
    // The index: its chunks as they were, the two tables new.
    size_t pos = 0;
    while (pos + 8 <= ofs.size()) {
        const uint32_t size = be32(&ofs[pos]);
        if (size < 8 || pos + size > ofs.size()) break;
        const bool is_names = std::memcmp(&ofs[pos + 4], "ofsi", 4) == 0;
        if (is_names || std::memcmp(&ofs[pos + 4], "ofsd", 4) == 0) {
            const std::vector<uint8_t>& table = is_names ? names : offsets;
            const size_t n = out.ofs.size();
            out.ofs.resize(n + 8);
            put_be32(&out.ofs[n], static_cast<uint32_t>(8 + table.size()));
            std::memcpy(&out.ofs[n + 4], is_names ? "ofsi" : "ofsd", 4);
            out.ofs.insert(out.ofs.end(), table.begin(), table.end());
        } else {
            out.ofs.insert(out.ofs.end(), ofs.begin() + static_cast<ptrdiff_t>(pos),
                           ofs.begin() + static_cast<ptrdiff_t>(pos + size));
        }
        pos += size;
    }
    return out;
}

} // namespace cafe::title::archive
