"""Animations: MOTA containers, TTT2's motion packs, and the byte-order conversion of one animation (Tekken 7 PC
stores them little-endian, the Wii U big-endian; the animations themselves are the same format).

Motion pack (data005: one per motion row): little-endian u32 count (6), count x {u32 offset, u32 size}, items
16-byte aligned from 0x40: body MOTA, hands, face, (empty), camera, extra.
MOTA: "MOTA", u8, u8 (byte order: 0 1 big-endian, 1 0 little-endian), u16, u32 0x14, u32 count @0xC, u32, then
count x u32 offsets (from the MOTA's start) at 0x14.
Animation: u16 type, u16 channel count, then
    type 0xC8 (raw): u32 frames, nch x u32 descriptors, frames x nch x 3 f32
    type 0x64 (compressed): nch x u16 descriptors, u16 frames, u8, u8, u16 n, n x u32, per channel 3 x i16
        (descriptors 4..7) or 3 x f32 (others), ((frames + 14) >> 4) x u32 keyframe offsets, then a byte stream.
"""
import array
import struct


def pack_items(d):
    """[(offset, size)] of a motion pack's items."""
    n, = struct.unpack_from('<I', d, 0)
    return [struct.unpack_from('<II', d, 4 + 8 * k) for k in range(n)]


def build_pack(items):
    out = bytearray(0x40)
    struct.pack_into('<I', out, 0, len(items))
    for k, it in enumerate(items):
        while len(out) % 16:
            out.append(0)
        struct.pack_into('<II', out, 4 + 8 * k, len(out), len(it))
        out += it
    return bytes(out)


def mota_offsets(b, o=0):
    """(byte order, [animation offsets]) of the MOTA at b[o:]."""
    if b[o:o + 4] != b'MOTA':
        raise ValueError('not a MOTA')
    e = '<' if b[o + 4] == 1 else '>'
    cnt, = struct.unpack_from(e + 'I', b, o + 0xC)
    return e, list(struct.unpack_from(e + '%dI' % cnt, b, o + 0x14)) if cnt else []


def extend_mota(m, anims):
    """A big-endian MOTA with big-endian animations appended (each padded to 4)."""
    if m[:4] != b'MOTA' or m[4] != 0:
        raise ValueError('not a big-endian MOTA')
    cnt, = struct.unpack_from('>I', m, 0xC)
    offs = list(struct.unpack_from('>%dI' % cnt, m, 0x14))
    data_start = 0x14 + 4 * cnt
    if cnt and offs[0] != data_start:
        raise ValueError('MOTA animations do not follow its offsets')
    body = bytearray(m[data_start:])
    while len(body) % 4:
        body.append(0)
    n2 = cnt + len(anims)
    new_offs = [o + 4 * len(anims) for o in offs]
    at = 0x14 + 4 * n2 + len(body)
    for a in anims:
        new_offs.append(at)
        a = bytes(a) + bytes(-len(a) % 4)
        body += a
        at += len(a)
    hdr = bytearray(m[:0x14])
    struct.pack_into('>I', hdr, 0xC, n2)
    return bytes(hdr) + struct.pack('>%dI' % n2, *new_offs) + bytes(body)


def replace_mota(m, anims):
    """A big-endian MOTA with m's header and only `anims` (big-endian)."""
    if m[:4] != b'MOTA' or m[4] != 0:
        raise ValueError('not a big-endian MOTA')
    return extend_mota(m[:0xC] + bytes(4) + m[0x10:0x14], anims)


def _swap(b, o, w):
    b[o:o + w] = b[o:o + w][::-1]


def swap_animation(src, src_endian):
    """A copy of the animation at the start of src in the other byte order (bytes after its swapped part are
    copied as they are)."""
    e = src_endian
    b = bytearray(src)
    typ, nch = struct.unpack_from(e + 'HH', src, 0)
    if typ == 0x64:
        desc = struct.unpack_from(e + '%dH' % nch, src, 4)
        o = 4 + 2 * nch
        frames, = struct.unpack_from(e + 'H', src, o)
        niter, = struct.unpack_from(e + 'H', src, o + 4)
        for p in range(0, o, 2):
            _swap(b, p, 2)
        _swap(b, o, 2)
        _swap(b, o + 4, 2)
        o += 6
        for _ in range(niter):
            _swap(b, o, 4)
            o += 4
        for d in desc:
            w = 2 if 4 <= d <= 7 else 4
            for _ in range(3):
                _swap(b, o, w)
                o += w
        for _ in range((frames + 14) >> 4):
            _swap(b, o, 4)
            o += 4
        return bytes(b)
    if typ == 0xC8:
        frames, = struct.unpack_from(e + 'I', src, 4)
        _swap(b, 0, 2)
        _swap(b, 2, 2)
        _swap(b, 4, 4)
        words = array.array('I', bytes(b[8:8 + 4 * (nch + frames * nch * 3)]))
        words.byteswap()
        b[8:8 + 4 * len(words)] = words.tobytes()
        return bytes(b)
    raise ValueError('animation type %#x' % typ)
