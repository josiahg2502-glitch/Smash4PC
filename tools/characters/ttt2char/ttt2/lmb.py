"""TTT2 Wii U 2D layouts: Namco "Lumen" LMB files (data007), read, edited and written back byte for byte.

Big-endian. A 0x40-byte header ("LMB\\0", u32 0x10, u32 0x10010200, ..., u32 @0x1C = file size), then tags
{u32 type, u32 payload size in 32-bit words, payload}; the last tag is 0xFF00 (empty) and ends the file. The
order is fixed: the tables, then every shape, then (in a few files) 0x0007 + 0xF018 characters, then every
sprite, then 0xFF00.

Tables (each a u32 count, then the records):
    F001 symbols: {u32 length, bytes, NUL, padded to 4}. Names of everything (labels, objects, atlases).
    F002 colours: {u16 r, g, b, a} (0x100 = 1.0).  F003 matrices: {f32 a, b, c, d, tx, ty}.
    F103 positions: {f32 x, y}.  F004 bounds: {f32 x0, y0, x1, y1}.  F005 action scripts: {u32 length, bytes}.
    F007 atlases: {u32 id, u32 name symbol, f32 width, height}: texture pages; the NTP3 that follows the LMB in
    its archive holds one texture per atlas, its GIDX = the atlas id.
    F008 F009 F00A 000A F00B: plain words (kept).  F00C properties (12 words): 0, 1, 2, root character id, -1,
    root id, u16 largest depth index of any sprite + u16 0, f32 60 (fps), f32 1280, f32 720, 0, 0.
    F00D counts: shape count, 0, sprite count, 0, ...
Characters share one id space (1 .. root); ids not used (the source's bitmaps) can take new characters.
    F022 shape: {u32 id, 0, bounds index, 0, graphic count}, then its F024 graphics: {u32 atlas id, u16 fill
    (0x41), u16 vertex count, u32 index count, vertices {f32 x, y, u, v}, u16 triangle indices padded to 4}.
    0027 sprite: {u32 id, 0, 0, label count, frame count, keyframe count, u16 depth count, u16 0}, followed by its
    labels 002B {name symbol, frame, 0}, its frames 0001 {index, child count} and its keyframes F105 {index, child
    count} (one per label: the whole display list at that frame, for jumps), each followed by its children:
        0004 place (12 words): character, placement id (unique in the file), 0, name symbol (0: none), u16 mode
             (1 place, 2 move/change, 3 replace the character), u16 blend, u16 depth, u16 0, u16 frame, u16 0,
             u16 position kind (0x8000: F103 index, 0: F003 index, 0xFFFF: none), s16 position index, s32 colour
             multiply and s32 colour add (F002 index or -1), 0, 0;
        0005 remove {0, u16 depth, u16 0};  000C do action {F005 index, 0} (0 = stop).
A per-fighter picture in TTT2 is a sprite with one label per fighter ("face_jun", "chara_face_yoko_law"); the
label's frame stops and replaces the object at a depth with the fighter's shape. The game jumps to the label of
the fighter it shows.

Written for this project from the files themselves (no third-party code).
"""
import copy
import struct

T_SYMBOLS, T_COLORS, T_MATRICES, T_POSITIONS, T_BOUNDS = 0xF001, 0xF002, 0xF003, 0xF103, 0xF004
T_ACTIONS, T_ATLASES, T_PROPS, T_COUNTS = 0xF005, 0xF007, 0xF00C, 0xF00D
T_SHAPE, T_GRAPHIC, T_SPRITE, T_LABEL, T_FRAME, T_KEYFRAME = 0xF022, 0xF024, 0x0027, 0x002B, 0x0001, 0xF105
T_PLACE, T_REMOVE, T_ACTION, T_END = 0x0004, 0x0005, 0x000C, 0xFF00
WORD_TABLES = (0xF008, 0xF009, 0xF00A, 0x000A, 0xF00B, T_PROPS, T_COUNTS)

MODE_PLACE, MODE_MOVE, MODE_REPLACE = 1, 2, 3
POS_MATRIX, POS_POSITION, POS_NONE = 0x0000, 0x8000, 0xFFFF
NO_ID = 0xFFFFFFFF


def _tag(t, payload):
    return struct.pack('>II', t, len(payload) // 4) + payload


def _pad4(b):
    return b + bytes(-len(b) % 4)


class Raw:
    """A tag kept as it is."""

    def __init__(self, t, payload):
        self.type, self.payload = t, payload

    def encode(self):
        return _tag(self.type, self.payload)


class Words:
    """A tag of plain u32 words."""

    def __init__(self, t, payload):
        self.type = t
        self.w = list(struct.unpack('>%dI' % (len(payload) // 4), payload))

    def encode(self):
        return _tag(self.type, struct.pack('>%dI' % len(self.w), *self.w))


class Symbols:
    type = T_SYMBOLS

    def __init__(self, payload):
        n, = struct.unpack_from('>I', payload, 0)
        q, self.items = 4, []
        for _ in range(n):
            ln, = struct.unpack_from('>I', payload, q)
            self.items.append(payload[q + 4:q + 4 + ln].decode('latin1'))
            q += 4 + ((ln + 4) & ~3)
        if q != len(payload):
            raise ValueError('LMB symbol table: %d bytes left' % (len(payload) - q))

    def encode(self):
        out = [struct.pack('>I', len(self.items))]
        for s in self.items:
            b = s.encode('latin1')
            out.append(struct.pack('>I', len(b)) + b + bytes(((len(b) + 4) & ~3) - len(b)))
        return _tag(self.type, b''.join(out))


class Records:
    """A counted list of fixed-size records (colours, matrices, positions, bounds, atlases)."""
    FORMATS = {T_COLORS: '>4H', T_MATRICES: '>6f', T_POSITIONS: '>2f', T_BOUNDS: '>4f', T_ATLASES: '>IIff'}

    def __init__(self, t, payload):
        self.type = t
        fmt = self.FORMATS[t]
        size = struct.calcsize(fmt)
        n, = struct.unpack_from('>I', payload, 0)
        if 4 + n * size != len(payload):
            raise ValueError('LMB table %#x: size does not match its count' % t)
        self.items = [list(struct.unpack_from(fmt, payload, 4 + i * size)) for i in range(n)]

    def encode(self):
        fmt = self.FORMATS[self.type]
        return _tag(self.type, struct.pack('>I', len(self.items)) +
                    b''.join(struct.pack(fmt, *r) for r in self.items))


class Actions:
    type = T_ACTIONS

    def __init__(self, payload):
        n, = struct.unpack_from('>I', payload, 0)
        q, self.items = 4, []
        for _ in range(n):
            ln, = struct.unpack_from('>I', payload, q)
            self.items.append(payload[q + 4:q + 4 + ln])
            q += 4 + ((ln + 3) & ~3)
        if q != len(payload):
            raise ValueError('LMB action table: %d bytes left' % (len(payload) - q))

    def encode(self):
        return _tag(self.type, struct.pack('>I', len(self.items)) +
                    b''.join(struct.pack('>I', len(a)) + _pad4(a) for a in self.items))


class Graphic:
    """A textured mesh of a shape: vertices [x, y, u, v], triangle indices, atlas id."""

    def __init__(self, atlas, verts, indices, fill=0x41):
        self.atlas, self.verts, self.indices, self.fill = atlas, verts, indices, fill

    @classmethod
    def decode(cls, payload):
        atlas, fill, nv, ni = struct.unpack_from('>IHHI', payload, 0)
        verts = [list(struct.unpack_from('>4f', payload, 12 + 16 * i)) for i in range(nv)]
        idx = list(struct.unpack_from('>%dH' % ni, payload, 12 + 16 * nv))
        return cls(atlas, verts, idx, fill)

    def encode(self):
        p = struct.pack('>IHHI', self.atlas, self.fill, len(self.verts), len(self.indices))
        p += b''.join(struct.pack('>4f', *v) for v in self.verts)
        p += _pad4(struct.pack('>%dH' % len(self.indices), *self.indices))
        return _tag(T_GRAPHIC, p)

    def uv_box(self):
        us = [v[2] for v in self.verts]
        vs = [v[3] for v in self.verts]
        return min(us), min(vs), max(us), max(vs)

    def xy_box(self):
        xs = [v[0] for v in self.verts]
        ys = [v[1] for v in self.verts]
        return min(xs), min(ys), max(xs), max(ys)


class Shape:
    def __init__(self, cid, bounds, graphics, unk1=0, unk3=0):
        self.id, self.bounds, self.graphics, self.unk1, self.unk3 = cid, bounds, graphics, unk1, unk3

    def encode(self):
        out = [_tag(T_SHAPE, struct.pack('>5I', self.id, self.unk1, self.bounds, self.unk3, len(self.graphics)))]
        return b''.join(out + [g.encode() for g in self.graphics])


class Place:
    """0x0004: put a character at a depth (mode 1), change the object there (2), or replace its character (3)."""
    FMT = '>IIIIHHHHHHHhiiII'
    FIELDS = ('char', 'pid', 'unk2', 'name', 'mode', 'blend', 'depth', 'unk5', 'placed', 'unk6', 'posflags', 'posid',
              'cmul', 'cadd', 'unk10', 'unk11')

    def __init__(self, *values, **kw):
        defaults = dict(char=0, pid=NO_ID, unk2=0, name=0, mode=MODE_PLACE, blend=0, depth=0, unk5=0, placed=0,
                        unk6=0, posflags=POS_NONE, posid=-1, cmul=-1, cadd=-1, unk10=0, unk11=0)
        defaults.update(zip(self.FIELDS, values))
        defaults.update(kw)
        self.__dict__.update(defaults)

    @classmethod
    def decode(cls, payload):
        return cls(*struct.unpack(cls.FMT, payload))

    def encode(self):
        return _tag(T_PLACE, struct.pack(self.FMT, *(getattr(self, f) for f in self.FIELDS)))

    def copy(self, **kw):
        return Place(*(getattr(self, f) for f in self.FIELDS), **kw)


class Remove:
    def __init__(self, depth, unk0=0, unk1=0):
        self.depth, self.unk0, self.unk1 = depth, unk0, unk1

    def encode(self):
        return _tag(T_REMOVE, struct.pack('>IHH', self.unk0, self.depth, self.unk1))

    def copy(self, **kw):
        c = Remove(self.depth, self.unk0, self.unk1)
        c.__dict__.update(kw)
        return c


class DoAction:
    def __init__(self, action, unk=0):
        self.action, self.unk = action, unk

    def encode(self):
        return _tag(T_ACTION, struct.pack('>II', self.action, self.unk))

    def copy(self, **kw):
        c = DoAction(self.action, self.unk)
        c.__dict__.update(kw)
        return c


def _child(t, payload):
    if t == T_PLACE:
        return Place.decode(payload)
    if t == T_REMOVE:
        u0, depth, u1 = struct.unpack('>IHH', payload)
        return Remove(depth, u0, u1)
    if t == T_ACTION:
        return DoAction(*struct.unpack('>II', payload))
    return Raw(t, payload)


def _copy_child(c, **kw):
    if isinstance(c, Raw):
        return Raw(c.type, c.payload)
    return c.copy(**kw)


class Frame:
    def __init__(self, index, children, key=False):
        self.index, self.children, self.key = index, children, key

    def encode(self):
        out = [_tag(T_KEYFRAME if self.key else T_FRAME, struct.pack('>II', self.index, len(self.children)))]
        return b''.join(out + [c.encode() for c in self.children])


class Label:
    def __init__(self, name, frame, unk=0):
        self.name, self.frame, self.unk = name, frame, unk

    def encode(self):
        return _tag(T_LABEL, struct.pack('>III', self.name, self.frame, self.unk))


class Sprite:
    def __init__(self, cid, depths, labels=(), frames=(), keyframes=(), unk1=0, unk2=0, unk6=0):
        self.id, self.depths = cid, depths
        self.labels, self.frames, self.keyframes = list(labels), list(frames), list(keyframes)
        self.unk1, self.unk2, self.unk6 = unk1, unk2, unk6

    def encode(self):
        out = [_tag(T_SPRITE, struct.pack('>6IHH', self.id, self.unk1, self.unk2, len(self.labels), len(self.frames),
                                          len(self.keyframes), self.depths, self.unk6))]
        return b''.join(out + [x.encode() for x in self.labels + self.frames + self.keyframes])

    def all_children(self):
        for f in self.frames + self.keyframes:
            for c in f.children:
                yield f, c

    def frame(self, index, key=False):
        for f in (self.keyframes if key else self.frames):
            if f.index == index:
                return f
        return None


class Lmb:
    """An LMB file: `items` are its tags in order (tables, shapes, sprites); edit them, then write()."""

    @classmethod
    def read(cls, d):
        if d[:4] != b'LMB\0':
            raise ValueError('not an LMB layout')
        self = cls()
        self.header = bytes(d[:0x40])
        tags, p = [], 0x40
        while True:
            t, n = struct.unpack_from('>II', d, p)
            tags.append((t, d[p + 8:p + 8 + 4 * n]))
            p += 8 + 4 * n
            if t == T_END:
                break
        self.trailer = bytes(d[p:])
        self.items = []
        i = 0
        while i < len(tags):
            t, pl = tags[i]
            i += 1
            if t == T_SYMBOLS:
                self.items.append(Symbols(pl))
            elif t in Records.FORMATS:
                self.items.append(Records(t, pl))
            elif t == T_ACTIONS:
                self.items.append(Actions(pl))
            elif t in WORD_TABLES:
                self.items.append(Words(t, pl))
            elif t == T_SHAPE:
                cid, u1, b, u3, ng = struct.unpack('>5I', pl)
                gs = [Graphic.decode(tags[i + k][1]) for k in range(ng)]
                i += ng
                self.items.append(Shape(cid, b, gs, u1, u3))
            elif t == T_SPRITE:
                cid, u1, u2, nl, nf, nk, depths, u6 = struct.unpack('>6IHH', pl)
                sp = Sprite(cid, depths, unk1=u1, unk2=u2, unk6=u6)
                for _ in range(nl):
                    sp.labels.append(Label(*struct.unpack('>III', tags[i][1])))
                    i += 1
                for count, key in ((nf, False), (nk, True)):
                    for _ in range(count):
                        ft, fp = tags[i]
                        if ft != (T_KEYFRAME if key else T_FRAME):
                            raise ValueError('LMB sprite %#x: tag %#x where a frame should be' % (cid, ft))
                        idx, nc = struct.unpack('>II', fp)
                        children = [_child(*tags[i + 1 + k]) for k in range(nc)]
                        i += 1 + nc
                        (sp.keyframes if key else sp.frames).append(Frame(idx, children, key))
                self.items.append(sp)
            else:
                self.items.append(Raw(t, pl))
        return self

    def write(self):
        body = b''.join(x.encode() for x in self.items)
        h = bytearray(self.header)
        struct.pack_into('>I', h, 0x1C, 0x40 + len(body) + len(self.trailer))
        return bytes(h) + body + self.trailer

    # ------------------------------------------------------------------ access
    def _table(self, t):
        for x in self.items:
            if getattr(x, 'type', None) == t:
                return x
        raise KeyError('LMB has no table %#x' % t)

    @property
    def symbols(self):
        return self._table(T_SYMBOLS).items

    @property
    def matrices(self):
        return self._table(T_MATRICES).items

    @property
    def positions(self):
        return self._table(T_POSITIONS).items

    @property
    def atlases(self):
        return self._table(T_ATLASES).items

    @property
    def props(self):
        return self._table(T_PROPS).w

    @property
    def counts(self):
        return self._table(T_COUNTS).w

    def shapes(self):
        return [x for x in self.items if isinstance(x, Shape)]

    def sprites(self):
        return [x for x in self.items if isinstance(x, Sprite)]

    def char(self, cid):
        for x in self.items:
            if isinstance(x, (Shape, Sprite)) and x.id == cid:
                return x
        return None

    def symbol(self, name, add=True):
        """The index of symbol `name` (added when missing and `add`)."""
        s = self.symbols
        if name in s:
            return s.index(name)
        if not add:
            raise KeyError(name)
        s.append(name)
        return len(s) - 1

    def labels_of(self, sprite):
        """{label name: frame} of a sprite."""
        s = self.symbols
        return {s[l.name]: l.frame for l in sprite.labels}

    def used_ids(self):
        ids = {x.id for x in self.items if isinstance(x, (Shape, Sprite))}
        for x in self.items:
            if isinstance(x, Raw) and x.type == 0x0007:
                ids.add(struct.unpack_from('>I', x.payload)[0])
        return ids

    def free_ids(self):
        """Character ids below the root no character has (new shapes and sprites can take them)."""
        used = self.used_ids()
        return [i for i in range(1, self.props[3]) if i not in used]

    def next_placement_id(self):
        m = -1
        for sp in self.sprites():
            for _, c in sp.all_children():
                if isinstance(c, Place) and c.pid != NO_ID:
                    m = max(m, c.pid)
        return m + 1

    def find_named(self, name):
        """Every (sprite, frame, place) that puts an object called `name`."""
        if name not in self.symbols:
            return []
        k = self.symbols.index(name)
        return [(sp, f, c) for sp in self.sprites() for f, c in sp.all_children()
                if isinstance(c, Place) and c.name == k and c.mode == MODE_PLACE]

    # ------------------------------------------------------------------ edits
    def add_record(self, t, rec):
        tab = self._table(t).items
        tab.append(list(rec))
        return len(tab) - 1

    def add_atlas(self, w, h, name=''):
        """A new texture page; its texture goes into the NTP3 with GIDX = the returned id."""
        at = self.atlases
        aid = max(a[0] for a in at) + 1 if at else 0
        at.append([aid, self.symbol(name), float(w), float(h)])
        return aid

    def _insert_char(self, obj, cls):
        """Insert a shape or sprite, keeping its section's ids ascending (`obj.id` must be free)."""
        if obj.id in self.used_ids():
            raise ValueError('LMB character id %#x is taken' % obj.id)
        same = [k for k, x in enumerate(self.items) if isinstance(x, cls)]
        pos = next((k for k in same if self.items[k].id > obj.id), same[-1] + 1)
        self.items.insert(pos, obj)
        self.counts[0 if cls is Shape else 2] += 1
        return obj

    def take_id(self, below=None, above=0):
        """A free character id (between `above` and `below` when given: sprites can only use characters defined
        before them, i.e. with smaller ids)."""
        ids = [i for i in self.free_ids() if i > above and (below is None or i < below)]
        if not ids:
            raise ValueError('LMB has no free character id below %#x' % (below or 0))
        return ids[0]

    def add_shape(self, cid, graphics, bounds=None):
        """A shape from Graphic objects; its bounds from the vertices unless given."""
        if bounds is None:
            boxes = [g.xy_box() for g in graphics]
            bounds = (min(b[0] for b in boxes), min(b[1] for b in boxes),
                      max(b[2] for b in boxes), max(b[3] for b in boxes))
        return self._insert_char(Shape(cid, self.add_record(T_BOUNDS, bounds), graphics), Shape)

    def add_sprite(self, sprite):
        self._insert_char(sprite, Sprite)
        self.fix_depths()
        return sprite

    def fix_depths(self):
        """Keep the properties' largest depth index >= every sprite's depth count - 1."""
        p = self.props
        need = max(sp.depths for sp in self.sprites()) - 1
        if (p[6] >> 16) < need:
            p[6] = (need << 16) | (p[6] & 0xFFFF)

    def insert_depth(self, sprite, at):
        """Free depth `at` of `sprite`: every object at a depth >= at moves one up (the drawing order is kept)."""
        for _, c in sprite.all_children():
            if isinstance(c, (Place, Remove)) and c.depth >= at:
                c.depth += 1
        sprite.depths += 1
        self.fix_depths()

    def _renumber(self, children, pid_map):
        out = []
        for c in children:
            c = _copy_child(c)
            if isinstance(c, Place) and c.pid != NO_ID:
                if c.pid not in pid_map:            # a keyframe repeats its frame's placement ids
                    pid_map[c.pid] = self._next_pid
                    self._next_pid += 1
                c.pid = pid_map[c.pid]
            out.append(c)
        return out

    def clone_sprite(self, src_id, new_id, char_map=None):
        """A copy of sprite `src_id` as `new_id` with the characters in char_map replaced."""
        src = self.char(src_id)
        char_map = char_map or {}
        self._next_pid = self.next_placement_id()
        pid_map = {}
        frames = []
        for f in src.frames + src.keyframes:
            ch = self._renumber(f.children, pid_map)
            for c in ch:
                if isinstance(c, Place) and c.char in char_map:
                    c.char = char_map[c.char]
            frames.append(Frame(f.index, ch, f.key))
        sp = Sprite(new_id, src.depths, [Label(l.name, l.frame, l.unk) for l in src.labels],
                    [f for f in frames if not f.key], [f for f in frames if f.key], src.unk1, src.unk2, src.unk6)
        return self.add_sprite(sp)

    def clone_depth(self, sprite, src_depth, dst_depth, char=None, name=None, matrix_fn=None, position_fn=None):
        """Copy everything `sprite` does at src_depth (places, changes, removes, in frames and keyframes) to
        dst_depth, with new placement ids, another character and name if given, and the matrices / positions
        changed by matrix_fn / position_fn (as new table entries). Returns the number of tags added."""
        self._next_pid = self.next_placement_id()
        name_k = self.symbol(name) if name else None
        pid_map, added = {}, 0
        for f in sprite.frames + sprite.keyframes:
            new = []
            for c in f.children:
                if isinstance(c, Remove) and c.depth == src_depth:
                    new.append(c.copy(depth=dst_depth))
                elif isinstance(c, Place) and c.depth == src_depth:
                    d = self._renumber([c], pid_map)[0]
                    d.depth = dst_depth
                    if char is not None and c.char != 0:
                        d.char = char
                    if name_k is not None and c.name != 0:
                        d.name = name_k
                    if c.posflags == POS_MATRIX and matrix_fn:
                        d.posid = self.add_record(T_MATRICES, matrix_fn(list(self.matrices[c.posid])))
                    elif c.posflags == POS_POSITION and position_fn:
                        d.posid = self.add_record(T_POSITIONS, position_fn(list(self.positions[c.posid])))
                    new.append(d)
            f.children.extend(new)
            added += len(new)
        sprite.depths = max(sprite.depths, dst_depth + 1)
        self.fix_depths()
        return added

    def label_span(self, sprite, name):
        """(first frame, end) of label `name`: its frames run to the next label's frame (or the sprite's end)."""
        labels = self.labels_of(sprite)
        start = labels[name]
        later = [f for f in labels.values() if f > start]
        end = min(later) if later else max([f.index for f in sprite.frames] + [start]) + 1
        return start, end

    def add_label_like(self, sprite, ref_label, new_label, char_map):
        """A new label `new_label` on new last frames of `sprite` that play what `ref_label`'s frames play (all of
        them, up to the next label: some labels are animations, e.g. a name that spins in), with the characters in
        char_map replaced. Returns the new label's frame index."""
        labels = self.labels_of(sprite)
        if new_label in labels:
            return labels[new_label]
        start, end = self.label_span(sprite, ref_label)
        index = max([f.index for f in sprite.frames + sprite.keyframes] + [-1]) + 1
        next_pid = self.next_placement_id()
        pid_map = {}            # new objects (the replaced characters) get new placement ids; shared ones keep theirs
        refs = [(False, f) for f in sprite.frames if start <= f.index < end]
        key = sprite.frame(start, True)
        if key is not None:
            refs.append((True, key))
        for is_key, ref in refs:
            ch = [_copy_child(c) for c in ref.children]
            for c in ch:
                if isinstance(c, Place) and c.char in char_map:
                    c.char = char_map[c.char]
                    if c.pid != NO_ID:
                        if c.pid not in pid_map:
                            pid_map[c.pid] = next_pid
                            next_pid += 1
                        c.pid = pid_map[c.pid]
            (sprite.keyframes if is_key else sprite.frames).append(Frame(index + ref.index - start, ch, is_key))
        sprite.labels.append(Label(self.symbol(new_label), index))
        return index


def copy_lmb(lm):
    return copy.deepcopy(lm)
