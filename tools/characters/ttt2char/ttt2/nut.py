"""TTT2 Wii U textures: NUT ("NTP3" version 0x0100, big-endian header), read and written byte for byte, and new
textures made from an RGBA image.

Per texture: u32 total size, u32 0 (palette), u32 data size, u16 header size, u16 0, u8 0, u8 mip count, u8 0,
u8 format (0 BC1, 1 BC2, 2 BC3, 8/14/17 plain), u16 width, u16 height, 24 zero bytes, [the mips' sizes as u32,
padded to 16, only with more than one mip], "eXt\\0" u32 0x20 u32 0x10 u32 0, "GIDX" u32 0x10 u32 id u32 0, then
the mips back to back (linear, not tiled, unpadded). The id (GIDX) is what a model's material names.
"""
import struct

import numpy as np

from .. import bc

FMT_BLOCK = {0: 8, 1: 16, 2: 16}       # bytes per 4x4 block
FMT_BPP = {8: 2, 14: 4, 17: 4}         # bytes per pixel of the plain formats


def mip_size(fmt, w, h):
    if fmt in FMT_BLOCK:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * FMT_BLOCK[fmt]
    return w * h * FMT_BPP[fmt]


def read(d):
    """-> {'version', 'textures': [{'fmt', 'w', 'h', 'mips': [bytes], 'gidx', ...}], 'trailer'}"""
    if d[:4] != b'NTP3':
        raise ValueError('not a NUT texture container')
    ver, count = struct.unpack_from('>HH', d, 4)
    p = 0x10
    out = []
    for _ in range(count):
        total, pal, dsize, hsize, z0 = struct.unpack_from('>IIIHH', d, p)
        b10, mips, b12, fmt, w, h = struct.unpack_from('>4B2H', d, p + 0x10)
        q = p + 0x30
        if mips > 1:
            msz = list(struct.unpack_from('>%dI' % mips, d, q))
            q += (4 * mips + 15) & ~15
        else:
            msz = [dsize]
        ext = d[q:p + hsize]                    # eXt and GIDX blocks, kept as they are
        g = ext.find(b'GIDX')
        gidx = struct.unpack_from('>I', ext, g + 8)[0] if g >= 0 else None
        data = d[p + hsize:p + hsize + dsize]
        mipdata, o = [], 0
        for s in msz:
            mipdata.append(data[o:o + s])
            o += s
        out.append(dict(fmt=fmt, w=w, h=h, mips=mipdata, gidx=gidx, pal=pal, z0=z0, b10=b10, b12=b12,
                        r18=d[p + 0x18:p + 0x30], ext=ext, tail=data[o:], total_extra=total - hsize - dsize))
        p += total
    return dict(version=ver, textures=out, trailer=d[p:])


def _ext(gidx):
    return b'eXt\0' + struct.pack('>III', 0x20, 0x10, 0) + b'GIDX' + struct.pack('>III', 0x10, gidx, 0)


def write(nut):
    """nut: what read() returns, or a plain list of textures."""
    if isinstance(nut, list):
        nut = dict(version=0x0100, textures=nut, trailer=b'')
    texs = nut['textures']
    out = bytearray(b'NTP3' + struct.pack('>HH', nut.get('version', 0x0100), len(texs)) + bytes(8))
    for t in texs:
        mips = t['mips']
        n = len(mips)
        mt = b''
        if n > 1:
            mt = struct.pack('>%dI' % n, *[len(m) for m in mips])
            mt += bytes((-len(mt)) % 16)
        ext = t.get('ext')
        if ext is None:
            ext = _ext(t['gidx'])
        hsize = 0x30 + len(mt) + len(ext)
        data = b''.join(mips) + t.get('tail', b'')
        total = hsize + len(data) + t.get('total_extra', 0)
        out += struct.pack('>IIIHH4B2H', total, t.get('pal', 0), len(data) - len(t.get('tail', b'')), hsize,
                           t.get('z0', 0), t.get('b10', 0), n, t.get('b12', 0), t['fmt'], t['w'], t['h'])
        out += t.get('r18', bytes(24)) + mt + ext + data + bytes(t.get('total_extra', 0))
    out += nut.get('trailer', b'')
    return bytes(out)


def decode(t):
    """A texture's first mip as an (H, W, 4) uint8 RGBA array (block formats only)."""
    fn = {0: bc.decode_bc1, 1: bc.decode_bc2, 2: bc.decode_bc3}.get(t['fmt'])
    if fn is None:
        raise NotImplementedError('NUT format %d' % t['fmt'])
    return fn(t['mips'][0], t['w'], t['h'])


def make_texture(rgba, gidx, fmt=0, mips=None, min_size=4):
    """A new texture from an (H, W, 4) uint8 image with power-of-two sides: a box-filtered mip chain (down to
    min_size, or `mips` levels), each level BC1 (fmt 0) or BC3 (fmt 2)."""
    img = np.asarray(rgba, np.uint8)
    levels = []
    cur = img.astype(np.float32)
    while True:
        h, w = cur.shape[:2]
        levels.append(bc.encode(np.clip(cur + 0.5, 0, 255).astype(np.uint8), fmt))
        if (mips and len(levels) >= mips) or (not mips and (w <= min_size or h <= min_size)):
            break
        cur = cur.reshape(h // 2, 2, w // 2, 2, 4).mean((1, 3))
    return dict(fmt=fmt, w=img.shape[1], h=img.shape[0], mips=levels, gidx=gidx)
