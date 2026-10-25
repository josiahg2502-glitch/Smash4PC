"""TTT2 spring-bone parameters (".mop", big-endian, no magic), read and written byte for byte.

f32 version (5.0) + 28 zero bytes | u32 record count at 0x20 + 12 zero bytes | records of 0x60 bytes from 0x30, each
starting with the u32 id of the OPT bone it drives (the rest kept as it is) | a variable tail (kept as it is).
Added fighters use their donor's file; this is here for inspection and tests.
"""
import struct


def read(d):
    ver, = struct.unpack_from('>f', d, 0)
    cnt, = struct.unpack_from('>I', d, 0x20)
    recs = [d[0x30 + 0x60 * k:0x30 + 0x60 * (k + 1)] for k in range(cnt)]
    return dict(version=ver, head=d[4:0x20], mid=d[0x24:0x30], records=recs,
                opt_ids=[struct.unpack_from('>I', r, 0)[0] for r in recs], tail=d[0x30 + 0x60 * cnt:])


def write(m):
    out = struct.pack('>f', m.get('version', 5.0)) + bytes(m.get('head', bytes(28)))
    out += struct.pack('>I', len(m['records'])) + bytes(m.get('mid', bytes(12)))
    for r in m['records']:
        if len(r) != 0x60:
            raise ValueError('a .mop record is 0x60 bytes')
        out += r
    return out + m.get('tail', b'')
