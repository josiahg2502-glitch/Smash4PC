"""TTT2 Wii U models: NUD ("NDP3" version 0x0201, big-endian), read and written byte for byte.

    model = read(data)      # dict: see below; vertex blocks are numpy structured arrays
    data = write(model)     # the same bytes for every NUD of the game

model = {'version', 'sphere': (x, y, z, r), 'bone_first', 'bone_last',
         'meshes': [{'name', 'sphere', 'center', 'sort_bias', 'zero', 'boneflag', 'singlebind',
                     'polys': [{'vtype', 'uvtype', 'pflag', 'tail' (12 bytes), 'indices' (u16 triangle strips,
                                0xFFFF between strips), 'verts' (structured array, vertex_dtype), 'materials'}]}]}
material = {'flags', 'zero', 'src', 'dst', 'alpha_test', 'alpha_func', 'ref', 'cull', 'u1', 'u2', 'zoff',
            'textures': [{'id', 'pad4' (6 bytes), 'map', 'wrap_s', 'wrap_t', 'min', 'mag', 'mip', 'b17', 'tail'}],
            'props': [(name, [floats])]}

File layout (as the game's files have it): header (0x30) | mesh records (0x30 each) | polygon records (0x30 each, in
mesh order) | materials (in polygon order) | zeros to 16 | index clump (all strips back to back, padded to 16) |
vertex clump (each polygon's block padded to 16) | vertex-add clump (likewise) | string table (mesh names, then each
material's property names in order; every string NUL-terminated and padded to 16, none shared).
A vertex type's high nibble picks the bone-weight layout (stored in the vertex-add clump with the position and
normals); its low nibble the normal layout. The UV type's high nibble is the number of UV sets, bits 1-3 the colour
layout, bit 0 full-precision UVs.
"""
import struct

import numpy as np

# ------------------------------------------------------------------------------------------------ vertex layouts
_POS = {
    0: [('pos', '>f4', 3), ('pw', '>f4')],
    1: [('pos', '>f4', 3), ('pw', '>f4'), ('nrm', '>f4', 4)],
    2: [('pos', '>f4', 3), ('pw', '>f4'), ('nrm', '>f4', 4), ('bitan', '>f4', 4), ('tan', '>f4', 4)],
    3: [('pos', '>f4', 3), ('pw', '>f4'), ('nrm', '>f4', 4), ('bitan', '>f4', 4), ('tan', '>f4', 4)],
    6: [('pos', '>f4', 3), ('nrm', '>f2', 4)],
    7: [('pos', '>f4', 3), ('nrm', '>f2', 4), ('bitan', '>f2', 4), ('tan', '>f2', 4)],
}
_WGT = {
    0x00: [],
    0x10: [('bones', '>u4', 4), ('w', '>f4', 4)],
    0x20: [('bones', '>u2', 4), ('w', '>f2', 4)],
    0x40: [('bones', 'u1', 4), ('w', 'u1', 4)],
}


def _uv_fields(uvtype):
    nuv, ct, ut = uvtype >> 4, uvtype & 0xE, uvtype & 1
    f = []
    if ct == 2:
        f.append(('color', 'u1', 4))
    elif ct == 4:
        f.append(('color', '>f2', 4))
    elif ct != 0:
        raise ValueError('colour type %d' % ct)
    if nuv:
        f.append(('uv', '>f4' if ut else '>f2', (nuv, 2)))
    return f


def block_dtypes(vtype, uvtype):
    """(dtype of the vertex-clump block, dtype of the vertex-add block or None)."""
    bt, nt = vtype & 0xF0, vtype & 0x0F
    if bt == 0:
        return np.dtype(_POS[nt] + _uv_fields(uvtype)), None
    return np.dtype(_uv_fields(uvtype)), np.dtype(_POS[nt] + _WGT[bt])


def vertex_dtype(vtype, uvtype):
    """One flat structured dtype holding every field of a vertex (what poly['verts'] uses)."""
    a, b = block_dtypes(vtype, uvtype)
    f = [(n, a.fields[n][0]) for n in a.names]
    if b is not None:
        f += [(n, b.fields[n][0]) for n in b.names]
    return np.dtype(f)


def _split(verts, vtype, uvtype):
    a, b = block_dtypes(vtype, uvtype)
    va = np.zeros(len(verts), a)
    for n in a.names:
        va[n] = verts[n]
    if b is None:
        return va, None
    vb = np.zeros(len(verts), b)
    for n in b.names:
        vb[n] = verts[n]
    return va, vb


# ------------------------------------------------------------------------------------------------ reader
def _cstr(d, o):
    return d[o:d.index(b'\0', o)].decode('latin1')


def _read_material(d, off, names):
    flags, z, src, ntex, dst, at, af, ref, cull, u1, u2, zoff = struct.unpack_from('>IIHHHBBHHIII', d, off)
    m = dict(flags=flags, zero=z, src=src, dst=dst, alpha_test=at, alpha_func=af, ref=ref, cull=cull, u1=u1, u2=u2,
             zoff=zoff, textures=[], props=[])
    p = off + 32
    for _ in range(ntex):
        tid, = struct.unpack_from('>I', d, p)
        mp, ws, wt, mn, mg, mip, b17 = struct.unpack_from('>H6B', d, p + 10)
        m['textures'].append(dict(id=tid, pad4=d[p + 4:p + 10], map=mp, wrap_s=ws, wrap_t=wt, min=mn, mag=mg,
                                  mip=mip, b17=b17, tail=d[p + 18:p + 24]))
        p += 24
    while True:
        size, no = struct.unpack_from('>II', d, p)
        cnt = d[p + 11]
        assert d[p + 8:p + 11] == b'\0\0\0' and d[p + 12:p + 16] == b'\0\0\0\0', d[p:p + 16].hex()
        vals = list(struct.unpack_from('>%df' % cnt, d, p + 16))
        # an empty list is stored as one terminator with name offset 0 and no values (no string of its own)
        m['props'].append((None if (no == 0 and cnt == 0 and size == 0) else names(no), vals))
        if size == 0:
            p += 16 + 4 * cnt
            break
        assert size == 16 + 4 * cnt, (size, cnt)
        p += size
    return m, p


def read(d):
    assert d[:4] == b'NDP3', d[:4]
    size, version, nmesh, bfirst, blast = struct.unpack_from('>IHHhH', d, 4)
    desc, isz, vsz, asz = struct.unpack_from('>4I', d, 16)
    idx0 = desc + 0x30
    v0 = idx0 + isz
    a0 = v0 + vsz
    s0 = a0 + asz
    names = lambda o: _cstr(d, s0 + o)
    model = dict(version=version, sphere=struct.unpack_from('>4f', d, 32), bone_first=bfirst, bone_last=blast,
                 meshes=[])
    for k in range(nmesh):
        p = 0x30 + 48 * k
        f = struct.unpack_from('>8f', d, p)
        no, z, bflag, sb, npoly, poff = struct.unpack_from('>IHHhHI', d, p + 32)
        mesh = dict(name=names(no), sphere=f[:4], center=f[4:7], sort_bias=f[7], zero=z, boneflag=bflag,
                    singlebind=sb, polys=[])
        for j in range(npoly):
            q = poff + 48 * j
            io, vo, ao, nv, vt, ut = struct.unpack_from('>3IHBB', d, q)
            mats = [x for x in struct.unpack_from('>4I', d, q + 16)]
            nidx, pflag = struct.unpack_from('>HH', d, q + 32)
            da, db = block_dtypes(vt, ut)
            va = np.frombuffer(d, da, nv, v0 + vo)
            verts = np.zeros(nv, vertex_dtype(vt, ut))
            for n in da.names:
                verts[n] = va[n]
            if db is not None:
                vb = np.frombuffer(d, db, nv, a0 + ao)
                for n in db.names:
                    verts[n] = vb[n]
            assert all(m == 0 for m in mats[len([m for m in mats if m]):]), mats
            poly = dict(vtype=vt, uvtype=ut, pflag=pflag, tail=d[q + 36:q + 48],
                        indices=np.frombuffer(d, '>u2', nidx, idx0 + io).copy(), verts=verts,
                        materials=[_read_material(d, m, names)[0] for m in mats if m],
                        _offs=(io, vo, ao, mats))
            mesh['polys'].append(poly)
        model['meshes'].append(mesh)
    return model


# ------------------------------------------------------------------------------------------------ writer
def _pad(b, a=16):
    while len(b) % a:
        b.append(0)


def _material_bytes(m, addname):
    out = bytearray(struct.pack('>IIHHHBBHHIII', m['flags'], m.get('zero', 0), m['src'], len(m['textures']),
                                m['dst'], m['alpha_test'], m['alpha_func'], m['ref'], m['cull'], m.get('u1', 0),
                                m.get('u2', 0), m.get('zoff', 0)))
    for t in m['textures']:
        out += struct.pack('>I', t['id']) + bytes(t.get('pad4', bytes(6)))
        out += struct.pack('>H6B', t.get('map', 0), t['wrap_s'], t['wrap_t'], t['min'], t['mag'], t['mip'],
                           t.get('b17', 0))
        out += bytes(t.get('tail', bytes(6)))
    props = m['props']
    for k, (name, vals) in enumerate(props):
        last = k == len(props) - 1
        size = 0 if last else 16 + 4 * len(vals)
        out += struct.pack('>II3xB4x', size, 0 if name is None else addname(name), len(vals)) + struct.pack('>%df' % len(vals), *vals)
    return out


def used_bones(model):
    """(first, last) bone index referenced by skinned vertices (all 4 slots), as the header stores it."""
    lo, hi = None, None
    for m in model['meshes']:
        for p in m['polys']:
            if p['vtype'] & 0xF0:
                b = p['verts']['bones']
                lo = int(b.min()) if lo is None else min(lo, int(b.min()))
                hi = int(b.max()) if hi is None else max(hi, int(b.max()))
    return (0, 0) if lo is None else (lo, hi)


def write(model):
    meshes = model['meshes']
    polys = [p for m in meshes for p in m['polys']]
    # string table
    strings = bytearray()

    def addname(s):
        o = len(strings)
        strings.extend(s.encode('latin1') + b'\0')
        _pad(strings)
        return o
    mesh_name_off = [addname(m['name']) for m in meshes]
    # materials (need string offsets in poly order, after the mesh names)
    poly_rec_start = 0x30 + 48 * len(meshes)
    mat_start = poly_rec_start + 48 * len(polys)
    mats = bytearray()
    mat_offs = []
    for p in polys:
        offs = []
        for m in p['materials']:
            offs.append(mat_start + len(mats))
            mats += _material_bytes(m, addname)
        mat_offs.append(offs + [0] * (4 - len(offs)))
    desc = bytearray(mats)
    desc_end = mat_start + len(mats)
    idx_start = (desc_end + 15) & ~15
    # clumps
    ib, vb, ab = bytearray(), bytearray(), bytearray()
    rec_offs = []
    for p in polys:
        io = len(ib)
        ib += np.asarray(p['indices'], '>u2').tobytes()
        va, vadd = _split(p['verts'], p['vtype'], p['uvtype'])
        vo = len(vb)
        vb += va.tobytes()
        _pad(vb)
        ao = 0
        if vadd is not None:
            ao = len(ab)
            ab += vadd.tobytes()
            _pad(ab)
        rec_offs.append((io, vo, ao))
    _pad(ib)
    # records
    out = bytearray(idx_start)
    bfirst, blast = model.get('bone_first'), model.get('bone_last')
    if bfirst is None:
        bfirst, blast = used_bones(model)
    total = idx_start + len(ib) + len(vb) + len(ab) + len(strings)
    struct.pack_into('>4sIHHhH4I4f', out, 0, b'NDP3', total, model.get('version', 0x0201), len(meshes), bfirst,
                     blast, idx_start - 0x30, len(ib), len(vb), len(ab), *model['sphere'])
    k = 0
    for mi, m in enumerate(meshes):
        p = 0x30 + 48 * mi
        struct.pack_into('>8fIHHhHI', out, p, *m['sphere'], *m['center'], m.get('sort_bias', 0.0),
                         mesh_name_off[mi], m.get('zero', 0), m['boneflag'], m['singlebind'], len(m['polys']),
                         poly_rec_start + 48 * k)
        k += len(m['polys'])
    for j, p in enumerate(polys):
        q = poly_rec_start + 48 * j
        io, vo, ao = rec_offs[j]
        struct.pack_into('>3IHBB4IHH', out, q, io, vo, ao, len(p['verts']), p['vtype'], p['uvtype'], *mat_offs[j],
                         len(p['indices']), p['pflag'])
        out[q + 36:q + 48] = bytes(p.get('tail', bytes(12)))
    out[mat_start:desc_end] = desc
    out += ib + vb + ab + strings
    assert len(out) == total
    return bytes(out)


# ------------------------------------------------------------------------------------------------ helpers
def bounding_sphere(points):
    """Centre = bounding-box centre, radius = farthest point (what new meshes use)."""
    p = np.asarray(points, np.float64)
    c = (p.min(0) + p.max(0)) / 2
    return tuple(float(x) for x in c) + (float(np.sqrt(((p - c) ** 2).sum(1)).max()),)


def strip_to_tris(ix):
    tris, run = [], []

    def flush():
        for k in range(len(run) - 2):
            a, b, c = run[k], run[k + 1], run[k + 2]
            if a == b or b == c or a == c:
                continue
            tris.append((a, b, c) if k % 2 == 0 else (b, a, c))
    for i in ix:
        if i == 0xFFFF:
            flush()
            run = []
        else:
            run.append(int(i))
    flush()
    return tris
