"""Body animations of TTT2 (Wii U, big-endian) and Tekken 7 (PC, little-endian): decoded to per-frame channel values,
and written back as raw animations.

An animation starts with u16 type and u16 channel count (nch). Each channel carries 3 values a frame; its descriptor
says what they are:
    11    a translation (x, y, z), in millimetres (the skeleton's units, before the fighter's size scale); the
          other descriptors that are not rotations are plain values too (3 in face animations, 15 and 21 in camera
          animations)
    4..7  a rotation, in radians, whose meaning depends on the animation's type (the 4..7 test is the only one the
          game's sampler sub_035866BC makes; it hands on each rotation as a quaternion (x, y, z, w), translations as
          (x, y, z, 1)):
          compressed (0x64): AXIS-ANGLE. x, y = the axis's direction, axis = (cos x sin y, -sin x, cos x cos y);
              z = the angle about it. The game's quaternion is (axis sin(z/2), cos(z/2)). Descriptor 6 = a hinge
              (x = y = 0: the axis is +z; elbows and knees), 5 = y always 0 (axis in the y-z plane; the yaw channel
              has x = -+pi/2: the vertical axis), 7 = any axis.
          raw (0xC8): EULER angles. The game's quaternion is the conjugate of that of Rz(z) Ry(y) Rx(x).
    The bone's local rotation in the .bon convention (column vectors, v_parent = R v_child + t, the bind rotations'
    R = Rz Ry Rx) is the conjugate of the game's quaternion: local_rotation() below. (The game's bone record keeps
    the transpose of R, row-major: rows = local axes; checked against the records of 4 fighters in play.)

The 23 body channels (BODY_CHANNELS; the game's channel -> bone table at 0x104BF338, used by its applier
sub_06896120):
    0 (11) the fighter's ground displacement x, z (y = 0; root motion, moves the player)
    1 (11) the model root: hips position (y = height)
    2 (5)  the turn: a rotation about the vertical axis, added to the player's facing (no bone)
    3 HARA_ROT1, 4 MUNE1 (chest; the game spreads it over MUNE1 and MUNE2 afterwards, sub_069E1168),
    5 KOSHI (pelvis), 6 (11) a spine parameter, x usually -400 (no bone; read by sub_069E1168),
    7 KUBI, 8 ATAMA, 9..12 SAKOTSU_R, KATA_R, UDE_R (hinge), TE_R, 13..16 the same on the left,
    17..19 MOMO_R, HIZA_R (hinge), ASHI_R, 20..22 MOMO_L, HIZA_L (hinge), ASHI_L.
    Channels after the 23rd come in (11, 7) pairs for a fighter's extra (OPT) bones.
    The sampler negates the z of channels 0 and 1 for the body (its third argument).
    Bone hierarchy (.bon parents): HARA_ROT1 root; MUNE1, KOSHI <- HARA_ROT1; MUNE2 <- MUNE1; KUBI, SAKOTSU_R/L <-
    MUNE2; ATAMA <- KUBI; KATA <- SAKOTSU; UDE <- KATA; TE <- UDE; MOMO_R/L <- KOSHI; HIZA <- MOMO; ASHI <- HIZA;
    TSUMASAKI (toes, not animated) <- ASHI. Local translations come from the skeleton (bind), rotations from the
    channels.

Type 0xC8, raw:
    u16 0xC8, u16 nch, u32 frames, nch x u32 descriptors, then frames x nch x 3 f32 (frame-major).
Type 0x64, compressed:
    u16 0x64, u16 nch, nch x u16 descriptors, then at p = 4 + 2 nch:
    u16 frames, u8 rotation shift (low 7 bits used), u8 translation shift, u16 n (= the number of non-rotation
    channels), n x f32 scales (one per non-rotation channel, in channel order), then each channel's frame-0 values
    (rotation: 3 x i16 in units of pi/32768; others: 3 x f32), then ((frames + 14) >> 4) x u32 block offsets
    (counted from the start of this offset table), then the blocks.
    Frame 0 is the frame-0 values as they are. Frame f >= 1 lies in block b = (f - 1) >> 4 (frames 16b+1 .. 16b+16).
    A block holds, for every channel and each of its 3 components in order, a chunk of L bytes that starts with L
    itself (6 bits); the next chunk starts L bytes after this one. The chunk is an MSB-first bit stream of values:
        a 4-bit code, then a field of the code's width read as an unsigned number, plus the code's bias:
        code  0: the value 0              code  1/2: 2 bits, +1 / -4      code  3/4: 4 bits, +5 / -20
        code  5/6: 6 bits, +21 / -84      code  7/8: 8 bits, +85 / -340   code  9/10: 10 bits, +341 / -1364
        code 11/12: 12 bits, +1365 / -5460                                code 13: 16 bits, a signed i16
        code 14: a 4-bit count c, then c values that all reuse the previous code (its field, its bias)
        code 15: the end (no further values in this chunk)
    The previous code carries over from one chunk to the next within a frame's decoding (it starts as code 0 at the
    first channel); a pending code-14 count does not.
    The component's integer state starts as the frame-0 value >> rotation shift (rotations) or 0 (translations).
    The chunk's first value v0 moves it to frame 16b+1 (state += v0); the following values are second differences:
    velocity starts at 0, and for each later frame velocity += value, state += velocity. After the end code the
    state stays. The state is an i16 (it wraps).
    Rotation = f32(state << rotation shift) * pi/32768; translation = f32(state << translation shift) * scale + its
    frame-0 value.
    This is what the game's decoder (sub_03585C8C(out, &frames, rotation flags, frame, nch, channels to decode),
    values sub_03585988, bits sub_0358592C) computes; decode() returns exactly its single-precision results (checked
    bit for bit against the running game). Some callers decode only the first 2 or 3 channels (root motion). The
    frame is clamped to 0 .. frames - 1. Between whole frames the sampler decodes both neighbours and blends them
    (sub_0358A90C, not modelled here).

Written for this project from the game's code and data (no third-party code).
"""
import struct

import numpy as np

COMPRESSED = 0x64
RAW = 0xC8
ROTATION_UNIT = np.float32(np.pi / 32768)         # 9.58738e-05, the game's constant at 0x10179F18
END = 0xF0000000                                   # the value sub_03585988 returns for code 15
_CODES = {1: (2, 1), 2: (2, -4), 3: (4, 5), 4: (4, -20), 5: (6, 21), 6: (6, -84), 7: (8, 85), 8: (8, -340),
          9: (10, 341), 10: (10, -1364), 11: (12, 1365), 12: (12, -5460), 13: (16, 0)}
_END_MODE = 99

TRANSLATION, ROTATION_XZ, HINGE_Z, ROTATION = 11, 5, 6, 7

BODY_DESCRIPTORS = (11, 11, 5, 7, 7, 7, 11, 7, 7, 7, 7, 6, 7, 7, 7, 6, 7, 7, 6, 7, 7, 6, 7)
# channel -> bone index in the TTT2 skeletons (-1: no bone), the game's own table (0x104BF338)
BODY_BONES = (-1, -1, -1, 0, 1, 3, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19)
BODY_CHANNELS = ('move', 'root', 'turn', 'HARA_ROT1', 'MUNE1', 'KOSHI', 'spine', 'KUBI', 'ATAMA', 'SAKOTSU_R',
                 'KATA_R', 'UDE_R', 'TE_R', 'SAKOTSU_L', 'KATA_L', 'UDE_L', 'TE_L', 'MOMO_R', 'HIZA_R', 'ASHI_R',
                 'MOMO_L', 'HIZA_L', 'ASHI_L')


def is_rotation(descriptor):
    return 4 <= descriptor <= 7


def _i16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


class _Bits:
    """sub_0358592C: an MSB-first bit reader (fields of up to 16 bits). Past the end of the buffer it reads zeros."""

    def __init__(self, b, pos):
        self.b, self.pos, self.acc, self.avail = b, pos, 0, 0

    def read(self, n):
        while self.avail < n:
            self.acc = ((self.acc << 8) | (self.b[self.pos] if self.pos < len(self.b) else 0)) & 0xFFFFFFFF
            self.pos += 1
            self.avail += 8
        self.avail -= n
        r = (self.acc >> self.avail) & 0xFFFF
        self.acc &= (1 << self.avail) - 1
        return r


def _value(rd, st):
    """sub_03585988: the next value of a chunk. st = [pending count, field width (99: end), bias]."""
    if st[0]:
        st[0] -= 1
    else:
        c = rd.read(4)
        if c == 0:
            st[1] = st[2] = 0
            return 0
        if c == 15:
            st[1], st[2] = _END_MODE, 0
            return END
        if c == 14:
            st[0] = (rd.read(4) - 1) & 0xFFFF
        else:
            st[1], st[2] = _CODES[c]
    if st[1] == 0:
        return 0
    if st[1] == _END_MODE:
        return END
    return _i16(rd.read(st[1])) + st[2]


def _chunk(b, ptr, carry, count):
    """The chunk at ptr: (its length L, [values read], [(width, bias) after each read]) for up to `count` reads
    that the game would make (it stops reading at an end code after the first value)."""
    rd = _Bits(b, ptr)
    length = rd.read(6)
    st = [0, carry[0], carry[1]]
    vals, modes = [], []
    for j in range(count):
        v = _value(rd, st)
        vals.append(v)
        modes.append((st[1], st[2]))
        if j and v == END:
            break
    return length, vals, modes


def header(buf, endian='>', offset=0):
    """The animation's header fields: dict(type, nch, frames, descriptors, size (bytes), ...); for 0x64 also the
    shifts (rotation_flag = bit 7 of the rotation shift byte, which the game's decoder masks off), scales, frame-0
    values (as stored), where the block offsets start (table) and the offsets."""
    e, o = endian, offset
    typ, nch = struct.unpack_from(e + 'HH', buf, o)
    if typ == RAW:
        frames, = struct.unpack_from(e + 'I', buf, o + 4)
        desc = struct.unpack_from(e + '%dI' % nch, buf, o + 8)
        return dict(type=typ, nch=nch, frames=frames, descriptors=list(desc), data=o + 8 + 4 * nch,
                    size=8 + 4 * nch + 12 * nch * frames)
    if typ != COMPRESSED:
        raise ValueError('animation type %#x is neither 0x64 nor 0xC8' % typ)
    desc = struct.unpack_from(e + '%dH' % nch, buf, o + 4)
    p = o + 4 + 2 * nch
    frames, shr, shf, n = struct.unpack_from(e + 'HBBH', buf, p)
    rot = [is_rotation(d) for d in desc]
    if n != rot.count(False):
        raise ValueError('compressed animation: %d scales for %d translation channels' % (n, rot.count(False)))
    scales = struct.unpack_from(e + '%df' % n, buf, p + 6)
    q = p + 6 + 4 * n
    first = []
    for r in rot:
        if r:
            first.append(struct.unpack_from(e + '3h', buf, q))
            q += 6
        else:
            first.append(struct.unpack_from(e + '3f', buf, q))
            q += 12
    nblocks = (frames + 14) >> 4
    offsets = struct.unpack_from(e + '%dI' % nblocks, buf, q)
    h = dict(type=typ, nch=nch, frames=frames, descriptors=list(desc), rotation_shift=shr & 0x7F,
             rotation_flag=shr >> 7, translation_shift=shf, scales=list(scales), first=first, table=q,
             offsets=list(offsets))
    h['size'] = _compressed_size(buf, h) - o
    return h


def _compressed_size(buf, h):
    """The end of the last block (the chunk lengths of the last block added up)."""
    if not h['offsets']:
        return h['table']
    ptr = h['table'] + h['offsets'][-1]
    for _ in range(3 * h['nch']):
        ptr += _Bits(buf, ptr).read(6)
    return max(ptr, h['table'] + 4 * len(h['offsets']))


def _frame0(h):
    out = np.zeros((h['nch'], 3), np.float32)
    for c, (d, v) in enumerate(zip(h['descriptors'], h['first'])):
        out[c] = np.float32(np.array(v, np.float32) * ROTATION_UNIT) if is_rotation(d) else np.array(v, np.float32)
    return out


def _decode_compressed(buf, h, frames=None):
    """Values (len(frames), nch, 3) float32 of the frames asked for (default: all)."""
    nf = h['frames']
    want = list(range(nf)) if frames is None else [min(max(int(f), 0), nf - 1) if nf else 0 for f in frames]
    nch, shr, shf = h['nch'], h['rotation_shift'], h['translation_shift']
    rot = np.array([is_rotation(d) for d in h['descriptors']], bool)
    base = _frame0(h)
    init = [[v >> shr for v in h['first'][c]] if rot[c] else [0, 0, 0] for c in range(nch)]
    states = np.zeros((len(want), nch, 3), np.int64)
    blocks = {}
    for i, f in enumerate(want):
        if f:
            b = (f - 1) >> 4
            if b not in blocks:
                blocks[b] = _decode_block(buf, h, b, init)
            states[i] = blocks[b][(f - 1) & 15]
    out = np.empty((len(want), nch, 3), np.float32)
    out[:, rot] = (states[:, rot] << shr).astype(np.float32) * ROTATION_UNIT
    tr = ~rot
    if tr.any():
        scale = np.array(h['scales'], np.float32).astype(np.float64)[None, :, None]
        x = (states[:, tr] << shf).astype(np.float32).astype(np.float64) * scale + base[tr].astype(np.float64)
        out[:, tr] = x.astype(np.float32)
    first = np.array(want) == 0
    out[first] = base
    return out


def _chunk_states(vals, modes, start, count):
    """Per frame k of a block: the component's state and the (width, bias) carried to the next chunk."""
    states, carry = [], []
    s = start + (0 if vals[0] == END else vals[0])
    vel, n, ended = 0, 1, False
    for k in range(count):
        if k and not ended:
            v = vals[k]
            n = k + 1
            if v == END:
                ended = True
            else:
                vel += v
                s += vel
        states.append(s)
        carry.append(modes[n - 1])
    return states, carry


def _decode_block(buf, h, b, init):
    """(count, nch, 3) integer states of frames 16b+1 .. 16b+count, as the game computes each frame on its own (only
    a chunk that starts with code 14 depends on the code the previous chunk left, which can differ by frame)."""
    count = min(16, h['frames'] - 1 - 16 * b)
    ptr = h['table'] + h['offsets'][b]
    carry = [(0, 0)] * count
    states = np.zeros((count, h['nch'], 3), np.int64)
    for c in range(h['nch']):
        for comp in range(3):
            rd = _Bits(buf, ptr)
            length = rd.read(6)
            if rd.read(4) != 14:
                length, vals, modes = _chunk(buf, ptr, (0, 0), count)
                col, carry = _chunk_states(vals, modes, init[c][comp], count)
            else:
                col, out = [0] * count, [None] * count
                for key in set(carry):
                    length, vals, modes = _chunk(buf, ptr, key, count)
                    ss, cc = _chunk_states(vals, modes, init[c][comp], count)
                    for k in range(count):
                        if carry[k] == key:
                            col[k], out[k] = ss[k], cc[k]
                carry = out
            states[:, c, comp] = col
            ptr += length
    return (states + 0x8000) % 0x10000 - 0x8000


def decode(buf, endian='>', offset=0):
    """-> dict(type, frames, descriptors, values: float32 array (frames, nch, 3)) of the animation at buf[offset:]
    (exactly the game's numbers for whole frames; rotations in radians: axis-angle for 0x64, Euler for 0xC8, see
    local_rotation() and to_raw(); translations in millimetres)."""
    buf = bytes(buf)
    h = header(buf, endian, offset)
    if h['type'] == RAW:
        vals = np.frombuffer(buf, endian + 'f4', h['frames'] * h['nch'] * 3, h['data'])
        vals = vals.reshape(h['frames'], h['nch'], 3).astype(np.float32)
    else:
        vals = _decode_compressed(buf, h)
    return dict(type=h['type'], frames=h['frames'], descriptors=h['descriptors'], values=vals)


def decode_frame(buf, frame, endian='>', offset=0):
    """(nch, 3) float32 values of one whole frame (clamped to the animation, as the game does)."""
    buf = bytes(buf)
    h = header(buf, endian, offset)
    if h['type'] == RAW:
        f = min(max(int(frame), 0), h['frames'] - 1)
        return np.frombuffer(buf, endian + 'f4', h['nch'] * 3, h['data'] + 12 * h['nch'] * f) \
            .reshape(h['nch'], 3).astype(np.float32)
    return _decode_compressed(buf, h, [frame])[0]


def encode_raw(descriptors, values, endian='>'):
    """A type 0xC8 animation: descriptors (nch ints), values (frames, nch, 3) in radians / millimetres."""
    values = np.asarray(values, np.float32)
    if values.ndim != 3 or values.shape[2] != 3 or values.shape[1] != len(descriptors):
        raise ValueError('values must be (frames, %d, 3), not %s' % (len(descriptors), values.shape))
    out = struct.pack(endian + 'HHI', RAW, len(descriptors), values.shape[0])
    out += struct.pack(endian + '%dI' % len(descriptors), *descriptors)
    return out + values.astype(endian + 'f4').tobytes()


def euler_to_matrix(angles):
    """3x3 rotation matrices (..., 3, 3) of Euler angles (..., 3) = (x, y, z) radians: R = Rz(z) Ry(y) Rx(x), acting
    on column vectors (the convention of the .bon skeleton's rotations and of raw animations)."""
    a = np.asarray(angles, np.float64)
    cx, cy, cz = np.cos(a[..., 0]), np.cos(a[..., 1]), np.cos(a[..., 2])
    sx, sy, sz = np.sin(a[..., 0]), np.sin(a[..., 1]), np.sin(a[..., 2])
    r = np.empty(a.shape[:-1] + (3, 3))
    r[..., 0, 0] = cz * cy
    r[..., 0, 1] = cz * sy * sx - sz * cx
    r[..., 0, 2] = cz * sy * cx + sz * sx
    r[..., 1, 0] = sz * cy
    r[..., 1, 1] = sz * sy * sx + cz * cx
    r[..., 1, 2] = sz * sy * cx - cz * sx
    r[..., 2, 0] = -sy
    r[..., 2, 1] = cy * sx
    r[..., 2, 2] = cy * cx
    return r


def euler_from_matrix(m):
    """(x, y, z) with euler_to_matrix((x, y, z)) = m (y in [-pi/2, pi/2])."""
    m = np.asarray(m, np.float64)
    y = np.arcsin(np.clip(-m[2, 0], -1, 1))
    if abs(m[2, 0]) < 1 - 1e-9:
        return np.array([np.arctan2(m[2, 1], m[2, 2]), y, np.arctan2(m[1, 0], m[0, 0])])
    return np.array([0.0, y, np.arctan2(-m[0, 1], m[1, 1])])         # gimbal lock: x folded into z


def rotation_axis(values):
    """The unit axis (..., 3) of compressed rotation values (..., 3): (cos x sin y, -sin x, cos x cos y)."""
    v = np.asarray(values, np.float64)
    x, y = v[..., 0], v[..., 1]
    return np.stack([np.cos(x) * np.sin(y), -np.sin(x), np.cos(x) * np.cos(y)], -1)


def _qmul(a, b):
    ax, ay, az, aw = np.moveaxis(a, -1, 0)
    bx, by, bz, bw = np.moveaxis(b, -1, 0)
    return np.stack([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz], -1)


def game_quaternion(values, typ=COMPRESSED):
    """The quaternion (..., 4) = (x, y, z, w) the game's sampler makes of rotation values (..., 3):
    compressed: (axis sin(z/2), cos(z/2)) (sub_035866BC); raw: qx(-x) qy(-y) qz(-z) (sub_0358C6E4)."""
    v = np.asarray(values, np.float64)
    if typ == COMPRESSED:
        half = v[..., 2] / 2
        return np.concatenate([rotation_axis(v) * np.sin(half)[..., None], np.cos(half)[..., None]], -1)

    def axis(i):
        q = np.zeros(v.shape[:-1] + (4,))
        q[..., i] = np.sin(-v[..., i] / 2)
        q[..., 3] = np.cos(-v[..., i] / 2)
        return q
    return _qmul(_qmul(axis(0), axis(1)), axis(2))


def quaternion_to_matrix(q):
    """3x3 matrices (..., 3, 3) of quaternions (..., 4) = (x, y, z, w), acting on column vectors. (Of a game
    quaternion this is the transpose of the bone's local rotation: what the game's bone record holds row-major.)"""
    q = np.asarray(q, np.float64)
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    r = np.empty(q.shape[:-1] + (3, 3))
    r[..., 0, 0] = 1 - 2 * (y * y + z * z)
    r[..., 0, 1] = 2 * (x * y - z * w)
    r[..., 0, 2] = 2 * (x * z + y * w)
    r[..., 1, 0] = 2 * (x * y + z * w)
    r[..., 1, 1] = 1 - 2 * (x * x + z * z)
    r[..., 1, 2] = 2 * (y * z - x * w)
    r[..., 2, 0] = 2 * (x * z - y * w)
    r[..., 2, 1] = 2 * (y * z + x * w)
    r[..., 2, 2] = 1 - 2 * (x * x + y * y)
    return r


def local_rotation(values, typ=COMPRESSED):
    """The bone's local rotation matrices (..., 3, 3) of rotation values (..., 3), in the .bon convention (column
    vectors): raw = Rz(z) Ry(y) Rx(x); compressed = a rotation by -z about rotation_axis(values)."""
    if typ == RAW:
        return euler_to_matrix(values)
    return np.swapaxes(quaternion_to_matrix(game_quaternion(values, COMPRESSED)), -1, -2)


def axis_angle_from_matrix(m):
    """Compressed rotation values (x, y, z) with local_rotation(values, COMPRESSED) = m (z in [0, pi]; the identity
    gives (0, 0, 0))."""
    m = np.asarray(m, np.float64).T                # the game's rotation: +z about the axis
    angle = np.arccos(np.clip((np.trace(m) - 1) / 2, -1, 1))
    if angle < 1e-9:
        return np.zeros(3)
    axis = np.array([m[2, 1] - m[1, 2], m[0, 2] - m[2, 0], m[1, 0] - m[0, 1]])
    if np.linalg.norm(axis) < 1e-6:                # a half turn: the axis from the symmetric part
        s = (m + np.eye(3)) / 2
        d = np.sqrt(np.clip(np.diag(s), 0, None))
        k = int(np.argmax(d))
        axis = s[k] / d[k]
    axis = axis / np.linalg.norm(axis)
    return np.array([-np.arcsin(np.clip(axis[1], -1, 1)), np.arctan2(axis[0], axis[2]), angle])


def to_raw(descriptors, values, typ=COMPRESSED):
    """Values (frames, nch, 3) of a decoded animation of type `typ`, with every rotation channel turned into the
    Euler angles a raw animation needs for the same pose (translations unchanged), ready for encode_raw()."""
    out = np.array(values, np.float64)
    if typ == RAW:
        return out.astype(np.float32)
    for c, d in enumerate(descriptors):
        if is_rotation(d):
            mats = local_rotation(out[:, c], COMPRESSED)
            out[:, c] = [euler_from_matrix(m) for m in mats]
    return out.astype(np.float32)
