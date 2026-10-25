"""Unreal Engine 4.14 packages (.uasset) as Tekken 7 PC cooks them: summary, names, imports, exports and tagged
properties.

Tekken 7's packages are cooked and unversioned (UE4 file version 0) with legacy file version -7, i.e. Unreal
Engine 4.14: object version 508 is assumed, which makes an export record 96 bytes. Everything is in the .uasset (no
.uexp); an export's SerialOffset is an absolute file offset.

Primitives (little-endian): FString = i32 n, n > 0: n Latin-1 bytes with a NUL, n < 0: -n UTF-16 code units with a
NUL; FName = i32 index into the name table + i32 number (> 0: "<name>_<number-1>"); FPackageIndex = i32, 0 null,
> 0 export (index-1), < 0 import (-index-1); TArray = i32 count + elements; a bulk TArray = i32 element size, i32
count, data.

Package summary (in order): u32 tag 0x9E2A83C1, i32 legacy version (-7), i32 legacy UE3 version, i32 UE4 version,
i32 licensee version, TArray custom versions {FGuid, i32}, i32 total header size, FString folder name, u32 package
flags, i32 name count + offset, i32 gatherable text count + offset, i32 export count + offset, i32 import count +
offset, i32 depends offset, i32 string asset reference count + offset, i32 thumbnail table offset, FGuid, TArray
generations {i32, i32}, two engine versions {u16 major, minor, patch, u32 changelist, FString branch}, u32
compression flags, TArray compressed chunks (4 x i32), u32 package source, TArray<FString> additional packages,
i32 asset registry offset, i64 bulk data start offset, i32 world tile info offset, TArray<i32> chunk ids, i32
preload dependency count + offset.
Name entry: FString + two u16 hashes. Import (28 bytes): FName class package, FName class, i32 outer, FName name.
Export (96 bytes): i32 class, super, template, outer; FName name; u32 flags; i32 serial size, serial offset; i32
forced export, not for client, not for server; FGuid; u32 package flags; i32 not always loaded for editor game,
is asset; five i32 dependency counts.

An export starts with its tagged properties, ended by a tag named "None", then i32 has-GUID [FGuid], then the
class's own data. A property tag: FName name, FName type, i32 size, i32 array index; StructProperty: FName struct
name + FGuid; BoolProperty: u8 value (size 0); ByteProperty/EnumProperty: FName enum; ArrayProperty: FName inner
type; then u8 has-GUID [FGuid]. An array of structs has one inner tag before the elements. Values come out as plain
Python (dicts in property order; object references as "Class'/Game/Path.Name'").
"""
import struct

import numpy as np


class ParseError(Exception):
    pass


class Reader:
    def __init__(self, data, pos=0, end=None, pkg=None):
        self.data, self.pos, self.end, self.pkg = data, pos, len(data) if end is None else end, pkg

    def tell(self):
        return self.pos

    def seek(self, pos):
        self.pos = pos

    def skip(self, n):
        self._need(n)
        self.pos += n

    def remaining(self):
        return self.end - self.pos

    def _need(self, n):
        if n < 0 or self.pos + n > self.end:
            raise ParseError('read of %d bytes at 0x%x past the end 0x%x' % (n, self.pos, self.end))

    def bytes(self, n):
        self._need(n)
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return bytes(b)

    def _unpack(self, fmt, n):
        self._need(n)
        v = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += n
        return v

    def u8(self):
        return self._unpack('<B', 1)[0]

    def i8(self):
        return self._unpack('<b', 1)[0]

    def u16(self):
        return self._unpack('<H', 2)[0]

    def i16(self):
        return self._unpack('<h', 2)[0]

    def u32(self):
        return self._unpack('<I', 4)[0]

    def i32(self):
        return self._unpack('<i', 4)[0]

    def u64(self):
        return self._unpack('<Q', 8)[0]

    def i64(self):
        return self._unpack('<q', 8)[0]

    def f32(self):
        return self._unpack('<f', 4)[0]

    def f64(self):
        return self._unpack('<d', 8)[0]

    def floats(self, n):
        return list(self._unpack('<%df' % n, 4 * n))

    def ints(self, n):
        return list(self._unpack('<%di' % n, 4 * n))

    def bool32(self):
        v = self.u32()
        if v > 1:
            raise ParseError('bad bool %d at 0x%x' % (v, self.pos - 4))
        return bool(v)

    def guid(self):
        return '%08X%08X%08X%08X' % self._unpack('<4I', 16)

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ''
        if abs(n) > 0x100000:
            raise ParseError('bad FString length %d at 0x%x' % (n, self.pos - 4))
        if n < 0:
            return self.bytes(-2 * n).decode('utf-16le').rstrip('\0')
        return self.bytes(n).decode('latin1').rstrip('\0')

    def fname(self):
        idx, num = self._unpack('<ii', 8)
        names = self.pkg.names
        if not 0 <= idx < len(names):
            raise ParseError('bad FName index %d at 0x%x' % (idx, self.pos - 8))
        return names[idx] if num == 0 else '%s_%d' % (names[idx], num - 1)

    def pkg_index(self):
        return self.pkg.resolve(self.i32())

    def tarray(self, fn):
        n = self.i32()
        if n < 0 or n > 50_000_000:
            raise ParseError('bad TArray count %d at 0x%x' % (n, self.pos - 4))
        return [fn() for _ in range(n)]

    def np_array(self, dtype, count):
        dt = np.dtype(dtype)
        self._need(dt.itemsize * count)
        a = np.frombuffer(self.data, dtype=dt, count=count, offset=self.pos).copy()
        self.pos += dt.itemsize * count
        return a

    def tarray_np(self, dtype):
        n = self.i32()
        if n < 0 or n > 200_000_000:
            raise ParseError('bad TArray count %d at 0x%x' % (n, self.pos - 4))
        return self.np_array(dtype, n)

    def bulk_array(self, dtype=None):
        """-> (element size, count, data as bytes or, with dtype, a numpy array)."""
        esize, n = self.i32(), self.i32()
        if esize < 0 or n < 0 or esize * n > len(self.data):
            raise ParseError('bad bulk array %d x %d at 0x%x' % (n, esize, self.pos - 8))
        if dtype is not None:
            dt = np.dtype(dtype)
            if dt.itemsize != esize:
                raise ParseError('bulk element size %d, expected %d at 0x%x' % (esize, dt.itemsize, self.pos - 8))
            return esize, n, self.np_array(dt, n)
        return esize, n, self.bytes(esize * n)

    def strip_flags(self):
        """FStripDataFlags: (global, class); global & 1 = editor data stripped, & 2 = stripped for servers."""
        return self.u8(), self.u8()


# ------------------------------------------------------------------------------------------------ packages
class Import:
    __slots__ = ('class_package', 'class_name', 'outer', 'name')

    def __init__(self, class_package, class_name, outer, name):
        self.class_package, self.class_name, self.outer, self.name = class_package, class_name, outer, name


class Export:
    __slots__ = ('class_index', 'super_index', 'template_index', 'outer', 'name', 'flags', 'serial_size',
                 'serial_offset', 'is_asset', 'class_name')


class Package:
    def __init__(self, data, path=''):
        self.data, self.path = data, path
        r = Reader(data, 0, pkg=self)
        self.summary = self._summary(r)
        self.names = self._names()
        self.imports = self._imports()
        self.exports = self._exports()

    def _summary(self, r):
        s = {}
        if r.u32() != 0x9E2A83C1:
            raise ParseError('%s is not an Unreal package' % self.path)
        if r.i32() != -7:
            raise ParseError('%s: not an Unreal Engine 4.14 package' % self.path)
        r.i32()
        if r.i32() not in (0, 508):
            raise ParseError('%s: unexpected UE4 file version' % self.path)
        r.i32()
        r.tarray(lambda: (r.guid(), r.i32()))
        r.i32()
        r.fstring()
        s['package_flags'] = r.u32()
        s['name_count'], s['name_offset'] = r.i32(), r.i32()
        r.i32(), r.i32()
        s['export_count'], s['export_offset'] = r.i32(), r.i32()
        s['import_count'], s['import_offset'] = r.i32(), r.i32()
        s['depends_offset'] = r.i32()
        r.i32(), r.i32(), r.i32()
        r.guid()
        r.tarray(lambda: (r.i32(), r.i32()))
        for _ in range(2):
            r.u16(), r.u16(), r.u16(), r.u32(), r.fstring()
        r.u32()
        if r.tarray(lambda: r.ints(4)):
            raise ParseError('%s: compressed package chunks are not supported' % self.path)
        r.u32()
        r.tarray(r.fstring)
        r.i32()
        s['bulk_data_start_offset'] = r.i64()
        r.i32()
        r.tarray(r.i32)
        r.i32(), r.i32()
        return s

    def _names(self):
        r = Reader(self.data, self.summary['name_offset'], pkg=self)
        out = []
        for _ in range(self.summary['name_count']):
            out.append(r.fstring())
            r.skip(4)
        return out

    def _imports(self):
        r = Reader(self.data, self.summary['import_offset'], pkg=self)
        return [Import(r.fname(), r.fname(), r.i32(), r.fname()) for _ in range(self.summary['import_count'])]

    def _exports(self):
        r = Reader(self.data, self.summary['export_offset'], pkg=self)
        out = []
        for _ in range(self.summary['export_count']):
            e = Export()
            e.class_index, e.super_index, e.template_index, e.outer = r.i32(), r.i32(), r.i32(), r.i32()
            e.name, e.flags, e.serial_size, e.serial_offset = r.fname(), r.u32(), r.i32(), r.i32()
            r.skip(12 + 16 + 4 + 4)
            e.is_asset = r.i32() != 0
            r.skip(20)
            if e.serial_offset < 0 or e.serial_offset + e.serial_size > len(self.data):
                raise ParseError('%s: export %s lies outside the file' % (self.path, e.name))
            out.append(e)
        if self.summary['depends_offset'] and r.tell() != self.summary['depends_offset']:
            raise ParseError('%s: export table size does not match Unreal Engine 4.14' % self.path)
        for e in out:                   # (a blueprint's objects have classes among its own exports)
            i = e.class_index
            e.class_name = (self.imports[-i - 1].name if i < 0 else out[i - 1].name if i > 0 else None) or 'Class'
        return out

    def object_name(self, index):
        if index == 0:
            return None
        return self.imports[-index - 1].name if index < 0 else self.exports[index - 1].name

    def object_path(self, index):
        """Outer chain joined with '.' after the package and ':' below it."""
        if index == 0:
            return None
        parts, i = [], index
        while i != 0:
            if i < 0:
                imp = self.imports[-i - 1]
                parts.append(imp.name)
                i = imp.outer
            else:
                exp = self.exports[i - 1]
                parts.append(exp.name)
                i = exp.outer
                if i == 0:
                    parts.append(self.package_name)
        parts.reverse()
        return parts[0] + ('.' + parts[1] if len(parts) > 1 else '') + ''.join(':' + p for p in parts[2:])

    def object_class(self, index):
        if index == 0:
            return None
        return self.imports[-index - 1].class_name if index < 0 else self.exports[index - 1].class_name

    def resolve(self, index):
        """FPackageIndex -> "Class'/Game/Path.Object'" (None for null)."""
        if index == 0:
            return None
        return "%s'%s'" % (self.object_class(index), self.object_path(index))

    @property
    def package_name(self):
        p = self.path
        for pre, rep in (('TekkenGame/Content/', '/Game/'), ('Engine/Content/', '/Engine/')):
            if p.startswith(pre):
                p = rep + p[len(pre):]
        return p.rsplit('.', 1)[0]

    def export_reader(self, e):
        if isinstance(e, int):
            e = self.exports[e]
        return Reader(self.data, e.serial_offset, e.serial_offset + e.serial_size, pkg=self)

    def find_export(self, class_name=None, name=None):
        for e in self.exports:
            if (class_name is None or e.class_name == class_name) and (name is None or e.name == name):
                return e
        return None

    def main_export(self):
        """The asset export, else the export named like the package."""
        for e in self.exports:
            if e.is_asset:
                return e
        base = self.package_name.rsplit('/', 1)[-1]
        for e in self.exports:
            if e.name == base:
                return e
        return self.exports[0]


def load(paks, path):
    """The package at a pak path or object path."""
    e = paks.find(path)
    if e is None:
        raise KeyError(path)
    return Package(paks.read(e.path), e.path)


# ------------------------------------------------------------------------------------------------ properties
def _vec(r, n):
    return [round(v, 6) for v in r.floats(n)]


# structs stored in their binary form rather than as tagged properties
NATIVE_STRUCTS = {
    'Vector': lambda r: _vec(r, 3),
    'Vector2D': lambda r: _vec(r, 2),
    'Vector4': lambda r: _vec(r, 4),
    'Plane': lambda r: _vec(r, 4),
    'Quat': lambda r: _vec(r, 4),
    'Rotator': lambda r: dict(zip(('pitch', 'yaw', 'roll'), _vec(r, 3))),
    'LinearColor': lambda r: dict(zip('rgba', _vec(r, 4))),
    'Color': lambda r: dict(zip('bgra', r.bytes(4))),
    'IntPoint': lambda r: r.ints(2),
    'IntVector': lambda r: r.ints(3),
    'Guid': lambda r: r.guid(),
    'Box': lambda r: {'min': _vec(r, 3), 'max': _vec(r, 3), 'valid': r.u8()},
    'Box2D': lambda r: {'min': _vec(r, 2), 'max': _vec(r, 2), 'valid': r.u8()},
    'BoxSphereBounds': lambda r: {'origin': _vec(r, 3), 'extent': _vec(r, 3), 'radius': round(r.f32(), 6)},
    'Matrix': lambda r: _vec(r, 16),
    'DateTime': lambda r: r.i64(),
    'Timespan': lambda r: r.i64(),
    'StringAssetReference': lambda r: r.fstring(),
    'StringClassReference': lambda r: r.fstring(),
    'SoftObjectPath': lambda r: r.fstring(),
    'SoftClassPath': lambda r: r.fstring(),
    'RichCurveKey': lambda r: {'interp': r.u8(), 'tangent_mode': r.u8(), 'tangent_weight_mode': r.u8(),
                               'time': r.f32(), 'value': r.f32(), 'arrive_tangent': r.f32(),
                               'arrive_weight': r.f32(), 'leave_tangent': r.f32(), 'leave_weight': r.f32()},
}


class Tag:
    __slots__ = ('name', 'type', 'size', 'index', 'struct_name', 'bool_val', 'enum_name', 'inner_type')


def read_tag(r):
    t = Tag()
    t.name = r.fname()
    if t.name == 'None':
        return None
    t.struct_name = t.enum_name = t.inner_type = t.bool_val = None
    t.type, t.size, t.index = r.fname(), r.i32(), r.i32()
    if t.type == 'StructProperty':
        t.struct_name = r.fname()
        r.guid()
    elif t.type == 'BoolProperty':
        t.bool_val = bool(r.u8())
    elif t.type in ('ByteProperty', 'EnumProperty'):
        t.enum_name = r.fname()
    elif t.type == 'ArrayProperty':
        t.inner_type = r.fname()
    if r.u8():
        r.guid()
    return t


def read_properties(r, strict=False):
    """A tagged property list up to its "None" (an unreadable value is kept raw unless strict)."""
    out = {}
    while True:
        tag = read_tag(r)
        if tag is None:
            return out
        start = r.tell()
        try:
            val = read_value(r, tag.type, tag, tag.size)
            if r.tell() != start + tag.size:
                raise ParseError('property %s (%s) used %d of %d bytes' % (tag.name, tag.type, r.tell() - start,
                                                                          tag.size))
        except (ParseError, struct.error, UnicodeDecodeError) as ex:
            if strict:
                raise
            r.seek(start)
            val = {'_raw': r.bytes(tag.size).hex(), '_type': tag.type, '_error': str(ex)}
        out[tag.name if tag.index == 0 and tag.name not in out else '%s[%d]' % (tag.name, tag.index)] = val


def read_value(r, ptype, tag, size):
    if ptype == 'BoolProperty':
        return tag.bool_val if tag is not None and tag.bool_val is not None else bool(r.u8())
    simple = {'IntProperty': r.i32, 'Int8Property': r.i8, 'Int16Property': r.i16, 'Int64Property': r.i64,
              'UInt16Property': r.u16, 'UInt32Property': r.u32, 'UInt64Property': r.u64, 'DoubleProperty': r.f64,
              'NameProperty': r.fname, 'StrProperty': r.fstring, 'LazyObjectProperty': r.guid}
    if ptype in simple:
        return simple[ptype]()
    if ptype == 'FloatProperty':
        return round(r.f32(), 7)
    if ptype == 'TextProperty':
        return read_text(r, size)
    if ptype in ('ObjectProperty', 'ClassProperty', 'WeakObjectProperty', 'InterfaceProperty'):
        return r.pkg_index()
    if ptype in ('AssetObjectProperty', 'AssetClassProperty', 'SoftObjectProperty', 'SoftClassProperty'):
        return r.fstring()
    if ptype in ('ByteProperty', 'EnumProperty'):
        enum = tag.enum_name if tag is not None else None
        if size == 1 or (enum in (None, 'None') and size != 8):
            return r.u8()
        return r.fname()
    if ptype == 'StructProperty':
        return read_struct(r, tag.struct_name)
    if ptype == 'ArrayProperty':
        return read_array(r, tag, size)
    if ptype == 'MapProperty':
        return read_map(r, size)
    if ptype == 'DelegateProperty':
        return {'object': r.pkg_index(), 'function': r.fname()}
    if ptype == 'MulticastDelegateProperty':
        return r.tarray(lambda: {'object': r.pkg_index(), 'function': r.fname()})
    raise ParseError('unsupported property type %s' % ptype)


def read_struct(r, struct_name):
    fn = NATIVE_STRUCTS.get(struct_name)
    return fn(r) if fn is not None else read_properties(r)


def read_text(r, size):
    """FText: u32 flags, i8 history type; type 0: FString namespace, key, source; type -1: nothing more."""
    start = r.tell()
    flags = r.u32()
    history = r.i8()
    if history == 0:
        return {'namespace': r.fstring(), 'key': r.fstring(), 'source': r.fstring()}
    if history == -1:
        rest = size - (r.tell() - start) if size is not None else 0
        if rest:
            return {'invariant': r.fstring()} if rest >= 4 else None
        return None
    raise ParseError('FText history type %d (flags %x)' % (history, flags))


def read_array(r, tag, size):
    start = r.tell()
    n = r.i32()
    inner = tag.inner_type
    if n < 0 or n > 10_000_000:
        raise ParseError('bad array count %d' % n)
    if inner == 'StructProperty':
        itag = read_tag(r)
        if itag is None or itag.type != 'StructProperty':
            raise ParseError('array %s has no inner struct tag' % tag.name)
        return [read_struct(r, itag.struct_name) for _ in range(n)]
    if inner in ('ByteProperty', 'EnumProperty'):
        if n and size - 4 == 8 * n:
            return [r.fname() for _ in range(n)]
        return [r.u8() for _ in range(n)]
    if inner == 'BoolProperty':
        return [bool(r.u8()) for _ in range(n)]
    out = [read_value(r, inner, None, None) for _ in range(n)]
    if size is not None and r.tell() - start != size:
        raise ParseError('array %s used %d of %d bytes' % (tag.name, r.tell() - start, size))
    return out


# UE 4.14 map tags do not name their key and value types: try the layouts Tekken 7 uses against the size.
_MAP_LAYOUTS = [
    ('NameProperty', 'IntProperty'), ('NameProperty', 'FloatProperty'), ('NameProperty', 'ObjectProperty'),
    ('NameProperty', 'NameProperty'), ('IntProperty', 'IntProperty'), ('IntProperty', 'ObjectProperty'),
    ('ObjectProperty', 'ObjectProperty'), ('StrProperty', 'StrProperty'), ('NameProperty', 'StrProperty'),
    ('ByteProperty', 'IntProperty'), ('NameProperty', 'Struct'), ('IntProperty', 'Struct'),
    ('ByteProperty', 'Struct'), ('ObjectProperty', 'Struct'),
]


def read_map(r, size):
    start = r.tell()
    for kt, vt in _MAP_LAYOUTS:
        r.seek(start)
        try:
            if r.i32() != 0:
                raise ParseError('map with removed keys')
            n = r.i32()
            if n < 0 or n > 100000:
                raise ParseError('bad map count')
            items = []
            for _ in range(n):
                k = read_value(r, kt, None, None)
                v = read_properties(r, strict=True) if vt == 'Struct' else read_value(r, vt, None, None)
                items.append([k, v])
            if r.tell() - start == size:
                return {'_map': '%s->%s' % (kt, vt), 'items': items}
        except (ParseError, struct.error, UnicodeDecodeError, IndexError):
            pass
    r.seek(start)
    raise ParseError('map layout not recognised')


def read_object(pkg, export):
    """An export's tagged properties, and a reader at its class data."""
    r = pkg.export_reader(export)
    props = read_properties(r)
    has_guid = r.i32()
    if has_guid not in (0, 1):
        raise ParseError('bad object GUID flag %d after the properties of %s' % (has_guid, export.name))
    if has_guid:
        r.guid()
    return props, r


def export_properties(pkg):
    """[(export, properties or None)] of every export (None where they cannot be read)."""
    out = []
    for ex in pkg.exports:
        try:
            props, _ = read_object(pkg, ex)
        except Exception:
            props = None
        out.append((ex, props))
    return out
