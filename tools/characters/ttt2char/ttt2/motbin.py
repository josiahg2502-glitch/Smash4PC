"""TTT2 Wii U movesets ("motbin", data006: one per motion row), read into the common moveset form (movesets.py)
and written back byte for byte.

    0x000 u32, "COM\\0", 4 x u32 string offsets (name, creator, date, full date), u16 aliases[56] @0x18,
          u16 current aliases[56] @0x88, u16 unknown[36] @0xF8 (the last four: the character id as
          id-1, -(id-1), id, -id), 19 x {u32 offset, u32 count} @0x140 (the tables), 13 x u32 @0x1D8 (embedded
          MOTAs, 0: none); offsets count from BASE (0x20C)
    BASE: the string block (names), the 19 tables (big-endian, layouts in movesets.TTT2), the embedded MOTAs.
"""
import struct

from ..movesets import TABLE_ORDER, TTT2, Moveset, pack_table, read_table

BASE = 0x20C


def read(d):
    if d[4:8] != b'COM\0':
        raise ValueError('not a TTT2 moveset')
    m = Moveset()
    m.game = 'ttt2'
    m.raw = d
    m.namebuf = d[BASE:]
    m.aliases = list(struct.unpack_from('>56H', d, 0x18))
    m.table_info, m.tables = {}, {}
    for k, t in enumerate(TABLE_ORDER):
        off, cnt = struct.unpack_from('>II', d, 0x140 + 8 * k)
        m.table_info[t] = (off, cnt)
        m.tables[t] = read_table(d, BASE + off, cnt, TTT2[t], '>')
    m.motalist = list(struct.unpack_from('>13I', d, 0x1D8))
    m.name_offsets = struct.unpack_from('>4I', d, 8)
    return m


def split(m):
    """(header, string block, MOTA blob, MOTA slots relative to the blob) of a read moveset."""
    d = m.raw
    first = m.table_info['reactions'][0]
    end = max(p + n * TTT2[t][0] for t, (p, n) in m.table_info.items())
    used = [o for o in m.motalist if o]
    mstart = min(used) if used else end
    return d[:BASE], d[BASE:BASE + first], d[BASE + mstart:], [(o - mstart) if o else None for o in m.motalist]


def build(header, strings, tables, mota_blob, mota_slots, name_offsets=None):
    """A motbin from its pieces: the header's table and MOTA pointers are rewritten (and the four string offsets
    with name_offsets); the tables follow the string block (padded to 4), the MOTAs the tables."""
    hdr = bytearray(header[:BASE])
    body = bytearray(strings)
    while len(body) % 4:
        body.append(0)
    ptrs = []
    for t in TABLE_ORDER:
        ptrs.append((len(body), len(tables[t])))
        body += pack_table(tables[t], TTT2[t])
    mota_base = len(body)
    body += mota_blob
    for k, (p, n) in enumerate(ptrs):
        struct.pack_into('>II', hdr, 0x140 + 8 * k, p, n)
    for k in range(13):
        s = mota_slots[k] if k < len(mota_slots) else None
        struct.pack_into('>I', hdr, 0x1D8 + 4 * k, 0 if s is None else mota_base + s)
    if name_offsets is not None:
        struct.pack_into('>4I', hdr, 8, *name_offsets)
    return bytes(hdr + body)
