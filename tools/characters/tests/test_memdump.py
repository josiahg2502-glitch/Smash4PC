"""Unit tests of the Tekken 7 memory dump (tekken7/memdump.py) on a made-up moveset in a made-up memory.

    python3 -m unittest discover tools/characters/tests
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.movesets import T7, TABLE_ORDER         # noqa: E402
from ttt2char.tekken7 import memdump, tkmvst          # noqa: E402
from ttt2char.ttt2 import animation                   # noqa: E402

import numpy as np                                    # noqa: E402


class Memory:
    """Sparse guest memory: bytes placed at addresses."""

    def __init__(self):
        self.parts = {}

    def put(self, address, data):
        self.parts[address] = bytes(data)

    def read(self, address, size):
        for a, d in self.parts.items():
            if a <= address < a + len(d):
                return d[address - a:address - a + size]
        return b''


def moveset_in_memory(mem, base, anim_at):
    """A moveset with one row in every table, links between them, two moves on one animation each."""
    names = b'[TEST]\0MAKER\0DATE\0FULL DATE\0move_a\0anim_a\0move_b\0anim_b\0'
    name_at = {n: base + memdump.NAMES_AT + names.index(n.encode() + b'\0')
               for n in ('[TEST]', 'MAKER', 'DATE', 'FULL DATE', 'move_a', 'anim_a', 'move_b', 'anim_b')}
    first = base + memdump.NAMES_AT + len(names) + 4                    # (the tables need no alignment)
    counts = {t: 1 for t in TABLE_ORDER}
    counts['moves'] = 2
    where, at = {}, first
    for t in TABLE_ORDER:
        where[t] = at
        at += T7[t][0] * counts[t]
    tables = bytearray(at - first)
    # links: every 'Q' field with a table target points at that table's row 0 (moves: the second row for 'move')
    for t in TABLE_ORDER:
        size, fields = T7[t]
        for i in range(counts[t]):
            o = where[t] - first + i * size
            for name, off, fmt, target in fields:
                if fmt != 'Q' or target is None:
                    continue
                if target == 'names':
                    v = name_at['move_a' if name == 'name' else 'anim_a'] if i == 0 else \
                        name_at['move_b' if name == 'name' else 'anim_b']
                elif target == 'anim':
                    v = anim_at[i]
                elif name in ('cancel2', 'voiceclip'):
                    v = 0                                                 # no link
                else:
                    v = where[target]
                struct.pack_into('<Q', tables, o + off, v)
    header = bytearray(memdump.NAMES_AT)
    struct.pack_into('<II', header, 0, 0x10000, 0x444948)
    struct.pack_into('<4Q', header, 8, name_at['[TEST]'], name_at['MAKER'], name_at['DATE'], name_at['FULL DATE'])
    struct.pack_into('<56H', header, 0x28, *range(100, 156))
    for k, t in enumerate(TABLE_ORDER):
        struct.pack_into('<QQ', header, memdump.TABLES_AT + 16 * k, where[t], counts[t])
    mem.put(base, bytes(header) + names + bytes(first - base - memdump.NAMES_AT - len(names)) + bytes(tables))
    return where, counts


def movelist_in_memory():
    """An MVLT as the game keeps it: entries' string offsets from the entry, demos' first input from the demo."""
    strings = b'\0name\0'
    entries_at = 0x20 + len(strings) + 2
    demos_at = entries_at + memdump.MOVELIST_ENTRY
    inputs_at = demos_at + 2 * memdump.MOVELIST_DEMO
    m = bytearray(inputs_at + 4 * 5)
    struct.pack_into('<4sHHIIIIII', m, 0, b'MVLT', 1000, 2, 1, 2, 0x20, entries_at, demos_at, inputs_at)
    m[0x20:0x20 + len(strings)] = strings
    for k in range(memdump.MOVELIST_STRINGS):
        struct.pack_into('<i', m, entries_at + 4 * k, 0x21 - entries_at)              # "name", from the entry
    for k, (first, count) in enumerate(((0, 2), (2, 3))):
        at = demos_at + memdump.MOVELIST_DEMO * k
        struct.pack_into('<HI', m, at + 14, count, inputs_at + 4 * first - at)
    for i in range(5):
        struct.pack_into('<I', m, inputs_at + 4 * i, 0x100 + i)
    return bytes(m), entries_at, demos_at


class MemDump(unittest.TestCase):
    def test_moveset_and_file(self):
        mem = Memory()
        raw = animation.encode_raw([1, 2], np.zeros((3, 2, 3), np.float32), '<')
        mem.put(0x5000, raw)
        mem.put(0x6000, raw + b'\x55' * 8)                                 # what follows it is not read
        where, counts = moveset_in_memory(mem, 0x10000, [0x5000, 0x6000])
        mvl, entries_at, demos_at = movelist_in_memory()
        mem.put(0x80000, mvl)
        self.assertTrue(memdump.looks_like_header(mem.read(0x10000, memdump.NAMES_AT)))
        blocks = memdump.read_moveset(mem.read, 0x10000, 0x80000)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'T7_Test.tkmvst')
            memdump.write_tkmvst(path, blocks, 77, 'Test')
            m = tkmvst.read(path)
            self.assertEqual(os.listdir(tmp), ['T7_Test.tkmvst'])          # (written as .part, then put in place)
        self.assertEqual((m.character_id, m.character_name), (77, 'Test'))
        self.assertEqual(m.aliases, list(range(100, 156)))
        self.assertEqual({t: len(m.tables[t]) for t in TABLE_ORDER}, counts)
        mv = m.tables['moves']
        name = lambda off: m.namebuf[off:m.namebuf.index(b'\0', off)]
        self.assertEqual([name(mv[0]['name']), name(mv[1]['anim_name'])], [b'move_a', b'anim_b'])
        self.assertEqual(mv[0]['cancel'], 0)                               # a link: the row index
        self.assertIsNone(mv[0]['cancel2'])                                # no link: none
        self.assertEqual(m.tables['cancels'][0]['req'], 0)
        self.assertEqual(m.animbuf[mv[1]['anim']:mv[1]['anim'] + len(raw)], raw)
        self.assertEqual(len(m.animbuf), 2 * len(raw))                     # each animation its own size
        # the command list as its file has it
        self.assertEqual(struct.unpack_from('<I', m.movelist, entries_at)[0], 0x21)
        self.assertEqual([struct.unpack_from('<I', m.movelist, demos_at + memdump.MOVELIST_DEMO * k + 16)[0]
                          for k in range(2)], [0, 2])
        self.assertEqual(len(m.movelist), len(mvl))

    def test_not_a_header(self):
        self.assertFalse(memdump.looks_like_header(bytes(memdump.NAMES_AT)))


if __name__ == '__main__':
    unittest.main()
