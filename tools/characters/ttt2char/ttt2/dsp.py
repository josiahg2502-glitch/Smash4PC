"""Nintendo DSP ADPCM (the Wii U's sound hardware format) and Namco's IDSP container for it, as TTT2's voice banks
hold them.

DSP ADPCM: frames of 8 bytes for 14 samples, a header byte (predictor << 4 | scale) and 14 signed 4-bit codes, high
half first. A sample = clamp16(((code << scale) << 11) + c1 * h1 + c2 * h2 + 1024 >> 11) with the predictor's pair of
11-bit fixed point coefficients (c1, c2) and the two previous samples (h1, h2); a channel has 8 coefficient pairs.

IDSP (big-endian): "IDSP", u32 0, u32 channels, u32 sample rate, u32 samples, u32 loop start, u32 loop end,
u32 interleave (16 bytes), u32 header size (0x40), u32 channel header size (0x60), u32 data offset, u32 data size per
channel, zeros to 0x40; then per channel the standard 0x60-byte DSP header {u32 samples, u32 nibbles, u32 rate,
u16 loop flag, u16 format 0, u32 loop start address, u32 end address, u32 current address, s16 coefficients[16],
u16 gain, u16 predictor/scale, u16 history x2, u16 loop predictor/scale, u16 loop history x2, padding}; then the
channels' data, interleaved 16 bytes at a time.
"""
import struct

import numpy as np

FRAME_SAMPLES = 14
INTERLEAVE = 16


def _clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def coefficients(x, n=8):
    """8 predictor pairs for a signal (int16 array): k-means over the frames' least-squares second-order predictors,
    weighted by the frames' energy; 11-bit fixed point."""
    x = x.astype(np.float64)
    if len(x) < 3:
        return [(0, 0)] * n
    frames = len(x) // FRAME_SAMPLES
    preds, weights = [], []
    for f in range(frames):
        s = f * FRAME_SAMPLES
        y = x[s:s + FRAME_SAMPLES]
        h1 = x[max(s - 1, 0):s + FRAME_SAMPLES - 1] if s else np.concatenate(([0.0], x[:FRAME_SAMPLES - 1]))
        h2 = x[max(s - 2, 0):s + FRAME_SAMPLES - 2] if s >= 2 else np.concatenate((np.zeros(2 - s), x[:FRAME_SAMPLES - 2 + s]))
        a = np.stack([h1, h2], 1)
        e = float((y ** 2).sum())
        if e < 1e3:
            continue
        sol, *_ = np.linalg.lstsq(a, y, rcond=None)
        sol = np.clip(sol, -1.99, 1.99)
        preds.append(sol)
        weights.append(e)
    base = [np.array(p) for p in ((0.0, 0.0), (1.0, 0.0), (1.9, -0.95), (1.5, -0.6), (1.75, -0.8), (0.9, 0.0),
                                  (1.95, -0.98), (1.2, -0.3))]
    if len(preds) < n:
        cent = base
    else:
        p = np.array(preds)
        w = np.array(weights)
        w = w / w.sum()
        cent = np.array(base[:n])
        for _ in range(20):
            d = ((p[:, None, :] - cent[None, :, :]) ** 2).sum(2)
            lab = d.argmin(1)
            for k in range(n):
                m = lab == k
                if m.any():
                    cent[k] = (p[m] * w[m, None]).sum(0) / w[m].sum()
        cent = list(cent)
    return [(int(np.clip(round(c[0] * 2048), -32768, 32767)), int(np.clip(round(c[1] * 2048), -32768, 32767)))
            for c in cent]


def encode(x, coefs):
    """int16 samples -> DSP ADPCM bytes (whole frames)."""
    x = np.asarray(x, np.int64)
    frames = (len(x) + FRAME_SAMPLES - 1) // FRAME_SAMPLES
    pad = np.zeros(frames * FRAME_SAMPLES, np.int64)
    pad[:len(x)] = x
    # choose each frame's predictor from its residual with the original samples as history
    h1 = np.concatenate(([0], pad[:-1]))
    h2 = np.concatenate(([0, 0], pad[:-2]))
    c = np.array(coefs, np.int64)
    res = pad[None, :] * 2048 - (c[:, 0:1] * h1[None, :] + c[:, 1:2] * h2[None, :])
    peak = np.abs(res).reshape(len(coefs), frames, FRAME_SAMPLES).max(2)     # [predictor, frame]
    choice = peak.argmin(0)
    out = bytearray()
    y1 = y2 = 0
    for f in range(frames):
        seg = pad[f * FRAME_SAMPLES:(f + 1) * FRAME_SAMPLES].tolist()
        k = int(choice[f])
        c1, c2 = coefs[k]
        best = None
        scale0 = 0
        p = int(peak[k, f]) >> 11
        while scale0 < 12 and (p >> scale0) > 7:
            scale0 += 1
        for scale in (scale0, min(scale0 + 1, 12)):
            a1, a2, err, codes = y1, y2, 0, []
            for s in seg:
                pred = c1 * a1 + c2 * a2
                code = ((s << 11) - pred) / (2048 << scale)
                code = int(code + 0.5) if code >= 0 else -int(-code + 0.5)
                code = -8 if code < -8 else (7 if code > 7 else code)
                v = _clamp16(((code << scale) << 11) + pred + 1024 >> 11)
                err += (v - s) * (v - s)
                codes.append(code & 15)
                a2, a1 = a1, v
            if best is None or err < best[0]:
                best = (err, scale, codes, a1, a2)
        _, scale, codes, y1, y2 = best
        out.append(k << 4 | scale)
        for i in range(0, FRAME_SAMPLES, 2):
            out.append(codes[i] << 4 | codes[i + 1])
    return bytes(out)


def decode(data, coefs, samples):
    out = []
    y1 = y2 = 0
    for f in range(0, len(data), 8):
        head = data[f]
        c1, c2 = coefs[head >> 4]
        scale = head & 15
        for b in data[f + 1:f + 8]:
            for code in (b >> 4, b & 15):
                code = code - 16 if code >= 8 else code
                v = _clamp16(((code << scale) << 11) + c1 * y1 + c2 * y2 + 1024 >> 11)
                out.append(v)
                y2, y1 = y1, v
    return np.array(out[:samples], np.int16)


def nibbles(samples):
    return samples // FRAME_SAMPLES * 16 + (samples % FRAME_SAMPLES + 2 if samples % FRAME_SAMPLES else 0)


def idsp(channels, rate):
    """An IDSP file from int16 channel arrays of equal length."""
    n = len(channels[0])
    encoded, headers = [], []
    for x in channels:
        coefs = coefficients(x)
        data = encode(x, coefs)
        encoded.append(data)
        h = struct.pack('>IIIHHIII', n, nibbles(n), rate, 0, 0, 2, nibbles(n) - 1, 2)
        h += struct.pack('>16h', *[v for pair in coefs for v in pair])
        h += struct.pack('>7H', 0, data[0] if data else 0, 0, 0, 0, 0, 0)
        headers.append(h.ljust(0x60, b'\0'))
    size = len(encoded[0])
    head = struct.pack('>4sIIIIIIIIIII', b'IDSP', 0, len(channels), rate, n, 0, 0, INTERLEAVE, 0x40, 0x60,
                       0x40 + 0x60 * len(channels), size).ljust(0x40, b'\0')
    body = bytearray()
    for o in range(0, size, INTERLEAVE):
        for data in encoded:
            body += data[o:o + INTERLEAVE].ljust(INTERLEAVE, b'\0')
    return head + b''.join(headers) + bytes(body)


def read_idsp(d):
    """-> (rate, [int16 arrays]) of an IDSP file."""
    if d[:4] != b'IDSP':
        raise ValueError('not an IDSP file')
    _, _, ch, rate, n, _, _, inter, hsize, chsize, doff, dsize = struct.unpack_from('>4sIIIIIIIIIII', d, 0)
    out = []
    for c in range(ch):
        h = d[hsize + chsize * c:hsize + chsize * (c + 1)]
        coefs = list(zip(*[iter(struct.unpack_from('>16h', h, 0x1C))] * 2))
        data = bytearray()
        for o in range(0, dsize, inter):
            p = doff + o * ch + c * inter
            data += d[p:p + inter]
        out.append(decode(bytes(data[:dsize]), coefs, n))
    return rate, out
