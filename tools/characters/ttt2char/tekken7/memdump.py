"""Tekken 7 movesets read from the running game's memory, written as the importer's .tkmvst files (tkmvst.py).

Tekken 7 keeps a fighter's moveset in memory, decoded, while the fighter is loaded (its files stay encrypted on
disk). In memory a moveset is one buffer:
    0x000  header: ..., 4 x u64 string pointers @0x08 (name, creator, date, full date), u16 aliases[56] @0x28,
           u16 current aliases[56] @0x98, ... (0x150 bytes: the .tkmvst info block)
    0x150  19 x {u64 pointer, u64 count}: the tables (movesets.TABLE_ORDER), back to back after the strings
    0x280  13 x u64: the fighter's motion files (MOTA; the importer does not use them)
    0x2E8  the strings (names of moves and animations), up to the first table
Every link is a 64-bit address (0: none). Animations live elsewhere, each referenced by moves' anim pointers; the
practice mode's command list (MVLT) is a separate buffer.

The .tkmvst form (TKMovesets2's, the importer's input; checked field by field against its files): the blocks
info, table, motion files, names, moveset (the 19 tables), animations, motion data, command list; a table link is the
row index in its table (none: all ones), a name an offset into the names block, an animation an offset into the
animation block, a header string an offset from the header, a table's place an offset into the moveset block.

Written for this project from the files and the game's memory (no third-party code).
"""
import os
import struct
import time
import zlib

from ..movesets import T7, TABLE_ORDER
from ..ttt2 import animation

HEADER_SIZE, TABLES_AT, MOTAS_AT, NAMES_AT = 0x150, 0x150, 0x280, 0x2E8
STRINGS = 4                         # header string pointers at 0x08
NONE = 0xFFFFFFFFFFFFFFFF
FILE_HEADER = 0x100                 # .tkmvst: header, then the block list
BLOCKS = 8


def _tables(header):
    return {t: struct.unpack_from('<QQ', header, TABLES_AT + 16 * k) for k, t in enumerate(TABLE_ORDER)}


def looks_like_header(header):
    """Whether bytes (at least NAMES_AT of them) are a moveset's header: tables in order, ascending, past the
    strings."""
    if len(header) < NAMES_AT:
        return False
    tables = [struct.unpack_from('<QQ', header, TABLES_AT + 16 * k) for k in range(len(TABLE_ORDER))]
    previous = 0
    for (p, n), t in zip(tables, TABLE_ORDER):
        if p == 0 or p < previous or n > 1 << 20:
            return False
        previous = p + n * T7[t][0]
    strings = struct.unpack_from('<4Q', header, 8)
    return all(0 < s < tables[0][0] for s in strings)


def read_animation(read, address):
    """The bytes of the animation at `address` (its size from its header)."""
    want = 0x10000
    while True:
        buf = read(address, want)
        try:
            size = animation.header(buf, '<')['size']
        except (struct.error, IndexError, ValueError):
            size = None                     # the header reaches past what was read
        if size is not None and size <= len(buf):
            return buf[:size]
        if len(buf) < want:                 # unreadable past this point
            raise ValueError('animation at %#x cannot be read' % address)
        want = max(want * 2, size or 0)


MOVELIST_ENTRY, MOVELIST_STRINGS, MOVELIST_DEMO = 372, 16, 24


def read_movelist(read, address):
    """The practice mode's command list (MVLT) at `address`, as its file has it: once loaded, the game keeps an
    entry's string offsets from the entry (the file: from the list's start) and a demo's first input as an offset
    from the demo (the file: an index into the inputs)."""
    head = read(address, 0x40)
    _, _, _, entries, ndemos, _, entries_at, demos_at, inputs_at = struct.unpack_from('<4sHHIIIIII', head, 0)
    m = bytearray(read(address, demos_at + MOVELIST_DEMO * ndemos))
    for e in range(entries):
        at = entries_at + MOVELIST_ENTRY * e
        for k in range(MOVELIST_STRINGS):
            v, = struct.unpack_from('<i', m, at + 4 * k)
            struct.pack_into('<I', m, at + 4 * k, (at + v) & 0xFFFFFFFF)
    end = 0
    for k in range(ndemos):
        at = demos_at + MOVELIST_DEMO * k
        count, = struct.unpack_from('<H', m, at + 14)
        relative, = struct.unpack_from('<i', m, at + 16)
        first = (at + relative - inputs_at) // 4
        struct.pack_into('<I', m, at + 16, first)
        end = max(end, first + count)
    return bytes(m) + read(address + len(m), inputs_at + 4 * end - len(m))


def read_moveset(read, address, movelist=None):
    """A moveset at `address` in the game's memory; read(address, size) -> bytes. -> dict of the .tkmvst blocks."""
    header = bytearray(read(address, NAMES_AT))
    if not looks_like_header(header):
        raise ValueError('no moveset at %#x' % address)
    tables = _tables(header)
    first = tables[TABLE_ORDER[0]][0]
    names_at = address + NAMES_AT
    names = read(names_at, first - names_at)
    end = max(p + n * T7[t][0] for t, (p, n) in tables.items())
    data = bytearray(read(first, end - first))
    if len(data) != end - first:
        raise ValueError('the moveset at %#x is not all readable' % address)

    # animations, in the order the moves first use them
    anims, anim_offsets = bytearray(), {}
    move_ptr, move_count = tables['moves']
    size = T7['moves'][0]
    for i in range(move_count):
        a, = struct.unpack_from('<Q', data, move_ptr - first + i * size + 0x10)
        if a and a not in anim_offsets:
            anim_offsets[a] = len(anims)
            anims += read_animation(read, a)

    def link(v, target):
        if v == 0:
            return NONE
        if target == 'names':
            return v - names_at
        if target == 'anim':
            return anim_offsets[v]
        p, _ = tables[target]
        return (v - p) // T7[target][0]

    for t in TABLE_ORDER:
        p, n = tables[t]
        size, fields = T7[t]
        for i in range(n):
            o = p - first + i * size
            for name, off, fmt, target in fields:
                if fmt == 'Q' and target is not None:
                    v, = struct.unpack_from('<Q', data, o + off)
                    struct.pack_into('<Q', data, o + off, link(v, target))

    table_block = b''.join(struct.pack('<QQ', p - first, n) for p, n in (tables[t] for t in TABLE_ORDER))
    for k in range(STRINGS):
        s, = struct.unpack_from('<Q', header, 8 + 8 * k)
        struct.pack_into('<Q', header, 8 + 8 * k, s - address)
    return dict(info=bytes(header[:HEADER_SIZE]), table=table_block, motalists=bytes(NAMES_AT - MOTAS_AT),
                name=bytes(names), moveset=bytes(data), animation=bytes(anims), mota=b'',
                movelist=read_movelist(read, movelist) if movelist else b'')


def write_tkmvst(path, blocks, character_id, name):
    """Writes the blocks as an uncompressed .tkmvst (the layout tkmvst.read takes)."""
    order = ['info', 'table', 'motalists', 'name', 'moveset', 'animation', 'mota', 'movelist']
    data, offsets = bytearray(), []
    for b in order:
        while len(data) % 8:
            data.append(0)
        offsets.append(len(data))
        data += blocks[b]
    block_list = FILE_HEADER + 8
    start = block_list + 8 * BLOCKS
    head = bytearray(start)
    head[0:4] = b'TKM2'
    head[4:7] = b'1.0'
    now = int(time.time())
    struct.pack_into('<IIII', head, 0x44, FILE_HEADER, block_list, BLOCKS, start)
    struct.pack_into('<Q', head, 0x58, len(data))
    crc = zlib.crc32(data) & 0xFFFFFFFF
    struct.pack_into('<II', head, 0x64, crc, crc)
    struct.pack_into('<QQ', head, 0x70, now, now)
    struct.pack_into('<I', head, 0x80, character_id)
    for at, text in ((0x90, 'Tekken 7'), (0xB0, name), (0xD0, name)):
        head[at:at + 31] = text.encode('latin1')[:31].ljust(31, b'\0')
    struct.pack_into('<8Q', head, block_list, *offsets)
    with open(path + '.part', 'wb') as f:       # (a dump stopped halfway leaves no moveset that looks saved)
        f.write(head)
        f.write(data)
    os.replace(path + '.part', path)
