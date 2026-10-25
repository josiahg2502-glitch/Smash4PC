"""The Wii U game's data archives (`content/hdd/dataNNN.ofs` + `.bin`), read-only.

An index (`.ofs`) is a list of chunks {u32 size, tag, payload} (big-endian); chunk `ofsi` holds one {u32 key, u32 word}
per entry and `ofsd` one u32 offset per entry plus the end, into the `.bin`'s payload at 0x100. Entries are 16-byte
aligned; the low four bits of an offset give the entry's length modulo 16 (0: a multiple of 16).

Every entry is scrambled: XOR with four 32-bit linear congruential streams, one per big-endian word of each 16-byte
block, seeded from the entry's key (x = x * 0xBDE95 + 0x8EF345, four times) and stepped once per block
(w = w * 1531 + 5011). Most entries are then an `NRCZ` record: "NRCZ", u32 size, 8 bytes of flags, a zlib stream.

Keys: an entry's key is the CRC-32 of its name with the register starting at zero (`zlib.crc32(name, 0xFFFFFFFF)`).
Members of a pack (a fighter's costume models, textures, ...) continue that CRC over the member index's base-4
digits, least significant first, one byte each. The title loads data000-data055 and then the update's data100; for
a key held twice the last one loaded wins, as here.
"""
import os
import re
import struct
import zlib

import numpy as np

PAYLOAD = 0x100
_SEED_MUL, _SEED_ADD, _STEP_MUL, _STEP_ADD = 0x000BDE95, 0x008EF345, 1531, 5011


def key_of(name):
    return zlib.crc32(name.encode('latin1'), 0xFFFFFFFF)


def member_key(pack, index):
    digits = bytearray()
    while index:
        digits.append(index & 3)
        index >>= 2
    return zlib.crc32(bytes(digits), pack) if digits else pack


def _keystream_coefficients(blocks):
    # The state after k steps is a_k * s_0 + b_k (mod 2^32) with a_k = MUL^k and b_k = ADD * (1 + MUL + ... MUL^(k-1)).
    with np.errstate(over='ignore'):
        a = np.empty(blocks, np.uint64)
        a[0] = 1
        a[1:] = np.cumprod(np.full(blocks - 1, _STEP_MUL, np.uint64))
        b = np.zeros(blocks, np.uint64)
        b[1:] = np.cumsum(a[:-1]) * np.uint64(_STEP_ADD)
    return a & np.uint64(0xFFFFFFFF), b & np.uint64(0xFFFFFFFF)


def scramble(data, key):
    """XOR with the entry's key stream; the same call scrambles and unscrambles."""
    if not data:
        return b''
    blocks = (len(data) + 15) // 16
    a, b = _keystream_coefficients(blocks)
    seeds = []
    x = key
    for _ in range(4):
        x = (x * _SEED_MUL + _SEED_ADD) & 0xFFFFFFFF
        seeds.append(x)
    s0 = np.array(seeds, np.uint64)
    with np.errstate(over='ignore'):
        stream = ((a[:, None] * s0[None, :] + b[:, None]) & np.uint64(0xFFFFFFFF)).astype('>u4').tobytes()
    return (np.frombuffer(data, np.uint8) ^ np.frombuffer(stream, np.uint8)[:len(data)]).tobytes()


def unpack(stored, key):
    """An entry's contents from its stored bytes."""
    d = scramble(stored, key)
    if len(d) >= 16 and d[:4] == b'NRCZ':
        size, = struct.unpack_from('>I', d, 4)
        out = zlib.decompressobj().decompress(d[16:], size)
        if len(out) != size:
            raise ValueError('entry %08x: NRCZ stream ends early' % key)
        return out
    return d


def pack(contents, key, flags=bytes(8)):
    """Stored bytes of an NRCZ entry (what the runtime's archive overlay writes)."""
    return scramble(b'NRCZ' + struct.pack('>I', len(contents)) + bytes(flags) + zlib.compress(contents, 1), key)


def _chunks(d):
    out, pos = {}, 0
    while pos + 8 <= len(d):
        size, tag = struct.unpack_from('>I4s', d, pos)
        if size < 8 or pos + size > len(d):
            break
        out.setdefault(tag, (pos + 8, size - 8))
        pos += size
    return out


def parse_index(ofs):
    """[(key, word, offset in the payload, size)] of an index."""
    c = _chunks(ofs)
    if b'ofsi' not in c or b'ofsd' not in c:
        raise ValueError('not an archive index')
    (pi, si), (po, so) = c[b'ofsi'], c[b'ofsd']
    names = [struct.unpack_from('>II', ofs, pi + 8 * i) for i in range(si // 8)]
    offsets = struct.unpack_from('>%dI' % (so // 4), ofs, po)
    if len(offsets) != len(names) + 1:
        raise ValueError('archive index tables disagree')
    out = []
    for i, (key, word) in enumerate(names):
        start, end, tail = offsets[i] & ~0xF, offsets[i + 1] & ~0xF, offsets[i] & 0xF
        size = end - start if tail == 0 else max(end - start, 16) - 16 + tail
        out.append((key, word, start, size))
    return out


class Archives:
    """All archives of an installed game folder (the one holding content/hdd)."""

    def __init__(self, game_dir):
        hdd = os.path.join(game_dir, 'content', 'hdd')
        names = sorted(n[:-4] for n in os.listdir(hdd) if re.fullmatch(r'data\d{3}\.ofs', n)) \
            if os.path.isdir(hdd) else []
        if 'data000' not in names:
            raise FileNotFoundError(hdd)
        self.hdd = hdd
        self.indices = {}
        self.where = {}               # key -> (archive, index); the last archive loaded wins
        for n in names:
            with open(os.path.join(hdd, n + '.ofs'), 'rb') as f:
                self.indices[n] = parse_index(f.read())
            for i, (key, _, _, _) in enumerate(self.indices[n]):
                self.where[key] = (n, i)
        self._files = {}

    def _bin(self, archive):
        f = self._files.get(archive)
        if f is None:
            f = self._files[archive] = open(os.path.join(self.hdd, archive + '.bin'), 'rb')
        return f

    def stored(self, archive, index):
        key, _, start, size = self.indices[archive][index]
        f = self._bin(archive)
        f.seek(PAYLOAD + start)
        return f.read(size)

    def entry(self, archive, index):
        """Contents of entry `index` of `archive` (e.g. 'data006', 42)."""
        return unpack(self.stored(archive, index), self.indices[archive][index][0])

    def has(self, key):
        return key in self.where

    def read(self, key):
        """Contents of the entry with this key."""
        if key not in self.where:
            raise KeyError('the game has no archive entry %08x' % key)
        return self.entry(*self.where[key])

    def read_name(self, name):
        return self.read(key_of(name))

    def raw(self, key):
        """The stored bytes of the entry with this key (the sound archives store their banks as they are)."""
        if key not in self.where:
            raise KeyError('the game has no archive entry %08x' % key)
        return self.stored(*self.where[key])

    def close(self):
        for f in self._files.values():
            f.close()
        self._files = {}
