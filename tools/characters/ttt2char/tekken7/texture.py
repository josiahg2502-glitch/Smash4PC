"""Cooked UTexture2D (Unreal Engine 4.14) from Tekken 7.

After the tagged properties and the object GUID flag:
    FStripDataFlags (UTexture), FStripDataFlags (UTexture2D), bool32 cooked (1)
    per platform: FName pixel format ("None" ends the list), i32 offset of the next entry, then
        i32 width, height, slices, FString pixel format, i32 first mip,
        TArray mips: {bool32 cooked, bulk data, i32 width, i32 height}
    bulk data: u32 flags, i32 element count, i32 size on disk, i64 offset; flags & 0x01: the data is at the
        package's bulk data start + offset; & 0x20: no data; otherwise the bytes follow inline.
"""
from .. import bc
from .uasset import ParseError, read_object

BULK_AT_END, BULK_ZLIB, BULK_UNUSED, BULK_SEPARATE_FILE = 0x01, 0x02, 0x20, 0x100


class Mip:
    __slots__ = ('width', 'height', 'data')

    def __init__(self, width, height, data):
        self.width, self.height, self.data = width, height, data


def _bulk(r, pkg):
    flags, count, size_on_disk, offset = r.u32(), r.i32(), r.i32(), r.i64()
    if flags & BULK_UNUSED:
        return None
    if flags & (BULK_SEPARATE_FILE | BULK_ZLIB):
        raise ParseError('texture bulk data flags 0x%x are not supported' % flags)
    if flags & BULK_AT_END:
        start = pkg.summary['bulk_data_start_offset'] + offset
        return bytes(pkg.data[start:start + size_on_disk])
    return r.bytes(count)


class Texture2D:
    def __init__(self, pkg, export=None):
        self.export = export or pkg.find_export('Texture2D') or pkg.main_export()
        self.name = self.export.name
        self.props, r = read_object(pkg, self.export)
        r.strip_flags()
        r.strip_flags()
        if not r.bool32():
            raise ParseError('%s is not a cooked texture' % self.name)
        self.pixel_format, self.mips = None, []
        while True:
            if r.fname() == 'None':
                break
            skip = r.i32()
            r.i32(), r.i32(), r.i32()
            pf = r.fstring()
            r.i32()
            mips = []
            for _ in range(r.i32()):
                r.bool32()
                data = _bulk(r, pkg)
                mips.append(Mip(r.i32(), r.i32(), data))
            if r.tell() != skip:
                raise ParseError('%s: platform data ends at 0x%x, expected 0x%x' % (self.name, r.tell(), skip))
            if self.pixel_format is None:          # the first platform's data (PC has one)
                self.pixel_format, self.mips = pf, mips
        if self.pixel_format is None:
            raise ParseError('%s has no platform data' % self.name)

    @property
    def srgb(self):
        return self.props.get('SRGB', True)

    def decode(self):
        """The largest mip with data as an (H, W, 4) uint8 RGBA array."""
        for m in self.mips:
            if m.data is not None:
                return bc.decode(self.pixel_format, m.data, m.width, m.height)
        raise ParseError('%s: no mip has data' % self.name)
