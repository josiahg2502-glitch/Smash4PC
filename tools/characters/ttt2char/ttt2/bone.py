"""TTT2 skeletons: BONE (".bon", magic "BONE", little-endian), read and written byte for byte.

After a 0x40-byte header (magic, u32 1, u32 100, 4 bytes, u32 bone count, f32 scale, seven u32 section offsets,
12 bytes) the sections follow back to back: parents (N s32) | local translations (N x 3 f32) | local rotations as
Euler angles in radians (N x 3 f32, R = Rz Ry Rx) | the OPT (spring) bone list (u32 count + count u32) | 8 f32
parameters (absent in some files: offset 0) | names (N x char[32]) | scales (N x 3 f32).
"""
import struct

import numpy as np


def read(d):
    if d[:4] != b'BONE':
        raise ValueError('not a BONE skeleton')
    v1, v2 = struct.unpack_from('<II', d, 4)
    b0c = d[0x0C:0x10]
    n, = struct.unpack_from('<I', d, 0x10)
    scale, = struct.unpack_from('<f', d, 0x14)
    o = struct.unpack_from('<7I', d, 0x18)
    cnt, = struct.unpack_from('<I', d, o[3])
    s = dict(v1=v1, v2=v2, b0c=b0c, scale=scale, hdr_tail=d[0x34:0x40],
             parent=list(struct.unpack_from('<%di' % n, d, o[0])),
             t=np.frombuffer(d, '<f4', 3 * n, o[1]).reshape(n, 3).copy(),
             r=np.frombuffer(d, '<f4', 3 * n, o[2]).reshape(n, 3).copy(),
             opt=list(struct.unpack_from('<%dI' % cnt, d, o[3] + 4)),
             params=list(struct.unpack_from('<8f', d, o[4])) if o[4] else None,   # absent in *_effpos.bon
             names_raw=[d[o[5] + 32 * i:o[5] + 32 * i + 32] for i in range(n)],
             s=np.frombuffer(d, '<f4', 3 * n, o[6]).reshape(n, 3).copy(),
             tail=d[o[6] + 12 * n:])
    s['names'] = [x.split(b'\0')[0].decode('latin1') for x in s['names_raw']]
    return s


def write(s):
    n = len(s['parent'])
    names_raw = s.get('names_raw')
    if names_raw is None or len(names_raw) != n or [x.split(b'\0')[0].decode('latin1') for x in names_raw] != s['names']:
        names_raw = [nm.encode('latin1')[:31].ljust(32, b'\0') for nm in s['names']]
    secs = [struct.pack('<%di' % n, *s['parent']),
            np.asarray(s['t'], '<f4').reshape(n, 3).tobytes(),
            np.asarray(s['r'], '<f4').reshape(n, 3).tobytes(),
            struct.pack('<I%dI' % len(s['opt']), len(s['opt']), *s['opt']),
            struct.pack('<8f', *s['params']) if s['params'] is not None else None,
            b''.join(names_raw),
            np.asarray(s['s'], '<f4').reshape(n, 3).tobytes()]
    offs, p = [], 0x40
    for x in secs:
        offs.append(p if x is not None else 0)
        p += len(x) if x is not None else 0
    secs = [x for x in secs if x is not None]
    hdr = b'BONE' + struct.pack('<II', s.get('v1', 1), s.get('v2', 100)) + bytes(s.get('b0c', b'\x00\x03\x00\x06'))
    hdr += struct.pack('<If7I', n, s['scale'], *offs) + bytes(s.get('hdr_tail', bytes(12)))
    return hdr + b''.join(secs) + s.get('tail', b'')


def euler_xyz_from_matrix(R):
    """Inverse of R = Rz(z) Ry(y) Rx(x) (the BONE convention)."""
    sy = -R[2, 0]
    y = np.arcsin(np.clip(sy, -1, 1))
    if abs(sy) < 0.999999:
        x = np.arctan2(R[2, 1], R[2, 2])
        z = np.arctan2(R[1, 0], R[0, 0])
    else:                                   # gimbal lock: put everything in x
        z = 0.0
        x = np.arctan2(-R[1, 2], R[1, 1])
    return np.array([x, y, z])


def matrix_from_euler_xyz(e):
    x, y, z = e
    cx, sx, cy, sy, cz, sz = np.cos(x), np.sin(x), np.cos(y), np.sin(y), np.cos(z), np.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def local_matrix(s, i):
    m = np.eye(4)
    m[:3, :3] = matrix_from_euler_xyz(np.asarray(s['r'][i], np.float64))
    m[:3, 3] = s['t'][i]
    return m


def world_matrices(s):
    n = len(s['parent'])
    w = [None] * n
    for i in range(n):
        p = s['parent'][i]
        assert p < i, 'parent after child'
        w[i] = local_matrix(s, i) if p < 0 else w[p] @ local_matrix(s, i)
    return np.array(w)
