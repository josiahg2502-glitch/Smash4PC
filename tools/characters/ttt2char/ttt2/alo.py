"""TTT2's effect files (ALO, magic "OELA"): read into a structure, write back, recolour and retexture.

An ALO is a big-endian memory image of the effect library's ("ale") structures; pointers are byte offsets that the
title's loader turns into addresses by adding the file's address, the string pool's, or the address of the block
holding them. Layout, as the loader walks it (offsets in bytes):

    root (720): "OELA", u32 version 10, four tracks (+8/+20/+32 vector, +44 scalar), +56 emitters (count u8 +124),
    +96 texture names and +100 model names (8 bytes each: u32, u32 pool offset; counts u8 +126/+127), +104 the
    string pool ("AL_EFFECT\\0" and the names), +200 = 1.
    track: u32 offset, u16 key count, 6 more bytes; a key is a u16 frame, 2 bytes, then f32 values: vector keys
    (16 bytes: x, y, z), pair keys (12), scalar keys (8); short keys are 4 bytes (u16 frame, s16 value).
    emitter (88): +0 links that the emitter itself starts ("children", count u16 +74), +4 links that each of its
    particles starts ("links", count +72; a projectile's invisible mover carries the visible emitters this way),
    +8 12-byte records (count +76), +12 parameters (16 bytes: u32 offset of its data, kind in the low four bits of
    byte +12; count +78), +16 the spawner block, +24 the particle block, +52 a pool offset. (+20/+28 hold leftover
    offsets the loader swaps but does not relocate; the writer copies them unchanged.)
    link (16): u32 offset of the emitter it starts, u16 +4 repeat interval in frames (+6 random range), u16 +8
    delay in frames (+10 random range), u8 +12 mode (bits: 0x01 start when the parent ends; 0x04 start once after
    the delay, clear = start again every repeat interval (0: every frame); 0x10 attach the started emitter to the
    parent, so that it follows it).
    spawner (576; its offsets count from its start): tracks +0 (vector), +24, +48/+60/+72, +84/+96/+108 (scalar);
    +40 texture layers (16 bytes, count u16 +248); +44 modifiers (8 bytes, count u8 +251).
    texture layer: kind u8 +12; kind 0: +4 -> 8 bytes {f32, s16, s16}; other kinds: +0 -> animation (16 bytes:
    +0 frames (24 bytes: u16, f32, f32, pool offset of a texture name +12, u16 +20; count u16 +10), +4 names
    (8 bytes, pool offset; count u16 +12)); +8 -> texture slots (72 bytes: pool offset of the texture's name,
    pair tracks +8/+20, f32 +32..+60, s16 +64/+66 the texture's width and height in pixels (256 when unused; the
    sprite code measures its UV rectangle in these pixels, so mostly the aspect matters, and some game files keep
    the size the texture was authored at); count u8 +13; slot 0 the colour texture, slot 1 e.g. a normal map for
    distortion).
    modifier: offset of its data, kind u8 +4 (1: scalar track, f32 (9.8 = gravity), ...; 2: scalar track;
    3: scalar and vector tracks; 4: five f32).
    particle (1280; offsets count from its start): +0 shape data by kind u8 +175 (1: two f32; 2: four scalar
    tracks, two f32; 3: two scalar tracks, four f32; 4: six f32), scalar tracks +4/+16/+28/+40, short track +52,
    vector tracks +64/+88/+112 (each followed by a vector), f32 +136..+164, u32 +168 flags (0x80: started only
    by a link, not with the effect), and an 808-byte block of plain values at +244.
    The string pool runs to the end of the file.

Every fixed-size record is kept whole (`raw`); the writer sets the offsets and counts from the structure. Read and
written back, a file is byte-identical: each block keeps its place (`at`). A block whose size changes, or a new
one, has no `at` (the editing helpers drop it); `write` then appends it (and moves the string pool after it, so
that the pool stays last), or lays the whole file out afresh when the old places no longer fit (e.g. after adding
an emitter).

Written for this project from the game's own loader and files (no third-party code).
"""
import copy
import struct

MAGIC = b'OELA'
VERSION = 10
POOL_HEAD = b'AL_EFFECT\0'

ROOT_SIZE, NODE_SIZE, SPAWNER_SIZE, PARTICLE_SIZE = 720, 88, 576, 1280
LAYER_SIZE, UV_SIZE, FRAME_SIZE, NAME_SIZE, ANIM_SIZE = 16, 72, 24, 8, 16
MODIFIER_SIZE, PARAM_SIZE, LINK_SIZE, RECORD_SIZE, REF_SIZE = 8, 16, 16, 12, 8
ALIGN = 16

KEYS = {'vec': (16, 3), 'pair': (12, 2), 'scalar': (8, 1), 'short': (4, 1)}      # bytes, values

ROOT_TRACKS = {8: 'vec', 20: 'vec', 32: 'vec', 44: 'scalar'}
SPAWNER_TRACKS = {0: 'vec', 24: 'scalar', 48: 'scalar', 60: 'scalar', 72: 'scalar', 84: 'scalar', 96: 'scalar',
                  108: 'scalar'}
PARTICLE_TRACKS = {4: 'scalar', 16: 'scalar', 28: 'scalar', 40: 'scalar', 52: 'short', 64: 'vec', 88: 'vec',
                   112: 'vec'}
SHAPES = {1: (8, {}), 2: (56, {0: 'scalar', 12: 'scalar', 24: 'scalar', 36: 'scalar'}),
          3: (40, {0: 'scalar', 12: 'scalar'}), 4: (24, {})}
MODIFIERS = {1: (32, {0: 'scalar'}), 2: (12, {0: 'scalar'}), 3: (24, {0: 'scalar', 12: 'vec'}), 4: (20, {})}
UV_TRACKS = {8: 'pair', 20: 'pair'}
PARAM_DATA = {1: 8, 2: 12, 3: 12}          # other kinds: 24 bytes


def _u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def _u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def _put16(b, o, v):
    struct.pack_into('>H', b, o, v)


def _put32(b, o, v):
    struct.pack_into('>I', b, o, v & 0xFFFFFFFF)


# ------------------------------------------------------------------------------------------------------- reading
class _Reader:
    def __init__(self, data):
        if data[:4] != MAGIC:
            raise ValueError('not an ALO (magic %r)' % bytes(data[:4]))
        if _u32(data, 4) != VERSION:
            raise ValueError('ALO version %d (only %d is known)' % (_u32(data, 4), VERSION))
        self.d = bytes(data)
        self.owned = bytearray(len(data))
        self.pool = _u32(data, 104)

    def take(self, at, size):
        if at < 0 or at + size > len(self.d):
            raise ValueError('ALO block at 0x%x+%d outside the file' % (at, size))
        self.owned[at:at + size] = b'\1' * size
        return self.d[at:at + size]

    def name(self, off):
        if not off:
            return None
        at = self.pool + off
        return self.d[at:self.d.index(b'\0', at)].decode('latin1')

    def track(self, ref_at, base, kind):
        """The track whose 12-byte reference is at ref_at (its keys at base + offset)."""
        off, count = _u32(self.d, ref_at), _u16(self.d, ref_at + 4)
        ksize, nval = KEYS[kind]
        t = {'kind': kind, 'flags': self.d[ref_at + 6:ref_at + 12], 'keys': []}
        if not off:
            if count:
                raise ValueError('ALO track with keys at offset 0')
            return t
        t['at'] = base + off
        raw = self.take(t['at'], ksize * count)
        pads = []
        for k in range(count):
            o = k * ksize
            if kind == 'short':
                t['keys'].append([_u16(raw, o), struct.unpack_from('>h', raw, o + 2)[0]])
            else:
                t['keys'].append([_u16(raw, o)] + list(struct.unpack_from('>%df' % nval, raw, o + 4)))
                pads.append(_u16(raw, o + 2))
        if any(pads):
            t['pads'] = pads
        return t

    def tracks(self, ref_base, base, table):
        return {o: self.track(ref_base + o, base, kind) for o, kind in table.items()}

    def items(self, at, count, size):
        """[(position, raw)] of a list of fixed-size records."""
        if not at or not count:
            return []
        raw = self.take(at, size * count)
        return [(at + k * size, raw[k * size:(k + 1) * size]) for k in range(count)]


def read(data):
    """An ALO's bytes -> its structure (a dict; see the module notes)."""
    r = _Reader(data)
    d = r.d
    doc = {'root': {'raw': r.take(0, ROOT_SIZE), 'tracks': r.tracks(0, 0, ROOT_TRACKS)}}
    doc['textures'] = [{'raw': raw, 'name': r.name(_u32(raw, 4))} for _, raw in r.items(_u32(d, 96), d[126], REF_SIZE)]
    doc['models'] = [{'raw': raw, 'name': r.name(_u32(raw, 4))} for _, raw in r.items(_u32(d, 100), d[127], REF_SIZE)]
    doc['textures_at'], doc['models_at'] = _u32(d, 96), _u32(d, 100)
    nodes_at, count = _u32(d, 56), d[124]
    doc['emitters_at'] = nodes_at
    index = {nodes_at + NODE_SIZE * i: i for i in range(count)}
    doc['emitters'] = [_emitter(r, nodes_at + NODE_SIZE * i, index) for i in range(count)]
    doc['pool'] = r.take(r.pool, len(d) - r.pool)
    doc['pool_at'] = r.pool
    if not doc['pool'].startswith(POOL_HEAD):
        raise ValueError('ALO string pool does not start with AL_EFFECT')
    doc['extra'] = _extra(d, r.owned)
    return doc


def _emitter(r, at, index):
    node = r.take(at, NODE_SIZE)
    e = {'raw': node, 'name': r.name(_u32(node, 52))}
    for key, off, cnt in (('children', 0, 74), ('links', 4, 72)):
        e[key + '_at'] = _u32(node, off)
        e[key] = [{'raw': raw, 'emitter': index.get(_u32(raw, 0))} for _, raw in
                  r.items(_u32(node, off), _u16(node, cnt), LINK_SIZE)]
    e['records_at'] = _u32(node, 8)
    e['records'] = [raw for _, raw in r.items(_u32(node, 8), _u16(node, 76), RECORD_SIZE)]
    e['params_at'] = _u32(node, 12)
    e['params'] = []
    for _, raw in r.items(_u32(node, 12), _u16(node, 78), PARAM_SIZE):
        p = {'raw': raw}
        if _u32(raw, 0):
            p['data_at'] = _u32(raw, 0)
            p['data'] = r.take(p['data_at'], PARAM_DATA.get(raw[12] & 15, 24))
            if raw[12] & 15 == 1:
                p['name'] = r.name(_u32(p['data'], 0))
        e['params'].append(p)
    e['spawner'] = _spawner(r, _u32(node, 16))
    e['particle'] = _particle(r, _u32(node, 24))
    return e


def _rel(base, off):
    return base + off if off else 0


def _spawner(r, at):
    raw = r.take(at, SPAWNER_SIZE)
    s = {'raw': raw, 'at': at, 'tracks': r.tracks(at, at, SPAWNER_TRACKS)}
    s['layers_at'] = _rel(at, _u32(raw, 40))
    s['layers'] = [_layer(r, at, pos, lay) for pos, lay in r.items(s['layers_at'], _u16(raw, 248), LAYER_SIZE)]
    s['modifiers_at'] = _rel(at, _u32(raw, 44))
    s['modifiers'] = []
    for _, m in r.items(s['modifiers_at'], raw[251], MODIFIER_SIZE):
        mod = {'raw': m}
        size, table = MODIFIERS.get(m[4], (0, {}))
        if _u32(m, 0) and size:
            mod['data_at'] = at + _u32(m, 0)
            mod['data'] = r.take(mod['data_at'], size)
            mod['tracks'] = r.tracks(mod['data_at'], at, table)
        s['modifiers'].append(mod)
    return s


def _layer(r, base, pos, raw):
    lay = {'raw': raw}
    if raw[12] == 0:
        if _u32(raw, 4):
            lay['still_at'] = base + _u32(raw, 4)
            lay['still'] = r.take(lay['still_at'], 8)
    elif _u32(raw, 0):
        at = base + _u32(raw, 0)
        head = r.take(at, ANIM_SIZE)
        anim = {'raw': head, 'at': at, 'frames_at': _rel(base, _u32(head, 0)), 'names_at': _rel(base, _u32(head, 4))}
        anim['frames'] = [{'raw': f, 'texture': r.name(_u32(f, 12))}
                          for _, f in r.items(anim['frames_at'], _u16(head, 10), FRAME_SIZE)]
        anim['names'] = [{'raw': n, 'name': r.name(_u32(n, 0))}
                         for _, n in r.items(anim['names_at'], _u16(head, 12), NAME_SIZE)]
        lay['anim'] = anim
    lay['uvs_at'] = _rel(base, _u32(raw, 8))
    lay['uvs'] = [{'raw': u, 'name': r.name(_u32(u, 0)), 'tracks': r.tracks(p, base, UV_TRACKS)}
                  for p, u in r.items(lay['uvs_at'], raw[13], UV_SIZE)]
    return lay


def _particle(r, at):
    raw = r.take(at, PARTICLE_SIZE)
    p = {'raw': raw, 'at': at, 'tracks': r.tracks(at, at, PARTICLE_TRACKS)}
    size, table = SHAPES.get(raw[175], (0, {}))
    if raw[175] and _u32(raw, 0) and size:
        dat = at + _u32(raw, 0)
        p['shape'] = {'raw': r.take(dat, size), 'at': dat, 'tracks': r.tracks(dat, at, table)}
    return p


def _extra(d, owned):
    """Runs of non-zero bytes that no block owns: [(offset, bytes)]."""
    out, k, n = [], 0, len(d)
    while k < n:
        if owned[k] or not d[k]:
            k += 1
            continue
        s = k
        while k < n and not owned[k] and d[k]:
            k += 1
        out.append((s, d[s:k]))
    return out


# ------------------------------------------------------------------------------------------------------- writing
class _Block:
    __slots__ = ('size', 'want', 'pos', 'fill')

    def __init__(self, size, want, fill):
        self.size, self.want, self.pos, self.fill = size, want, None, fill


class _Writer:
    def __init__(self, doc):
        self.doc = doc
        self.blocks = []
        self.pool = bytearray(doc.get('pool') or POOL_HEAD)
        if not self.pool.startswith(POOL_HEAD):
            raise ValueError('ALO string pool must start with AL_EFFECT')

    def block(self, size, want, fill):
        b = _Block(size, want, fill)
        self.blocks.append(b)
        return b

    def name(self, name, old=0):
        """Pool offset of a name (None -> 0), reusing its old offset when it still holds it."""
        if name is None:
            return 0
        s = name.encode('latin1')
        if old and self.pool[old:old + len(s) + 1] == s + b'\0' and self.pool[old - 1] == 0:
            return old
        k = self.pool.find(b'\0' + s + b'\0')
        if k >= 0:
            return k + 1
        at = len(self.pool)
        self.pool += s + b'\0'
        return at

    # tracks: a block for the keys (none when empty), the 12-byte reference written by the holder
    def track(self, t):
        keys = t['keys']
        if not keys and 'at' not in t:
            return None
        ksize, nval = KEYS[t['kind']]

        def fill(out, pos):
            pads = t.get('pads') or [0] * len(keys)
            for k, key in enumerate(keys):
                o = pos + k * ksize
                if t['kind'] == 'short':
                    struct.pack_into('>Hh', out, o, int(key[0]), int(key[1]))
                else:
                    struct.pack_into('>HH%df' % nval, out, o, int(key[0]), pads[k], *key[1:1 + nval])
        return self.block(ksize * len(keys), t.get('at'), fill)

    def tracks(self, table):
        return {o: (t, self.track(t)) for o, t in table.items()}

    @staticmethod
    def put_tracks(out, at, base, refs):
        for o, (t, blk) in refs.items():
            _put32(out, at + o, (blk.pos - base) if blk is not None else 0)
            _put16(out, at + o + 4, len(t['keys']))
            out[at + o + 6:at + o + 12] = t['flags']


def write(doc):
    """The ALO bytes of a structure from `read` (or built with this module's helpers)."""
    w = _Writer(doc)
    doc_root = doc['root']
    emitters = doc['emitters']

    w.block(ROOT_SIZE, 0, None)                 # the root record: always first
    nodes = w.block(NODE_SIZE * len(emitters), doc.get('emitters_at'), None)
    plans = [_plan_emitter(w, e) for e in emitters]
    root_tracks = w.tracks(doc_root['tracks'])
    tex = w.block(REF_SIZE * len(doc['textures']), doc.get('textures_at') or None, None)
    mdl = w.block(REF_SIZE * len(doc['models']), doc.get('models_at') or None, None)
    tex_offs = [w.name(t['name'], _u32(t['raw'], 4)) for t in doc['textures']]
    mdl_offs = [w.name(m['name'], _u32(m['raw'], 4)) for m in doc['models']]
    for plan in plans:                 # names inside emitters
        plan['names'](w)
    pool = w.block(len(w.pool), doc.get('pool_at'), None)      # created last: laid out last

    size = _place(w.blocks, pool)
    out = bytearray(size)
    if all(b.pos == b.want for b in w.blocks if b.size):
        for at, b in doc.get('extra', ()):
            out[at:at + len(b)] = b

    # root
    out[0:ROOT_SIZE] = doc_root['raw']
    out[0:4] = MAGIC
    _put32(out, 4, VERSION)
    _Writer.put_tracks(out, 0, 0, root_tracks)
    _put32(out, 56, nodes.pos if emitters else 0)
    out[124] = len(emitters)
    _put32(out, 96, tex.pos if doc['textures'] or doc.get('textures_at') else 0)
    out[126] = len(doc['textures'])
    _put32(out, 100, mdl.pos if doc['models'] or doc.get('models_at') else 0)
    out[127] = len(doc['models'])
    _put32(out, 104, pool.pos)
    for k, t in enumerate(doc['textures']):
        o = tex.pos + REF_SIZE * k
        out[o:o + REF_SIZE] = t['raw']
        _put32(out, o + 4, tex_offs[k])
    for k, m in enumerate(doc['models']):
        o = mdl.pos + REF_SIZE * k
        out[o:o + REF_SIZE] = m['raw']
        _put32(out, o + 4, mdl_offs[k])
    for k, plan in enumerate(plans):
        plan['fill'](out, nodes.pos + NODE_SIZE * k, nodes.pos)
    for b in w.blocks:
        if b.fill:
            b.fill(out, b.pos)
    out[pool.pos:pool.pos + len(w.pool)] = w.pool
    return bytes(out)


def _place(blocks, last):
    """Positions for the blocks: their old places when all of them still fit, else a fresh sequential layout.
    `last` (the string pool, which a reader takes to run to the end of the file) always ends the file: when other
    blocks need new places, it moves after them."""
    if last.want is not None and any(b.want is None and b.size for b in blocks if b is not last):
        last.want = None
    pinned = sorted((b for b in blocks if b.want is not None), key=lambda b: (b.want, b.size))
    ok, end = True, 0
    for b in pinned:
        if b.want < end and b.size:
            ok = False
            break
        end = max(end, b.want + b.size)
    if ok:
        for b in pinned:
            b.pos = b.want
        cur = _align(end)
        for b in blocks:
            if b.pos is None:
                b.pos = cur
                cur = _align(cur + b.size) if b.size else cur
        return max([end] + [b.pos + b.size for b in blocks if b.size])
    cur = 0
    for b in blocks:
        b.pos = cur
        if b.size:
            cur = _align(cur + b.size)
    return max(b.pos + b.size for b in blocks if b.size)


def _align(x):
    return (x + ALIGN - 1) // ALIGN * ALIGN


def _plan_emitter(w, e):
    """Blocks of one emitter; -> {'fill': fn(out, node_pos, nodes_pos), 'names': fn(w)}."""
    s, p = e['spawner'], e['particle']
    plan = {}
    # spawner and its parts (offsets relative to the spawner)
    sp = w.block(SPAWNER_SIZE, s.get('at'), None)
    sp_tracks = w.tracks(s['tracks'])
    layers = w.block(LAYER_SIZE * len(s['layers']), s.get('layers_at') or None, None)
    lay_plans = []
    for lay in s['layers']:
        lp = {'lay': lay}
        if 'still' in lay:
            lp['still'] = w.block(8, lay.get('still_at'), None)
        if 'anim' in lay:
            a = lay['anim']
            lp['anim'] = w.block(ANIM_SIZE, a.get('at'), None)
            lp['frames'] = w.block(FRAME_SIZE * len(a['frames']), a.get('frames_at') or None, None)
            lp['names'] = w.block(NAME_SIZE * len(a['names']), a.get('names_at') or None, None)
        lp['uvs'] = w.block(UV_SIZE * len(lay['uvs']), lay.get('uvs_at') or None, None)
        lp['uv_tracks'] = [w.tracks(u['tracks']) for u in lay['uvs']]
        lay_plans.append(lp)
    mods = w.block(MODIFIER_SIZE * len(s['modifiers']), s.get('modifiers_at') or None, None)
    mod_plans = []
    for m in s['modifiers']:
        mp = {}
        if 'data' in m:
            mp['data'] = w.block(len(m['data']), m.get('data_at'), None)
            mp['tracks'] = w.tracks(m.get('tracks', {}))
        mod_plans.append(mp)
    # particle
    pa = w.block(PARTICLE_SIZE, p.get('at'), None)
    pa_tracks = w.tracks(p['tracks'])
    shape = shape_tracks = None
    if 'shape' in p:
        shape = w.block(len(p['shape']['raw']), p['shape'].get('at'), None)
        shape_tracks = w.tracks(p['shape']['tracks'])
    # node lists
    children = w.block(LINK_SIZE * len(e['children']), e.get('children_at') or None, None)
    links = w.block(LINK_SIZE * len(e['links']), e.get('links_at') or None, None)
    records = w.block(RECORD_SIZE * len(e['records']), e.get('records_at') or None, None)
    params = w.block(PARAM_SIZE * len(e['params']), e.get('params_at') or None, None)
    pdata = [w.block(len(q['data']), q.get('data_at'), None) if 'data' in q else None for q in e['params']]
    names = {}

    def collect_names(w):
        names['node'] = w.name(e.get('name'), _u32(e['raw'], 52))
        names['params'] = [w.name(q.get('name'), _u32(q['data'], 0)) if 'data' in q and (q['raw'][12] & 15) == 1
                           else None for q in e['params']]
        names['layers'] = []
        for lay in s['layers']:
            ln = {'frames': [], 'names': []}
            if 'anim' in lay:
                ln['frames'] = [w.name(f['texture'], _u32(f['raw'], 12)) for f in lay['anim']['frames']]
                ln['names'] = [w.name(n['name'], _u32(n['raw'], 0)) for n in lay['anim']['names']]
            ln['uvs'] = [w.name(u['name'], _u32(u['raw'], 0)) for u in lay['uvs']]
            names['layers'].append(ln)

    def fill(out, at, nodes_pos):
        node = bytearray(e['raw'])
        _put32(node, 0, children.pos if e['children'] else 0)
        _put16(node, 74, len(e['children']))
        _put32(node, 4, links.pos if e['links'] else 0)
        _put16(node, 72, len(e['links']))
        _put32(node, 8, records.pos if e['records'] else 0)
        _put16(node, 76, len(e['records']))
        _put32(node, 12, params.pos if e['params'] else 0)
        _put16(node, 78, len(e['params']))
        _put32(node, 16, sp.pos)
        _put32(node, 24, pa.pos)
        _put32(node, 52, names['node'])
        if not e['children'] and e.get('children_at'):
            _put32(node, 0, children.pos)
        if not e['links'] and e.get('links_at'):
            _put32(node, 4, links.pos)
        if not e['records'] and e.get('records_at'):
            _put32(node, 8, records.pos)
        if not e['params'] and e.get('params_at'):
            _put32(node, 12, params.pos)
        out[at:at + NODE_SIZE] = node
        for key, blk in (('children', children), ('links', links)):
            for k, it in enumerate(e[key]):
                o = blk.pos + LINK_SIZE * k
                out[o:o + LINK_SIZE] = it['raw']
                if it['emitter'] is not None:
                    _put32(out, o, nodes_pos + NODE_SIZE * it['emitter'])
        for k, rec in enumerate(e['records']):
            o = records.pos + RECORD_SIZE * k
            out[o:o + RECORD_SIZE] = rec
        for k, q in enumerate(e['params']):
            o = params.pos + PARAM_SIZE * k
            out[o:o + PARAM_SIZE] = q['raw']
            if pdata[k] is not None:
                _put32(out, o, pdata[k].pos)
                out[pdata[k].pos:pdata[k].pos + len(q['data'])] = q['data']
                if names['params'][k] is not None:
                    _put32(out, pdata[k].pos, names['params'][k])
        # spawner
        a = sp.pos
        out[a:a + SPAWNER_SIZE] = s['raw']
        _Writer.put_tracks(out, a, a, sp_tracks)
        if s['layers'] or s.get('layers_at'):
            _put32(out, a + 40, layers.pos - a)
        _put16(out, a + 248, len(s['layers']))
        if s['modifiers'] or s.get('modifiers_at'):
            _put32(out, a + 44, mods.pos - a)
        out[a + 251] = len(s['modifiers'])
        for k, lp in enumerate(lay_plans):
            lay = lp['lay']
            o = layers.pos + LAYER_SIZE * k
            raw = bytearray(lay['raw'])
            if 'still' in lp:
                _put32(raw, 4, lp['still'].pos - a)
                out[lp['still'].pos:lp['still'].pos + 8] = lay['still']
            if 'anim' in lp:
                an = lay['anim']
                _put32(raw, 0, lp['anim'].pos - a)
                head = bytearray(an['raw'])
                _put32(head, 0, (lp['frames'].pos - a) if (an['frames'] or an.get('frames_at')) else 0)
                _put16(head, 10, len(an['frames']))
                _put32(head, 4, (lp['names'].pos - a) if (an['names'] or an.get('names_at')) else 0)
                _put16(head, 12, len(an['names']))
                out[lp['anim'].pos:lp['anim'].pos + ANIM_SIZE] = head
                for j, f in enumerate(an['frames']):
                    fo = lp['frames'].pos + FRAME_SIZE * j
                    out[fo:fo + FRAME_SIZE] = f['raw']
                    _put32(out, fo + 12, names['layers'][k]['frames'][j])
                for j, n in enumerate(an['names']):
                    no = lp['names'].pos + NAME_SIZE * j
                    out[no:no + NAME_SIZE] = n['raw']
                    _put32(out, no, names['layers'][k]['names'][j])
            if lay['uvs'] or lay.get('uvs_at'):
                _put32(raw, 8, lp['uvs'].pos - a)
            raw[13] = len(lay['uvs'])
            out[o:o + LAYER_SIZE] = raw
            for j, u in enumerate(lay['uvs']):
                uo = lp['uvs'].pos + UV_SIZE * j
                out[uo:uo + UV_SIZE] = u['raw']
                _put32(out, uo, names['layers'][k]['uvs'][j])
                _Writer.put_tracks(out, uo, a, lp['uv_tracks'][j])
        for k, (m, mp) in enumerate(zip(s['modifiers'], mod_plans)):
            o = mods.pos + MODIFIER_SIZE * k
            raw = bytearray(m['raw'])
            if 'data' in mp:
                _put32(raw, 0, mp['data'].pos - a)
                out[mp['data'].pos:mp['data'].pos + len(m['data'])] = m['data']
                _Writer.put_tracks(out, mp['data'].pos, a, mp['tracks'])
            out[o:o + MODIFIER_SIZE] = raw
        # particle
        c = pa.pos
        out[c:c + PARTICLE_SIZE] = p['raw']
        _Writer.put_tracks(out, c, c, pa_tracks)
        if shape is not None:
            _put32(out, c, shape.pos - c)
            out[shape.pos:shape.pos + len(p['shape']['raw'])] = p['shape']['raw']
            _Writer.put_tracks(out, shape.pos, c, shape_tracks)

    plan['fill'] = fill
    plan['names'] = collect_names
    return plan


# ------------------------------------------------------------------------------------------------------- editing
_PLACES = ('at', 'emitters_at', 'textures_at', 'models_at', 'pool_at', 'children_at', 'links_at', 'records_at',
           'params_at', 'data_at', 'layers_at', 'modifiers_at', 'still_at', 'frames_at', 'names_at', 'uvs_at')


def unpin(obj):
    """Drops every remembered position (and the leftover bytes) so that `write` lays the file out afresh."""
    if isinstance(obj, dict):
        for k in _PLACES:
            obj.pop(k, None)
        obj.pop('extra', None)
        for v in obj.values():
            unpin(v)
    elif isinstance(obj, list):
        for v in obj:
            unpin(v)
    return obj


# Named fields (offsets into an emitter's records). Found by comparing the game's own variants (the aura colours
# cmn/alo/eff_aura01{b,g,r,v,...}, its fire effects), by reading the title's particle code and by changing them in
# game runs (particles counted); "[guess]" marks meanings not yet proven.
NODE_COLOUR = 36          # emitter: f32 x3, RGB scale (1 = as is; the game's coloured auras use 2..4)
INTERVAL = 60             # emitter: u16 frames between emissions (+2: random range)
DURATION = 64             # emitter: u16 frames it emits for (+2: random range); a started emitter can outlive
                          # the particle that started it
DELAY = 68                # emitter: u16 frames before it starts (+2: random range) [guess]
COLOUR_TRACK = 0          # spawner track (vector keys): RGB
ALPHA_TRACK = 24          # spawner track (scalar keys): opacity over a particle's life
SIZE_TRACK = 48           # spawner track (scalar keys): size factor over a particle's life [guess]
VELOCITY_TRACKS = (84, 96, 108)   # spawner tracks (scalar keys): velocity x, y, z [guess: axes and units]
SIZE = 148                # spawner: f32 x2, particle width, height (the game's units; its sprites use 250..1500);
                          # +8 / +12: their random ranges
PARTICLE_FLAGS = 136      # spawner: u32 flags (0x10000: one random for width and height)
BLEND = 239               # spawner: byte; 0x80 additive, 0x40 alpha-blended [guess: the game's smoke uses both]
LIFE = 244                # spawner: u16 particle life in frames (60 per second); +2: random range
PARTICLE_TYPE = 250       # spawner: u8; 2 sprite, 3 model, 4 strip, 5 none (a projectile's invisible mover)
COUNT_TRACK = 52          # particle block track (short keys): particles per emission; u16 +172: random range
START_FLAGS = 168         # particle block: u32 flags
CHILD_ONLY = 0x80         # START_FLAGS bit: started only by a link; clear: starts with the effect (a "root")
SHAPE = 175               # particle block: u8 emission shape (0 point, 2 sphere: angle tracks and radius, ...)


def emitter_colour(e):
    return list(struct.unpack_from('>3f', e['raw'], NODE_COLOUR))


def set_emitter_colour(e, rgb):
    raw = bytearray(e['raw'])
    struct.pack_into('>3f', raw, NODE_COLOUR, *rgb)
    e['raw'] = bytes(raw)


def _set_keys(track, keys):
    if len(keys) != len(track['keys']):
        track.pop('at', None)
        track.pop('pads', None)
    track['keys'] = [list(k) for k in keys]


def recolour(doc, scale=None, tint=None, emitters=None):
    """Recolours emitters (all, or the given indices): `scale` sets their RGB scale, `tint` (r, g, b) replaces the
    colour of every key of their colour track."""
    for i, e in enumerate(doc['emitters']):
        if emitters is not None and i not in emitters:
            continue
        if scale is not None:
            set_emitter_colour(e, scale)
        if tint is not None:
            t = e['spawner']['tracks'][COLOUR_TRACK]
            _set_keys(t, [[k[0]] + list(tint) for k in (t['keys'] or [[0, 1, 1, 1]])])
    return doc


def texture_names(doc):
    """Every texture file the effect names (in order of first use)."""
    seen = []
    for name in _texture_refs(doc):
        if name and name not in seen:
            seen.append(name)
    return seen


def _texture_refs(doc):
    for t in doc['textures']:
        yield t['name']
    for e in doc['emitters']:
        for lay in e['spawner']['layers']:
            for f in lay.get('anim', {}).get('frames', ()):
                yield f['texture']
            for n in lay.get('anim', {}).get('names', ()):
                yield n['name']
            for u in lay['uvs']:
                yield u['name']


def retexture(doc, names, sizes=None):
    """Renames texture files ({old name: new name}) everywhere the effect names them (the string pool is then
    rebuilt, so that it names only what is used); sizes: {new name: (width, height)} for the texture slots."""
    if any(n in names for n in texture_names(doc)):
        doc.pop('pool', None)
        doc.pop('pool_at', None)
    for t in doc['textures']:
        t['name'] = names.get(t['name'], t['name'])
    for e in doc['emitters']:
        for lay in e['spawner']['layers']:
            a = lay.get('anim')
            if a:
                for f in a['frames']:
                    f['texture'] = names.get(f['texture'], f['texture'])
                for n in a['names']:
                    n['name'] = names.get(n['name'], n['name'])
            for u in lay['uvs']:
                u['name'] = names.get(u['name'], u['name'])
                if sizes and u['name'] in sizes:
                    raw = bytearray(u['raw'])
                    struct.pack_into('>hh', raw, 64, *sizes[u['name']])
                    u['raw'] = bytes(raw)
    return doc


# ------------------------------------------------------------------------------------------------------- building
def _track(kind, keys=()):
    return {'kind': kind, 'flags': bytes(6), 'keys': [list(k) for k in keys]}


def new():
    """An effect with no emitters (root values as the game's own files have them)."""
    raw = bytearray(ROOT_SIZE)
    raw[0:4] = MAGIC
    _put32(raw, 4, VERSION)
    struct.pack_into('>ffI', raw, 60, 10000.0, 1000.0, 0xFF808080)
    struct.pack_into('>ff', raw, 72, -0.5, -0.5)
    _put32(raw, 84, 0x00FFFFFF)
    _put32(raw, 92, 0x10)
    _put32(raw, 200, 1)
    tracks = {8: _track('vec', [[0, 0, 0, 0]]), 20: _track('vec', [[0, 0, 0, 0]]), 32: _track('vec', [[0, 1, 1, 1]]),
              44: _track('scalar', [[0, 1]])}
    return {'root': {'raw': bytes(raw), 'tracks': tracks}, 'textures': [], 'models': [], 'emitters': [],
            'pool': POOL_HEAD, 'extra': []}


def new_emitter():
    """An emitter with every value zero and every track one key (colour, alpha and size 1; others 0): a starting
    point for tests; real effects are better made from a game emitter (`clone_emitter`)."""
    sp = {'raw': bytes(SPAWNER_SIZE), 'tracks': {o: _track(k, [[0, 0, 0, 0]] if k == 'vec' else [[0, 0]])
                                                  for o, k in SPAWNER_TRACKS.items()}, 'layers': [], 'modifiers': []}
    sp['tracks'][COLOUR_TRACK]['keys'] = [[0, 1, 1, 1]]
    sp['tracks'][ALPHA_TRACK]['keys'] = [[0, 1]]
    sp['tracks'][SIZE_TRACK]['keys'] = [[0, 1]]
    pa = {'raw': bytes(PARTICLE_SIZE), 'tracks': {o: _track(k, [[0, 0, 0, 0]] if k == 'vec' else [[0, 0]])
                                                   for o, k in PARTICLE_TRACKS.items()}}
    pa['tracks'][52]['keys'] = [[0, 1]]
    pa['tracks'][112]['keys'] = [[0, 1, 1, 1]]
    node = bytearray(NODE_SIZE)
    struct.pack_into('>4f', node, 32, 500.0, 1.0, 1.0, 1.0)
    return {'raw': bytes(node), 'name': None, 'children': [], 'links': [], 'records': [], 'params': [],
            'spawner': sp, 'particle': pa}


def is_root(e):
    """Whether an emitter starts with the effect (else only a link starts it)."""
    return not _u32(e['particle']['raw'], START_FLAGS) & CHILD_ONLY


def set_root(e, root=True):
    raw = bytearray(e['particle']['raw'])
    flags = _u32(raw, START_FLAGS)
    _put32(raw, START_FLAGS, (flags & ~CHILD_ONLY) if root else (flags | CHILD_ONLY))
    e['particle']['raw'] = bytes(raw)


def roots(doc):
    """Indices of the emitters that start with the effect."""
    return [i for i, e in enumerate(doc['emitters']) if is_root(e)]


def clone_emitter(doc, index, keep_links=False, root=None):
    """A copy of one of an effect's emitters, ready to add to another effect (its links to other emitters are
    dropped unless keep_links, which keeps the indices as they are). root: True makes it start with the effect,
    False only by a link (`link` does that), None keeps the original's setting."""
    e = unpin(copy.deepcopy(doc['emitters'][index]))
    if not keep_links:
        e['children'], e['links'] = [], []
    if root is not None:
        set_root(e, root)
    return e


def add_emitter(doc, e):
    """Appends an emitter; -> its index. The file is laid out afresh when written."""
    unpin(doc)
    doc['emitters'].append(e)
    return len(doc['emitters']) - 1


# Link record modes (byte +12), as the title's link update reads them.
LINK_AT_END = 0x01        # start the emitter when the parent (particle or emitter) ends
LINK_ONCE = 0x04          # start it once after the delay; without it: again every repeat interval
LINK_ATTACH = 0x10        # attach it to the parent, which it then follows
LINK_DEFAULT = bytes(12) + b'\x14\0\0\0'      # once, attached: the game's most common record (projectiles)


def link_record(mode=LINK_ONCE | LINK_ATTACH, delay=0, delay_random=0, repeat=0, repeat_random=0):
    """A 16-byte link record (the emitter's offset is filled in by `write`)."""
    raw = bytearray(LINK_SIZE)
    struct.pack_into('>HHHHB', raw, 4, repeat, repeat_random, delay, delay_random, mode)
    return bytes(raw)


def link(doc, parent, child, template=None, per_particle=True):
    """Makes emitter `parent` start emitter `child`: per_particle, each particle of the parent starts it (the game's
    projectiles: an invisible mover emitter whose particle carries the visible ones), else the parent emitter
    itself does. template: a 16-byte link record to copy (default: once, attached). The child is marked to start
    only by its links (else it would also start with the effect)."""
    unpin(doc)
    key = 'links' if per_particle else 'children'
    doc['emitters'][parent][key].append({'raw': bytes(template or LINK_DEFAULT), 'emitter': child})
    set_root(doc['emitters'][child], False)
