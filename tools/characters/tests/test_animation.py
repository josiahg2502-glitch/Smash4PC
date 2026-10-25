"""Unit tests of ttt2char.ttt2.animation on made-up animations (no game files needed).

    python3 -m pytest -q tools/characters/tests/test_animation.py
"""
import os
import random
import struct
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.ttt2 import animation  # noqa: E402

UNIT = np.float32(np.pi / 32768)
# (code, field width, bias) of the compressed values, smallest range first
CODES = [(1, 2, 1), (2, 2, -4), (3, 4, 5), (4, 4, -20), (5, 6, 21), (6, 6, -84), (7, 8, 85), (8, 8, -340),
         (9, 10, 341), (10, 10, -1364), (11, 12, 1365), (12, 12, -5460)]


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, width):
        self.bits += [(value >> (width - 1 - i)) & 1 for i in range(width)]

    def bytes(self):
        b = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int(''.join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


def code_of(v):
    if v == 0:
        return 0, 0, 0
    for c, w, bias in CODES:
        if 0 <= v - bias < (1 << w):
            return c, w, bias
    return 13, 16, 0


def chunk(values, runs=True, end=False):
    """One component's chunk: the 6-bit length, then the values (runs of a repeated code as code 14 when
    `runs`), then an end code if `end`."""
    w = Bits()
    w.put(0, 6)
    i = 0
    while i < len(values):
        c, width, bias = code_of(values[i])
        n = 1
        while runs and i + n < len(values) and n < 15 and code_of(values[i + n])[0] == c:
            n += 1
        if n >= 3 and c != 0:
            w.put(c, 4)
            w.put((values[i] - bias) & 0xFFFF, width)      # the first one sets the code
            w.put(14, 4)
            w.put(n - 1, 4)                                 # the rest reuse it
            for v in values[i + 1:i + n]:
                w.put((v - bias) & 0xFFFF, width)
            i += n
        else:
            w.put(c, 4)
            if c:
                w.put((values[i] - bias) & 0xFFFF, width)
            i += 1
    if end:
        w.put(15, 4)
    b = bytearray(w.bytes())
    assert len(b) < 64
    b[0] = (b[0] & 0x03) | (len(b) << 2)
    return bytes(b)


def compressed(descriptors, first, states, rot_shift=0, tr_shift=0, scales=None, endian='>', runs=True,
               end_short=False):
    """A 0x64 animation: frame-0 values `first` (rotations i16, others f32), then the integer state of every
    component for frames 1.. (states: (frames - 1, nch, 3)), encoded as the game reads it."""
    nch = len(descriptors)
    frames = len(states) + 1
    rot = [animation.is_rotation(d) for d in descriptors]
    scales = scales if scales is not None else [0.5] * rot.count(False)
    e = endian
    head = struct.pack(e + 'HH', 0x64, nch) + struct.pack(e + '%dH' % nch, *descriptors)
    head += struct.pack(e + 'HBBH', frames, rot_shift, tr_shift, len(scales)) + struct.pack(e + '%df' % len(scales),
                                                                                           *scales)
    init = []
    for c in range(nch):
        if rot[c]:
            head += struct.pack(e + '3h', *first[c])
            init.append([v >> rot_shift for v in first[c]])
        else:
            head += struct.pack(e + '3f', *first[c])
            init.append([0, 0, 0])
    nb = (frames + 14) >> 4
    blocks = []
    for b in range(nb):
        rows = states[16 * b:16 * b + 16]
        data = b''
        for c in range(nch):
            for comp in range(3):
                seq = [int(r[c][comp]) for r in rows]
                vals = [seq[0] - init[c][comp]]
                prev_d = 0
                for k in range(1, len(seq)):
                    d = seq[k] - seq[k - 1]
                    vals.append(d - prev_d)
                    prev_d = d
                done = end_short and len(seq) > 1 and all(v == 0 for v in vals[1:])
                if done:
                    vals = vals[:1]
                data += chunk(vals, runs, end=done)
        blocks.append(data)
    table, at = [], 4 * nb
    for blk in blocks:
        table.append(at)
        at += len(blk)
    return head + struct.pack(e + '%dI' % nb, *table) + b''.join(blocks)


def expected(descriptors, first, states, rot_shift, tr_shift, scales):
    """The values the game computes, from the spec (single precision like the console)."""
    rot = [animation.is_rotation(d) for d in descriptors]
    frames = len(states) + 1
    out = np.zeros((frames, len(descriptors), 3), np.float32)
    fi = 0
    for c, d in enumerate(descriptors):
        for comp in range(3):
            if rot[c]:
                out[0, c, comp] = np.float32(np.float32(first[c][comp]) * UNIT)
            else:
                out[0, c, comp] = np.float32(first[c][comp])
            for f in range(1, frames):
                s = int(states[f - 1][c][comp])
                if rot[c]:
                    out[f, c, comp] = np.float32(np.float32(s << rot_shift) * UNIT)
                else:
                    x = float(np.float32(s << tr_shift)) * float(np.float32(scales[fi])) + float(out[0, c, comp])
                    out[f, c, comp] = np.float32(x)
        fi += 0 if rot[c] else 1
    return out


def random_animation(rng, frames, descriptors, rot_shift=2, tr_shift=1, smooth=True):
    nch = len(descriptors)
    rot = [animation.is_rotation(d) for d in descriptors]
    first, states = [], np.zeros((frames - 1, nch, 3), np.int64)
    for c in range(nch):
        if rot[c]:
            f0 = [rng.randrange(-32768, 32768) for _ in range(3)]
            start = [v >> rot_shift for v in f0]
        else:
            f0 = [float(np.float32(rng.uniform(-1000, 1000))) for _ in range(3)]
            start = [0, 0, 0]
        first.append(f0)
        for comp in range(3):
            s, v = start[comp], 0
            for f in range(frames - 1):
                if smooth:
                    v += rng.randrange(-40, 41)
                    s += v
                else:
                    s = rng.randrange(-32768, 32768) >> rot_shift if rot[c] else rng.randrange(-20000, 20000)
                s = max(-32768 >> rot_shift, min((32767 >> rot_shift), s))
                states[f, c, comp] = s
    return first, states


class Compressed(unittest.TestCase):
    def check(self, descriptors, first, states, rs=0, ts=0, scales=None, endian='>', **kw):
        scales = scales if scales is not None else [0.5] * sum(not animation.is_rotation(d) for d in descriptors)
        b = compressed(descriptors, first, states, rs, ts, scales, endian, **kw)
        want = expected(descriptors, first, states, rs, ts, scales)
        got = animation.decode(b, endian)
        self.assertEqual(got['type'], 0x64)
        self.assertEqual(got['frames'], len(states) + 1)
        self.assertEqual(got['descriptors'], list(descriptors))
        self.assertEqual(got['values'].dtype, np.float32)
        np.testing.assert_array_equal(got['values'], want)
        self.assertEqual(animation.header(b, endian)['size'], len(b))
        for f in sorted({0, min(1, len(states)), len(states) // 2, len(states)}):
            np.testing.assert_array_equal(animation.decode_frame(b, f, endian), want[f])
        return b, want

    def test_hand_made(self):
        # one rotation channel (z only moves) and one translation channel, 4 frames
        desc = [6, 11]
        first = [[0, 0, 100], [1.0, 2.0, 3.0]]
        states = np.array([[[0, 0, 110], [0, 4, 0]],
                           [[0, 0, 130], [0, 8, 0]],
                           [[0, 0, 160], [0, 12, 0]]])
        b, want = self.check(desc, first, states, scales=[0.25])
        self.assertAlmostEqual(float(want[3, 0, 2]), 160 * np.pi / 32768, places=6)
        self.assertEqual(float(want[3, 1, 1]), 2.0 + 12 * 0.25)

    def test_every_code_width(self):
        # second differences that need every code (0, 2..12 bits both signs, and 16 bits)
        diffs = [0, 3, -3, 12, -12, 50, -50, 200, -200, 900, -900, 3000, -3000, 9000, -9000]
        seq, s, v = [], 0, 0
        for d in diffs:
            v += d
            s += v
            seq.append(s)
        seq = [((x + 0x8000) % 0x10000) - 0x8000 for x in seq]
        states = np.array([[[0, 0, x]] for x in seq])
        self.check([11], [[0.0, 0.0, 0.0]], states, scales=[1.0])

    def test_runs_and_end(self):
        rng = random.Random(3)
        first, states = random_animation(rng, 40, [7, 11, 6], smooth=True)
        states[:, 0, 0] = states[0, 0, 0]          # a still component: its chunks end early
        states[20:, 1, 2] = states[19, 1, 2]
        self.check([7, 11, 6], first, states, 2, 1, [0.1, ], runs=True, end_short=True)
        self.check([7, 11, 6], first, states, 2, 1, [0.1, ], runs=False)

    def test_random_both_endians(self):
        rng = random.Random(7)
        desc = list(animation.BODY_DESCRIPTORS)
        for frames, smooth in ((1, True), (2, True), (16, True), (17, True), (18, False), (33, True),
                               (50, False)):
            first, states = random_animation(rng, frames, desc, smooth=smooth)
            scales = [float(np.float32(rng.uniform(0.01, 2))) for _ in range(desc.count(11))]
            b, want = self.check(desc, first, states, 2, 1, scales, '>')
            b2, _ = self.check(desc, first, states, 2, 1, scales, '<')
            self.assertNotEqual(b, b2)

    def test_state_wraps_as_i16(self):
        states = np.array([[[0, 0, 32000]], [[0, 0, -32000]], [[0, 0, 31000]]])   # differences overflow i16
        self.check([11], [[0.0, 0.0, 0.0]], states, scales=[1.0])

    def test_frame_clamped(self):
        rng = random.Random(1)
        first, states = random_animation(rng, 20, [7, 11])
        b = compressed([7, 11], first, states, 2, 1, [0.3])
        v = animation.decode(b)['values']
        np.testing.assert_array_equal(animation.decode_frame(b, 99), v[-1])
        np.testing.assert_array_equal(animation.decode_frame(b, -5), v[0])

    def test_run_carried_from_previous_chunk(self):
        # a chunk may start with code 14: it reuses the code the previous chunk's last value read, and that depends
        # on how many values the game read there (the frame within the block)
        desc = [11]
        e = '>'
        c0 = chunk([5, 1, 1], runs=False)     # x: frame 1 reads one value (code 3), frames 2, 3 end on code 1
        w = Bits()                                # y: code 14, count 3, then fields of the carried code's width
        w.put(0, 6)
        for value, width in [(14, 4), (3, 4), (2, 2), (1, 2), (1, 2)]:
            w.put(value, width)
        c1 = bytearray(w.bytes())
        c1[0] = (c1[0] & 3) | (len(c1) << 2)
        c2 = chunk([0, 0, 0], runs=False)
        frames = 4
        head = struct.pack(e + 'HHH', 0x64, 1, 11) + struct.pack(e + 'HBBHf', frames, 0, 0, 1, 1.0)
        head += struct.pack(e + '3f', 0, 0, 0)
        b = head + struct.pack(e + 'I', 4) + c0 + bytes(c1) + c2
        got = animation.decode(b, e)['values'][:, 0]
        # x: 5, then velocity 1, 2 -> 5, 6, 8
        np.testing.assert_array_equal(got[:, 0], [0, 5, 6, 8])
        # y at frame 1: the carried code is 3 (4 bits, +5) -> the run reads a 4-bit field: 0b1001 = 9 -> 14
        # frames 2, 3: the carried code is 1 (2 bits, +1): fields 2, 1, 1 -> 3, then +2, then +2+2
        self.assertEqual(got[1, 1], 14.0)
        np.testing.assert_array_equal(got[2:, 1], [3 + 2, 3 + 2 + 4])


class Raw(unittest.TestCase):
    def test_round_trip(self):
        rng = np.random.default_rng(5)
        desc = list(animation.BODY_DESCRIPTORS)
        vals = rng.normal(size=(7, len(desc), 3)).astype(np.float32)
        for e in '<>':
            b = animation.encode_raw(desc, vals, e)
            self.assertEqual(len(b), 8 + 4 * len(desc) + 12 * len(desc) * 7)
            self.assertEqual(struct.unpack_from(e + 'HHI', b, 0), (0xC8, len(desc), 7))
            d = animation.decode(b, e)
            self.assertEqual(d['type'], 0xC8)
            self.assertEqual(d['frames'], 7)
            self.assertEqual(d['descriptors'], desc)
            np.testing.assert_array_equal(d['values'], vals)
            np.testing.assert_array_equal(animation.decode_frame(b, 3, e), vals[3])
            self.assertEqual(animation.header(b, e)['size'], len(b))

    def test_offset_and_shape_check(self):
        vals = np.zeros((2, 2, 3), np.float32)
        b = b'\0' * 12 + animation.encode_raw([11, 7], vals)
        self.assertEqual(animation.decode(b, '>', 12)['frames'], 2)
        with self.assertRaises(ValueError):
            animation.encode_raw([11], vals)
        with self.assertRaises(ValueError):
            animation.decode(struct.pack('>HH', 0x99, 0))


class Rotations(unittest.TestCase):
    def test_compressed_axis_angle(self):
        v = [0.3, -1.1, 0.7]
        a = animation.rotation_axis(v)
        np.testing.assert_allclose(a, [np.cos(0.3) * np.sin(-1.1), -np.sin(0.3), np.cos(0.3) * np.cos(-1.1)])
        q = animation.game_quaternion(v)
        np.testing.assert_allclose(q, np.append(a * np.sin(0.35), np.cos(0.35)))
        r = animation.local_rotation(v)
        np.testing.assert_allclose(r @ a, a, atol=1e-12)                  # the axis stays
        np.testing.assert_allclose(r @ r.T, np.eye(3), atol=1e-12)
        self.assertAlmostEqual(float(np.trace(r)), 1 + 2 * np.cos(0.7), places=12)
        # the local rotation turns by -z about the axis (the conjugate of the game's quaternion)
        np.testing.assert_allclose(r, animation.quaternion_to_matrix(q).T, atol=1e-12)
        # hinge: x = y = 0 -> axis +z, local rotation Rz(-z)
        np.testing.assert_allclose(animation.local_rotation([0, 0, 0.5]), animation.euler_to_matrix([0, 0, -0.5]),
                                   atol=1e-12)
        np.testing.assert_allclose(animation.local_rotation(animation.axis_angle_from_matrix(r)), r, atol=1e-9)
        np.testing.assert_allclose(animation.axis_angle_from_matrix(np.eye(3)), [0, 0, 0])
        half = animation.local_rotation([0.2, 0.4, np.pi])
        np.testing.assert_allclose(animation.local_rotation(animation.axis_angle_from_matrix(half)), half, atol=1e-6)

    def test_raw_euler(self):
        v = [0.1, 0.2, 0.3]
        r = animation.euler_to_matrix(v)
        cx, sx = np.cos(0.1), np.sin(0.1)
        rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
        cy, sy = np.cos(0.2), np.sin(0.2)
        ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
        cz, sz = np.cos(0.3), np.sin(0.3)
        rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
        np.testing.assert_allclose(r, rz @ ry @ rx, atol=1e-12)
        np.testing.assert_allclose(animation.local_rotation(v, animation.RAW), r)
        np.testing.assert_allclose(animation.euler_from_matrix(r), v, atol=1e-12)
        # the game's quaternion of a raw rotation is the conjugate of the local rotation's
        q = animation.game_quaternion(v, animation.RAW)
        np.testing.assert_allclose(animation.quaternion_to_matrix(q), r.T, atol=1e-12)

    def test_to_raw_keeps_the_pose(self):
        rng = np.random.default_rng(3)
        desc = list(animation.BODY_DESCRIPTORS)
        vals = rng.uniform(-3, 3, size=(5, len(desc), 3))
        vals[:, 11, :2] = 0                                                # a hinge
        raw = animation.to_raw(desc, vals)
        self.assertEqual(raw.dtype, np.float32)
        for c, d in enumerate(desc):
            if animation.is_rotation(d):
                np.testing.assert_allclose(animation.local_rotation(raw[:, c].astype(np.float64), animation.RAW),
                                           animation.local_rotation(vals[:, c]), atol=2e-6)
            else:
                np.testing.assert_array_equal(raw[:, c], vals[:, c].astype(np.float32))
        np.testing.assert_allclose(raw[:, 11, 2], -vals[:, 11, 2].astype(np.float32), atol=2e-6)   # hinge sign flips
        b = animation.encode_raw(desc, raw)
        np.testing.assert_array_equal(animation.decode(b)['values'], raw)

    def test_body_tables(self):
        self.assertEqual(len(animation.BODY_DESCRIPTORS), 23)
        self.assertEqual(len(animation.BODY_BONES), 23)
        self.assertEqual(len(animation.BODY_CHANNELS), 23)
        for c, b in enumerate(animation.BODY_BONES):
            if b >= 0:
                self.assertTrue(animation.is_rotation(animation.BODY_DESCRIPTORS[c]))


if __name__ == '__main__':
    unittest.main()
