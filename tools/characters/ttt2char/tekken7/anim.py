"""Tekken 7 animations (UE 4.14 AnimSequence, cooked), decoded to per-frame local bone transforms.

What a cooked AnimSequence holds after its properties (NumFrames, SequenceLength, TrackToSkeletonMapTable):
    FGuid skeleton (16 bytes), strip flags (2 bytes), u32 1, u8 key encoding (1: variable keys), u8 translation,
    rotation and scale formats (0: three f32; 1: three f32 of a unit quaternion's x y z, w >= 0 derived),
    TArray<i32> track offsets (per track: translation offset, key count, rotation offset, key count), the scale
    offsets (TArray<i32>, i32), TArray<i32> track -> skeleton bone, some other fields, then TArray<u8> the byte
    stream the offsets point into. With variable keys a track of more than one key stores, after its keys, the frame
    of each key (u8, or u16 from 256 frames on), padded to 4 bytes. Between keys the value is interpolated linearly
    (rotations: normalised linear interpolation).
Only the formats Tekken 7's tiger uses are read (variable keys, f32 translations, 96-bit rotations).

Written for this project from the files themselves (no third-party code).
"""
import struct

import numpy as np

from .. import InputError
from .uasset import read_object


def _locate_stream(d, o, need):
    """The byte stream: the first TArray<u8> after o whose length covers what the offsets need."""
    for q in range(o, min(len(d) - 4, o + 4096)):
        n, = struct.unpack_from('<I', d, q)
        if need <= n <= need + 4096 and q + 4 + n <= len(d):
            return q + 4, n
    raise InputError('animation: its compressed data was not found')


def decode(pkg):
    """-> {'frames': n, 'length': seconds, 'tracks': {skeleton bone: (translations (n, 3), rotations (n, 4) x y z w)}}
    (bones without a track are not in 'tracks')."""
    export = pkg.exports[0]
    props, r = read_object(pkg, export)
    d, o = r.data, r.pos
    frames = int(props['NumFrames'])
    bones = [t['BoneTreeIndex'] for t in props.get('TrackToSkeletonMapTable', [])]
    o += 16 + 2 + 4
    key_format, tformat, rformat, _ = d[o:o + 4]
    o += 4
    if key_format != 1 or tformat != 0 or rformat != 1:
        raise InputError('animation %s: compression %d/%d/%d is not read yet' % (export.name, key_format, tformat,
                                                                               rformat))
    n, = struct.unpack_from('<i', d, o)
    offs = struct.unpack_from('<%di' % n, d, o + 4)
    o += 4 + 4 * n
    table = 1 if frames < 256 else 2

    def size(keys):
        return 12 * keys + ((keys * table + 3) & ~3 if keys > 1 else 0)
    need = max(offs[k] + size(offs[k + 1]) for k in range(0, n, 2))
    start, _ = _locate_stream(d, o, need)

    def track(off, keys):
        vals = np.frombuffer(d, '<f4', 3 * keys, start + off).reshape(keys, 3).astype(np.float64)
        if keys == 1:
            return np.repeat(vals, frames, 0)
        at = np.frombuffer(d, np.uint8 if table == 1 else '<u2', keys, start + off + 12 * keys).astype(np.float64)
        out = np.empty((frames, 3))
        for c in range(3):
            out[:, c] = np.interp(np.arange(frames), at, vals[:, c])
        return out

    tracks = {}
    for t, bone in enumerate(bones):
        to, tk, ro, rk = offs[4 * t:4 * t + 4]
        pos = track(to, tk)
        xyz = track(ro, rk)
        w = np.sqrt(np.clip(1.0 - (xyz ** 2).sum(1), 0.0, 1.0))
        q = np.concatenate([xyz, w[:, None]], 1)
        q /= np.linalg.norm(q, axis=1, keepdims=True)
        tracks[bone] = (pos, q)
    return {'frames': frames, 'length': float(props.get('SequenceLength', frames / 60.0)), 'tracks': tracks}
