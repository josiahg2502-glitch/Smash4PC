"""Tekken 7 PC's Unreal Engine 4 pak files (version 3), read-only.

    file tail (last 44 bytes): u32 magic 0x5A6F12E1, u32 version (3), i64 index offset, i64 index size, 20-byte SHA-1
    index: FString mount point ("../../../"), i32 entry count, then per entry: FString path (relative to the mount
           point) and an entry record
    entry record: i64 offset, i64 stored size, i64 size, i32 compression (0 none, 1 zlib), 20-byte SHA-1,
           [compression != 0: i32 block count, {i64 start, i64 end} per block], u8 encrypted flag, u32 block size

At `offset` every entry's data is preceded by a copy of its record (with offset 0): 53 bytes for an uncompressed
entry. Tekken 7 sets the encrypted flag on every entry although the data is plain, so the flag is ignored; no entry
is compressed (zlib blocks are read anyway, block offsets being absolute in version 3). A path in a `*_P.pak` patch
pak replaces the same path in the base paks.

A few fighters' files (Geese Howard's and Noctis's costumes, Noctis's voice bank) are stored encrypted by Tekken 7
itself (a package then does not start with the package tag, a sound bank not with BKHD). Those are read from a folder
of the files as the game reads them (tekken7/autodump.py saves them from the running game), under their pak paths.
"""
import os
import struct
import zlib

PAK_MAGIC = 0x5A6F12E1
ENTRY_HEADER_SIZE = 53
GAME_PREFIX = 'TekkenGame/Content/'
# how each kind of file starts when it is plain (the others are taken as plain)
PLAIN_START = {'.uasset': b'\xc1\x83\x2a\x9e', '.umap': b'\xc1\x83\x2a\x9e', '.bnk': b'BKHD'}


class EncryptedError(Exception):
    """A file Tekken 7 keeps encrypted, not saved from the running game yet (the message is its pak path)."""


def plain(path, data):
    """Whether a file's data (at least its first 4 bytes) is plain."""
    start = PLAIN_START.get(os.path.splitext(path)[1].lower())
    return start is None or data[:4] == start


def _fstring(b, o):
    """FString: i32 length (negative: UTF-16 code units), the characters including a NUL."""
    n, = struct.unpack_from('<i', b, o)
    o += 4
    if n == 0:
        return '', o
    if n < 0:
        return b[o:o - 2 * n].decode('utf-16le').rstrip('\0'), o - 2 * n
    return b[o:o + n].decode('latin1').rstrip('\0'), o + n


class Entry:
    __slots__ = ('pak', 'path', 'offset', 'size', 'usize', 'comp', 'blocks')

    def __init__(self, pak, path, offset, size, usize, comp, blocks):
        self.pak, self.path, self.offset, self.size, self.usize, self.comp, self.blocks = \
            pak, path, offset, size, usize, comp, blocks


def read_index(path):
    with open(path, 'rb') as f:
        f.seek(-44, 2)
        magic, ver, off, size = struct.unpack('<IIqq', f.read(24))
        if magic != PAK_MAGIC:
            raise ValueError('%s is not a pak file' % path)
        if ver != 3:
            raise ValueError('%s: pak version %d (Tekken 7 has version 3)' % (path, ver))
        f.seek(off)
        b = f.read(size)
    mount, o = _fstring(b, 0)
    mount = mount.replace('../../../', '')
    n, = struct.unpack_from('<i', b, o)
    o += 4
    out = []
    for _ in range(n):
        name, o = _fstring(b, o)
        eoff, esize, eusize, comp = struct.unpack_from('<qqqi', b, o)
        o += 28 + 20
        blocks = []
        if comp:
            nb, = struct.unpack_from('<i', b, o)
            blocks = [struct.unpack_from('<qq', b, o + 4 + 16 * k) for k in range(nb)]
            o += 4 + 16 * nb
        o += 5                                      # encrypted flag, block size
        out.append(Entry(path, mount + name, eoff, esize, eusize, comp, blocks))
    return out


def pak_names(pak_dir):
    """The pak files' names in a folder (none when there is no such folder), the base paks first, then the patches.
    (A listing, not a glob: a Steam library's path may hold [ ] * ?.)"""
    try:
        names = sorted(n for n in os.listdir(pak_dir) if n.endswith('.pak'))
    except OSError:
        return []
    return sorted(names, key=lambda n: (n.endswith('_P.pak'), n))


class Paks:
    """Every pak of the game: path -> the entry that counts (patch paks win). Lookups take pak paths
    ('TekkenGame/Content/...uasset') or object paths ('/Game/...'), in any case."""

    def __init__(self, pak_dir, saved=None):
        """saved: the folder of the encrypted files as the game reads them (pak paths below it), if any."""
        self.saved = saved
        names = pak_names(pak_dir)
        if not names:
            raise FileNotFoundError(pak_dir)
        self.index = {}
        for n in names:
            for e in read_index(os.path.join(pak_dir, n)):
                self.index[e.path.lower()] = e
        self._files = {}

    @staticmethod
    def object_to_file(objpath, ext='.uasset'):
        """'/Game/Character/X/Y.Y' -> 'TekkenGame/Content/Character/X/Y.uasset'."""
        p = objpath.split('.')[0]
        if p.startswith('/Game/'):
            p = GAME_PREFIX + p[len('/Game/'):]
        elif p.startswith('/Engine/'):
            p = 'Engine/Content/' + p[len('/Engine/'):]
        return p + ext

    def find(self, path):
        k = path.lower()
        return self.index.get(k) or self.index.get(self.object_to_file(path).lower())

    def stored(self, e, n=None):
        """An entry's data as the pak stores it (the first n bytes, or all)."""
        f = self._files.get(e.pak)
        if f is None:
            f = self._files[e.pak] = open(e.pak, 'rb')
        if e.comp == 0:
            n = e.size if n is None else min(n, e.size)
            f.seek(e.offset + ENTRY_HEADER_SIZE)
            d = f.read(n)
            if len(d) != n:
                raise IOError('%s: short read of %s' % (e.pak, e.path))
            return d
        if e.comp != 1:
            raise NotImplementedError('%s: pak compression %d' % (e.path, e.comp))
        parts = []
        for start, end in e.blocks:
            f.seek(start)
            parts.append(zlib.decompress(f.read(end - start)))
        return b''.join(parts)[:e.usize if n is None else min(n, e.usize)]

    def encrypted(self, e):
        """Whether Tekken 7 keeps an entry encrypted."""
        return not plain(e.path, self.stored(e, 4))

    def saved_path(self, e):
        return os.path.join(self.saved, *e.path.split('/')) if self.saved else None

    def read(self, path):
        e = self.find(path)
        if e is None:
            raise KeyError(path)
        d = self.stored(e)
        if plain(e.path, d):
            return d
        saved = self.saved_path(e)
        if saved and os.path.isfile(saved):
            with open(saved, 'rb') as f:
                d = f.read()
            if plain(e.path, d):
                return d
        raise EncryptedError(e.path)

    def close(self):
        for f in self._files.values():
            f.close()
        self._files = {}
