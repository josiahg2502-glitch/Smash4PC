"""The title's executable (`code/Tekken.rpx`, an ELF file of the Wii U's RPX kind), read-only: its loadable sections at
their addresses, for reading the title's own tables (keys of pictures, ...).

    ELF header (big-endian, 32-bit): section headers at +32 (u32), count at +48 (u16); a header is 40 bytes {name,
    type, flags, address, offset, size, ...}; a section with flag 0x08000000 holds its data zlib-compressed after a
    u32 of its size; type 8 (NOBITS) has no data.
"""
import os
import struct
import zlib

ZLIB_FLAG = 0x08000000
ALLOC_FLAG = 2


class Rpx:
    def __init__(self, path):
        with open(path, 'rb') as f:
            raw = f.read()
        if raw[:4] != b'\x7fELF':
            raise ValueError('%s is not an RPX file' % path)
        shoff, = struct.unpack_from('>I', raw, 32)
        shnum, = struct.unpack_from('>H', raw, 48)
        self.sections = []
        for i in range(shnum):
            _, kind, flags, addr, off, size = struct.unpack_from('>6I', raw, shoff + 40 * i)
            if not addr or not flags & ALLOC_FLAG:
                continue
            if kind == 8:
                data = bytes(size)
            else:
                data = raw[off:off + size]
                if flags & ZLIB_FLAG:
                    data = zlib.decompress(data[4:])
            self.sections.append((addr, data))

    @classmethod
    def of_game(cls, game_dir):
        return cls(os.path.join(game_dir, 'code', 'Tekken.rpx'))

    def read(self, address, n):
        for addr, data in self.sections:
            if addr <= address and address + n <= addr + len(data):
                return data[address - addr:address - addr + n]
        raise KeyError('%#x is in no section of the executable' % address)

    def u32(self, address):
        return struct.unpack('>I', self.read(address, 4))[0]
