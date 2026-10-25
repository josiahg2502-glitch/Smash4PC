// zlib125: the system's zlib 1.2.5, served by the host's zlib. Each guest
// z_stream (32-bit, big-endian) gets a host z_stream; the public fields are
// copied in before a call and back after it. The guest's zalloc/zfree are
// not used: the host stream allocates its own state.

#include "cafe/export.h"
#include "cafe/guest.h"

#include <zlib.h>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace cafe::os {
namespace {

struct GuestStream {
    be<uint32_t> next_in, avail_in, total_in;
    be<uint32_t> next_out, avail_out, total_out;
    be<uint32_t> msg, state, zalloc, zfree, opaque;
    be<int32_t> data_type;
    be<uint32_t> adler, reserved;
};
static_assert(sizeof(GuestStream) == 0x38);

std::mutex g_mutex;
std::unordered_map<uint32_t, std::unique_ptr<z_stream>> g_streams;

z_stream* host_stream(uint32_t address, bool create) {
    std::lock_guard lock(g_mutex);
    auto& stream = g_streams[address];
    if (!stream && create) stream = std::make_unique<z_stream>();
    return stream.get();
}

void forget(uint32_t address) {
    std::lock_guard lock(g_mutex);
    g_streams.erase(address);
}

void copy_in(const GuestStream& g, z_stream& h) {
    h.next_in = guest<Bytef>(g.next_in);
    h.avail_in = g.avail_in;
    h.total_in = g.total_in;
    h.next_out = guest<Bytef>(g.next_out);
    h.avail_out = g.avail_out;
    h.total_out = g.total_out;
}

void copy_out(const z_stream& h, GuestStream& g) {
    g.next_in = guest_address(h.next_in);
    g.avail_in = h.avail_in;
    g.total_in = static_cast<uint32_t>(h.total_in);
    g.next_out = guest_address(h.next_out);
    g.avail_out = h.avail_out;
    g.total_out = static_cast<uint32_t>(h.total_out);
    g.msg = 0;
    g.data_type = h.data_type;
    g.adler = static_cast<uint32_t>(h.adler);
}

// A fresh host stream for an init call, taking the guest's input fields.
z_stream* start(GuestStream* g, uint32_t address) {
    forget(address);
    z_stream* h = host_stream(address, true);
    copy_in(*g, *h);
    return h;
}

int32_t z_inflateInit2(GuestStream* g, int32_t window_bits, GuestAddress, int32_t) {
    if (!g) return Z_STREAM_ERROR;
    z_stream* h = start(g, guest_address(g));
    const int result = inflateInit2(h, window_bits);
    copy_out(*h, *g);
    return result;
}
int32_t z_inflateInit(GuestStream* g, GuestAddress version, int32_t size) {
    return z_inflateInit2(g, MAX_WBITS, version, size);
}

int32_t z_deflateInit2(GuestStream* g, int32_t level, int32_t method, int32_t window_bits, int32_t mem_level,
                       int32_t strategy, GuestAddress, int32_t) {
    if (!g) return Z_STREAM_ERROR;
    z_stream* h = start(g, guest_address(g));
    const int result = deflateInit2(h, level, method, window_bits, mem_level, strategy);
    copy_out(*h, *g);
    return result;
}

// inflate, deflate and the rest: run on the stream's host state.
template <int (*Call)(z_stream*, int)>
int32_t with_flush(GuestStream* g, int32_t flush) {
    z_stream* h = g ? host_stream(guest_address(g), false) : nullptr;
    if (!h) return Z_STREAM_ERROR;
    copy_in(*g, *h);
    const int result = Call(h, flush);
    copy_out(*h, *g);
    return result;
}
template <int (*Call)(z_stream*)>
int32_t plain(GuestStream* g) {
    z_stream* h = g ? host_stream(guest_address(g), false) : nullptr;
    if (!h) return Z_STREAM_ERROR;
    copy_in(*g, *h);
    const int result = Call(h);
    copy_out(*h, *g);
    return result;
}
template <int (*Call)(z_stream*)>
int32_t ending(GuestStream* g) {
    const int32_t result = plain<Call>(g);
    if (g) forget(guest_address(g));
    return result;
}

uint32_t z_deflateBound(GuestStream* g, uint32_t source_length) {
    z_stream* h = g ? host_stream(guest_address(g), false) : nullptr;
    return static_cast<uint32_t>(deflateBound(h, source_length));
}

uint32_t z_crc32(uint32_t crc, const uint8_t* data, uint32_t length) {
    return static_cast<uint32_t>(crc32(crc, data, length));
}
uint32_t z_adler32(uint32_t adler, const uint8_t* data, uint32_t length) {
    return static_cast<uint32_t>(adler32(adler, data, length));
}

} // namespace

CAFE_EXPORT(zlib125, inflateInit_, z_inflateInit);
CAFE_EXPORT(zlib125, inflateInit2_, z_inflateInit2);
CAFE_EXPORT(zlib125, inflate, with_flush<inflate>);
CAFE_EXPORT(zlib125, inflateReset, plain<inflateReset>);
CAFE_EXPORT(zlib125, inflateEnd, ending<inflateEnd>);
CAFE_EXPORT(zlib125, deflateInit2_, z_deflateInit2);
CAFE_EXPORT(zlib125, deflate, with_flush<deflate>);
CAFE_EXPORT(zlib125, deflateEnd, ending<deflateEnd>);
CAFE_EXPORT(zlib125, deflateBound, z_deflateBound);
CAFE_EXPORT(zlib125, crc32, z_crc32);
CAFE_EXPORT(zlib125, adler32, z_adler32);

} // namespace cafe::os
