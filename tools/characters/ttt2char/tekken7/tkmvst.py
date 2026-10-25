"""Tekken 7 movesets saved by TKMovesets2 (".tkmvst"), read into the common moveset form (movesets.py).

    0x000 "TKM2", char version[28], u8 game version hash[32], u16 game id @0x40, u16 minor @0x42,
          u32 header size @0x44, u32 block list @0x48, u32 block count @0x4C, u32 data start @0x50,
          u64 data size @0x58 (compressed files: the size of the data once decompressed), u8 compression @0x60
          (0 none, 1 LZ4, 2 LZMA), u32 crc32 @0x64, u32 original crc32 @0x68, u64 dates @0x70 and 0x78,
          u32 character id @0x80 (the game's own roster id), u64 flags @0x88, char origin[32] @0x90,
          target character[32] @0xB0, original character name[32] @0xD0.
    Everything from the data start on may be compressed as a whole: one LZ4 block, or an .xz stream.
    The block list: 8 u64 offsets from the data start: moveset info, table, MOTA list, names, moveset (the 19 tables
    back to back), animations, MOTAs, movelist.
    Moveset info: ..., u16 aliases[56] @0x28, u16 current aliases[56] @0x98, u16 unknown[36] @0x108.
    Table: 19 x {u64 offset into the moveset block, u64 count}. A move's anim is an offset into the animation block.
"""
import lzma
import os
import struct

from .. import InputError
from ..lz4 import decompress_block
from ..movesets import T7, TABLE_ORDER, Moveset, read_table

BLOCKS = ['info', 'table', 'motalists', 'name', 'moveset', 'animation', 'mota', 'movelist']


def decompress(d):
    """The file with its data decompressed (as TKMovesets2 writes it without compression)."""
    if d[:4] != b'TKM2':
        raise InputError('not a TKMovesets2 moveset (.tkmvst)')
    start, = struct.unpack_from('<I', d, 0x50)
    size, = struct.unpack_from('<Q', d, 0x58)
    method = d[0x60]
    if method == 0:
        return d
    if method == 1:
        data = decompress_block(d[start:], size)
    elif method == 2:
        data = lzma.decompress(d[start:])
    else:
        raise InputError('this moveset uses compression %d, which the importer does not know' % method)
    if len(data) != size:
        raise InputError('the moveset decompresses to %d bytes instead of %d' % (len(data), size))
    return d[:start] + data


def read(path):
    with open(path, 'rb') as f:
        d = decompress(f.read())
    m = Moveset()
    m.game = 't7'
    m.path = path
    m.character_id, = struct.unpack_from('<I', d, 0x80)
    m.character_name = d[0xD0:0xF0].split(b'\0')[0].decode('latin1')
    start, = struct.unpack_from('<I', d, 0x50)
    list_at, = struct.unpack_from('<I', d, 0x48)
    ab = [start + b for b in struct.unpack_from('<8Q', d, list_at)] + [len(d)]
    m.blocks = dict(zip(BLOCKS, ab))
    info = ab[0]
    m.aliases = list(struct.unpack_from('<56H', d, info + 0x28))
    m.namebuf = d[ab[3]:ab[4]]
    m.tables = {}
    for k, t in enumerate(TABLE_ORDER):
        off, cnt = struct.unpack_from('<QQ', d, ab[1] + 16 * k)
        m.tables[t] = read_table(d, ab[4] + off, cnt, T7[t], '<')
    # animations: offsets into the animation block; an animation ends where the next one starts
    m.animbuf = d[ab[5]:ab[6]]
    offs = sorted(set(mv['anim'] for mv in m.tables['moves']))
    m.anim_offsets = offs
    m.anim_size = dict(zip(offs, [b - a for a, b in zip(offs, offs[1:] + [len(m.animbuf)])]))
    m.movelist = d[ab[7]:ab[8]]           # the displayable command list (MVLT), if TKMovesets2 saved it
    return m


def saved_movesets(folder):
    """{Tekken 7 roster id: path} of the .tkmvst files in `folder` (none if it does not exist), from their headers."""
    found = {}
    names = sorted(n for n in os.listdir(folder) if n.endswith('.tkmvst')) if folder and os.path.isdir(folder) else []
    for path in (os.path.join(folder, n) for n in names):     # (a listing: the folder's path may hold [ ] * ?)
        with open(path, 'rb') as f:
            head = f.read(0xF0)
        if len(head) < 0xF0 or head[:4] != b'TKM2' or not head[0x90:0xB0].startswith(b'Tekken 7'):
            continue
        found.setdefault(struct.unpack_from('<I', head, 0x80)[0], path)
    return found
