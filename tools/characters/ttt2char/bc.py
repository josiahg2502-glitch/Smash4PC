"""Block-compressed textures (Microsoft's "Texture Block Compression in Direct3D 11" formats) in numpy.

Decoding returns an (H, W, 4) uint8 RGBA array; blocks are 4x4 texels stored row-major over the image (the image is
padded up to a multiple of 4 and cropped afterwards).

    BC1 (8 B):  u16 c0, u16 c1 (RGB565, little-endian), u32 2-bit indices (texel i at bits 2i).
                c0 > c1: palette c0, c1, (2 c0 + c1) / 3, (c0 + 2 c1) / 3; otherwise c0, c1, (c0 + c1) / 2, transparent.
    BC2 (16 B): 64 bits of explicit 4-bit alpha, then a BC1 colour block (always the four-colour palette).
    BC3 (16 B): a BC4 alpha block, then a BC1 colour block (always the four-colour palette).
    BC4 (8 B):  u8 a0, u8 a1, 48 bits of 3-bit indices. a0 > a1: eight-step ramp; otherwise six steps, 0 and 255.
    BC5 (16 B): two BC4 blocks (red, green); blue is rebuilt as the Z of a unit normal.

Encoding writes BC1 (opaque, four-colour mode) or BC3: the endpoints come from the principal axis of each block's
colours, then two least-squares passes refine them.
"""
import numpy as np


# ------------------------------------------------------------------------------------------------ decoding
def _blocks(data, w, h, bsize):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    a = np.frombuffer(data, dtype=np.uint8, count=bw * bh * bsize)
    return a.reshape(bw * bh, bsize), bw, bh


def _assemble(texels, bw, bh, w, h):
    """(n, 16, 4) texels of n blocks -> (h, w, 4) image."""
    img = texels.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return np.ascontiguousarray(img[:h, :w])


def _565(c):
    r = ((c >> 11) & 31).astype(np.uint32)
    g = ((c >> 5) & 63).astype(np.uint32)
    b = (c & 31).astype(np.uint32)
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1)


def _bc1_texels(blk, force4):
    c0 = blk[:, 0].astype(np.uint32) | (blk[:, 1].astype(np.uint32) << 8)
    c1 = blk[:, 2].astype(np.uint32) | (blk[:, 3].astype(np.uint32) << 8)
    idx = blk[:, 4:8].copy().view('<u4').reshape(-1).astype(np.uint32)
    p0, p1 = _565(c0), _565(c1)
    four = np.ones_like(c0, dtype=bool) if force4 else (c0 > c1)
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], axis=1)
    alpha = np.full((len(c0), 4), 255, np.uint32)
    alpha[:, 3] = np.where(four, 255, 0)
    sel = (idx[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    rows = np.arange(len(c0))[:, None]
    return np.concatenate([pal[rows, sel], alpha[rows, sel][..., None]], axis=-1).astype(np.uint8)


def _bc4_values(blk):
    a0 = blk[:, 0].astype(np.int32)
    a1 = blk[:, 1].astype(np.int32)
    bits = np.zeros(len(blk), np.uint64)
    for i in range(6):
        bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = ((bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
    eight = a0 > a1
    pal = np.zeros((len(blk), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    for i in range(1, 7):
        pal[:, 1 + i] = np.where(eight, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, 1 + i] = np.where(eight, pal[:, 1 + i], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(eight, pal[:, 6], 0)
    pal[:, 7] = np.where(eight, pal[:, 7], 255)
    return pal[np.arange(len(blk))[:, None], sel].astype(np.uint8)


def decode_bc1(data, w, h):
    blk, bw, bh = _blocks(data, w, h, 8)
    return _assemble(_bc1_texels(blk, False), bw, bh, w, h)


def decode_bc2(data, w, h):
    blk, bw, bh = _blocks(data, w, h, 16)
    t = _bc1_texels(blk[:, 8:], True)
    a = blk[:, :8]
    t[..., 3] = np.stack([a & 15, a >> 4], axis=-1).reshape(-1, 16) * 17
    return _assemble(t, bw, bh, w, h)


def decode_bc3(data, w, h):
    blk, bw, bh = _blocks(data, w, h, 16)
    t = _bc1_texels(blk[:, 8:], True)
    t[..., 3] = _bc4_values(blk[:, :8])
    return _assemble(t, bw, bh, w, h)


def decode_bc4(data, w, h):
    blk, bw, bh = _blocks(data, w, h, 8)
    v = _bc4_values(blk)
    return _assemble(np.stack([v, v, v, np.full_like(v, 255)], axis=-1), bw, bh, w, h)


def decode_bc5(data, w, h):
    blk, bw, bh = _blocks(data, w, h, 16)
    r = _bc4_values(blk[:, :8])
    g = _bc4_values(blk[:, 8:])
    x = r.astype(np.float32) / 127.5 - 1
    y = g.astype(np.float32) / 127.5 - 1
    z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
    b = np.clip(np.round((z + 1) * 127.5), 0, 255).astype(np.uint8)
    return _assemble(np.stack([r, g, b, np.full_like(r, 255)], axis=-1), bw, bh, w, h)


def decode_bgra8(data, w, h):
    a = np.frombuffer(data, np.uint8, count=w * h * 4).reshape(h, w, 4)
    return np.ascontiguousarray(a[..., [2, 1, 0, 3]])


def decode_g8(data, w, h):
    a = np.frombuffer(data, np.uint8, count=w * h).reshape(h, w)
    return np.stack([a, a, a, np.full_like(a, 255)], axis=-1)


def decode_bc7(data, w, h):
    """BC7 through Pillow's block decoder (its 'bcn' decoder, format 7), on whole blocks."""
    from PIL import Image
    bw, bh = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    im = Image.frombytes('RGBA', (bw, bh), bytes(data[:(bw // 4) * (bh // 4) * 16]), 'bcn', 7)
    return np.asarray(im)[:h, :w].copy()


# Unreal Engine pixel format names -> decoder
DECODERS = {'PF_DXT1': decode_bc1, 'PF_DXT3': decode_bc2, 'PF_DXT5': decode_bc3, 'PF_BC4': decode_bc4,
            'PF_BC5': decode_bc5, 'PF_BC7': decode_bc7, 'PF_B8G8R8A8': decode_bgra8, 'PF_G8': decode_g8}


def decode(pixel_format, data, w, h):
    fn = DECODERS.get(pixel_format)
    if fn is None:
        raise NotImplementedError('texture pixel format %s is not supported' % pixel_format)
    return fn(data, w, h)


# ------------------------------------------------------------------------------------------------ encoding
def _blockify(img):
    h, w = img.shape[:2]
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (ph, pw) != (h, w):
        img = np.pad(img, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    return img.reshape(ph // 4, 4, pw // 4, 4, -1).transpose(0, 2, 1, 3, 4).reshape(-1, 16, img.shape[2])


def _to565(c):
    c = np.clip(np.rint(c), 0, 255)
    r = np.rint(c[..., 0] * 31 / 255).astype(np.uint32)
    g = np.rint(c[..., 1] * 63 / 255).astype(np.uint32)
    b = np.rint(c[..., 2] * 31 / 255).astype(np.uint32)
    return (r << 11) | (g << 5) | b


def _from565(v):
    v = v.astype(np.uint32)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


_WEIGHTS = np.array([[1, 0], [0, 1], [2 / 3, 1 / 3], [1 / 3, 2 / 3]], np.float32)  # index -> (w c0, w c1)


def _fit_colour(px):
    """px: (n, 16, 3) float -> (c0 u16, c1 u16, indices (n, 16)) in four-colour mode (c0 >= c1)."""
    n = len(px)
    mean = px.mean(1, keepdims=True)
    d = px - mean
    cov = np.einsum('nki,nkj->nij', d, d)
    axis = np.ones((n, 3), np.float32) / np.sqrt(3)
    for _ in range(8):                                     # power iteration for the principal axis
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
    t = np.einsum('nki,ni->nk', d, axis)
    lo = mean[:, 0] + axis * t.min(1)[:, None]
    hi = mean[:, 0] + axis * t.max(1)[:, None]
    a, b = hi, lo
    for it in range(3):
        c0, c1 = _to565(a), _to565(b)
        e0, e1 = _from565(c0), _from565(c1)
        pal = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)
        idx = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1).argmin(-1)
        if it == 2:
            break
        # least squares: px_k ~ w0_k * a + w1_k * b
        w = _WEIGHTS[idx]
        ata = np.einsum('nki,nkj->nij', w, w)
        atb = np.einsum('nki,nkc->nic', w, px)
        det = ata[:, 0, 0] * ata[:, 1, 1] - ata[:, 0, 1] * ata[:, 1, 0]
        ok = np.abs(det) > 1e-6
        inv = np.zeros_like(ata)
        inv[:, 0, 0], inv[:, 1, 1] = ata[:, 1, 1], ata[:, 0, 0]
        inv[:, 0, 1], inv[:, 1, 0] = -ata[:, 0, 1], -ata[:, 1, 0]
        sol = np.einsum('nij,njc->nic', inv, atb) / np.where(ok, det, 1)[:, None, None]
        a = np.where(ok[:, None], np.clip(sol[:, 0], 0, 255), a)
        b = np.where(ok[:, None], np.clip(sol[:, 1], 0, 255), b)
    # the four-colour palette needs c0 > c1: swap the endpoints (and indices 0 <-> 1, 2 <-> 3) where c0 < c1
    swap = c0 < c1
    c0s, c1s = np.where(swap, c1, c0), np.where(swap, c0, c1)
    idx = np.where(swap[:, None], idx ^ 1, idx)
    idx = np.where((c0s == c1s)[:, None], 0, idx)
    return c0s.astype(np.uint16), c1s.astype(np.uint16), idx.astype(np.uint32)


def _pack_colour(c0, c1, idx):
    bits = np.zeros(len(c0), np.uint32)
    for i in range(16):
        bits |= idx[:, i] << (2 * i)
    out = np.zeros((len(c0), 8), np.uint8)
    out[:, 0:2] = c0.astype('<u2').view(np.uint8).reshape(-1, 2)
    out[:, 2:4] = c1.astype('<u2').view(np.uint8).reshape(-1, 2)
    out[:, 4:8] = bits.astype('<u4').view(np.uint8).reshape(-1, 4)
    return out


def _alpha_block(al):
    """al: (n, 16) float -> (n, 8) BC4 blocks with the eight-step ramp (a0 > a1) where the block varies."""
    a0 = np.clip(np.rint(al.max(1)), 0, 255).astype(np.int32)
    a1 = np.clip(np.rint(al.min(1)), 0, 255).astype(np.int32)
    pal = np.stack([a0, a1] + [((7 - k) * a0 + k * a1) / 7 for k in range(1, 7)], 1).astype(np.float32)
    idx = np.abs(al[:, :, None] - pal[:, None, :]).argmin(-1).astype(np.uint64)
    idx = np.where((a0 == a1)[:, None], 0, idx)
    bits = np.zeros(len(al), np.uint64)
    for i in range(16):
        bits |= idx[:, i] << np.uint64(3 * i)
    out = np.zeros((len(al), 8), np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    for k in range(6):
        out[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(0xFF)).astype(np.uint8)
    return out


def encode(img, fmt=0):
    """img: (H, W, 3 or 4) uint8 -> bytes. fmt 0 = BC1 (DXT1), 2 = BC3 (DXT5): the NUT format numbers."""
    img = np.asarray(img)
    if img.shape[2] == 3:
        img = np.concatenate([img, np.full(img.shape[:2] + (1,), 255, np.uint8)], 2)
    blk = _blockify(img).astype(np.float32)
    colour = _pack_colour(*_fit_colour(blk[:, :, :3]))
    if fmt == 0:
        return colour.tobytes()
    if fmt == 2:
        return np.concatenate([_alpha_block(blk[:, :, 3]), colour], 1).tobytes()
    raise ValueError('cannot encode texture format %d' % fmt)
