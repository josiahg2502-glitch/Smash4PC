"""Cooked USkeletalMesh (Unreal Engine 4.14 with Tekken 7's custom versions: skeletal mesh 5, editor object 7,
rendering object 9, recompute tangent 1).

After the tagged properties and the object GUID flag:
    FStripDataFlags, FBoxSphereBounds (7 f32)
    TArray materials: {i32 FPackageIndex, bool32 cast shadow, bool32 recompute tangent}
    reference skeleton: TArray {FName, i32 parent}, TArray local transforms {quat x y z w, translation, scale}
        (10 f32), TMap<FName, i32>
    TArray LODs:
        FStripDataFlags
        TArray sections: FStripDataFlags, i16 material, i32 first index, i32 triangle count, u8 sorting, bool32
            disabled, i16 cloth section, u8 cloth LOD, bool32 recompute tangent, u32 first vertex, TArray<u16> bone
            map, i32 vertex count, i32 max influences, TArray cloth mapping (64 bytes each), TArray<FVector> physics
            vertices, TArray<FVector> physics normals, i16 cloth asset, i16 cloth submesh
        indices: u8 size (2 or 4) + bulk array
        TArray<i16> active bones, i32 size, i32 vertex count, TArray<i16> required bones, TArray<i32> import vertex
        map, i32 max import vertex, i32 UV set count
        vertex buffer: FStripDataFlags, i32 UV sets, bool32 full-precision UVs, bool32 eight influences, FVector
            extension, FVector origin, bulk array of {u32 tangent X, u32 tangent Z (packed normals: byte / 127.5 - 1,
            Z.w = bitangent sign), u8 bone[4|8], u8 weight[4|8], FVector position, UV sets as half2 or float2}
        [with the mesh property bHasVertexColors: FStripDataFlags + bulk array of FColor (B, G, R, A)]
        [unless class strip flag 1: adjacency indices like the indices]
        [when a section has cloth mapping: FStripDataFlags + bulk array]
A vertex's bone indices index its section's bone map, whose entries are reference-skeleton bones. Units are
centimetres, Z up, left-handed.
"""
import numpy as np

from .uasset import ParseError, read_object


def read_ref_skeleton(r):
    bones = r.tarray(lambda: (r.fname(), r.i32()))
    n = r.i32()
    pose = r.np_array('<f4', n * 10).reshape(n, 10)
    r.tarray(lambda: (r.fname(), r.i32()))
    if len(pose) != len(bones):
        raise ParseError('reference pose has %d bones, skeleton %d' % (len(pose), len(bones)))
    return {'names': [b[0] for b in bones], 'parents': [b[1] for b in bones], 'pose': pose}


def _indices(r):
    size = r.u8()
    if size == 2:
        return r.bulk_array('<u2')[2].astype(np.uint32)
    if size == 4:
        return r.bulk_array('<u4')[2]
    raise ParseError('index size %d' % size)


class Section:
    __slots__ = ('material_index', 'base_index', 'num_triangles', 'base_vertex', 'num_vertices', 'bone_map',
                 'disabled', 'cloth_mapping')


def _section(r):
    s = Section()
    r.strip_flags()
    s.material_index = r.i16()
    s.base_index = r.i32()
    s.num_triangles = r.i32()
    r.u8()
    s.disabled = r.bool32()
    r.i16()
    r.u8()
    r.bool32()
    s.base_vertex = r.u32()
    s.bone_map = r.tarray_np('<u2')
    s.num_vertices = r.i32()
    r.i32()
    s.cloth_mapping = r.i32()
    r.skip(64 * s.cloth_mapping)
    r.skip(12 * r.i32())
    r.skip(12 * r.i32())
    r.i16(), r.i16()
    return s


class LOD:
    pass


def _lod(r, has_vertex_colors):
    lod = LOD()
    gflags, cflags = r.strip_flags()
    if gflags & 1 == 0:
        raise ParseError('a mesh LOD with editor data is not supported')
    lod.sections = r.tarray(lambda: _section(r))
    lod.indices = _indices(r)
    r.tarray_np('<i2')
    r.i32()
    lod.num_vertices = r.i32()
    r.tarray_np('<i2')
    r.tarray_np('<i4')
    r.i32()
    r.i32()
    r.strip_flags()
    nuv = r.i32()
    full_uv = r.bool32()
    extra = r.bool32()
    r.floats(3), r.floats(3)
    ninf = 8 if extra else 4
    dt = np.dtype([('tx', '<u4'), ('tz', '<u4'), ('bi', 'u1', (ninf,)), ('bw', 'u1', (ninf,)),
                   ('pos', '<f4', (3,)), ('uv', '<f4' if full_uv else '<f2', (nuv, 2))])
    _, n, lod.verts = r.bulk_array(dt)
    if n != lod.num_vertices:
        raise ParseError('vertex buffer has %d vertices, the LOD %d' % (n, lod.num_vertices))
    lod.colors = None
    if has_vertex_colors:
        r.strip_flags()
        lod.colors = r.bulk_array('<u4')[2].view(np.uint8).reshape(-1, 4)
    if not cflags & 1:
        _indices(r)
    if any(s.cloth_mapping for s in lod.sections):
        r.strip_flags()
        r.bulk_array()
    return lod


def unpack_normal(u32):
    b = np.stack([(u32 >> (8 * i)) & 0xFF for i in range(4)], axis=-1).astype(np.float32)
    return b / 127.5 - 1.0


class SkeletalMesh:
    def __init__(self, pkg, export=None):
        self.export = export or pkg.find_export('SkeletalMesh')
        self.name = self.export.name
        self.props, r = read_object(pkg, self.export)
        r.strip_flags()
        r.floats(7)
        self.materials = r.tarray(lambda: {'material': r.pkg_index(), 'cast_shadow': r.bool32(),
                                           'recompute_tangent': r.bool32()})
        self.skeleton = read_ref_skeleton(r)
        has_colors = bool(self.props.get('bHasVertexColors', False))
        self.lods = r.tarray(lambda: _lod(r, has_colors))

    def geometry(self, lod_index=0):
        """Vertex streams of a LOD (float32): positions, normals, tangents (xyz + sign), uvs (n, sets, 2), weights
        (bytes / 255); joints (reference-skeleton bones) and the triangles of each section."""
        lod = self.lods[lod_index]
        v = lod.verts
        tz = unpack_normal(v['tz'])
        tx = unpack_normal(v['tx'])
        normal = tz[:, :3]
        normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-8)
        tangent = tx[:, :3]
        tangent /= np.maximum(np.linalg.norm(tangent, axis=1, keepdims=True), 1e-8)
        sign = np.where(tz[:, 3] < 0, -1.0, 1.0).astype(np.float32)
        joints = np.zeros(v['bi'].shape, np.int32)
        for si, s in enumerate(lod.sections):
            a, b = s.base_vertex, s.base_vertex + s.num_vertices
            bm = s.bone_map.astype(np.int32)
            local = v['bi'][a:b].astype(np.int32)
            if local.size and local.max() >= len(bm):
                raise ParseError('section %d: bone %d outside its bone map (%d)' % (si, local.max(), len(bm)))
            joints[a:b] = bm[local]
        tris = [lod.indices[s.base_index:s.base_index + 3 * s.num_triangles].reshape(-1, 3) for s in lod.sections]
        return {'positions': v['pos'].astype(np.float32), 'normals': normal,
                'tangents': np.concatenate([tangent, sign[:, None]], 1), 'uvs': v['uv'].astype(np.float32),
                'joints': joints, 'weights': v['bw'].astype(np.float32) / 255.0, 'triangles': tris}


# ------------------------------------------------------------------------------------------------ pose math
# A reference-pose transform (quat x y z w, translation, scale) maps p_parent = T + R (S p_local).
def quat_to_mat(q):
    q = np.asarray(q, np.float64)
    q = q / np.linalg.norm(q, axis=-1, keepdims=True)
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    m = np.empty(q.shape[:-1] + (3, 3))
    m[..., 0, 0] = 1 - 2 * (y * y + z * z)
    m[..., 0, 1] = 2 * (x * y - z * w)
    m[..., 0, 2] = 2 * (x * z + y * w)
    m[..., 1, 0] = 2 * (x * y + z * w)
    m[..., 1, 1] = 1 - 2 * (x * x + z * z)
    m[..., 1, 2] = 2 * (y * z - x * w)
    m[..., 2, 0] = 2 * (x * z - y * w)
    m[..., 2, 1] = 2 * (y * z + x * w)
    m[..., 2, 2] = 1 - 2 * (x * x + y * y)
    return m


def local_matrices(pose):
    """(n, 10) poses -> (n, 4, 4) local matrices."""
    m = np.zeros((len(pose), 4, 4))
    m[:, :3, :3] = quat_to_mat(pose[:, 0:4]) * pose[:, None, 7:10]
    m[:, :3, 3] = pose[:, 4:7]
    m[:, 3, 3] = 1
    return m


def global_matrices(pose, parents):
    loc = local_matrices(pose)
    g = np.zeros_like(loc)
    for i, p in enumerate(parents):
        if p >= i:
            raise ValueError('bone %d has its parent %d after it' % (i, p))
        g[i] = loc[i] if p < 0 else g[p] @ loc[i]
    return g


def mat_to_quat(m):
    """3x3 rotation -> quaternion x, y, z, w."""
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def matrix_to_pose(m):
    """4x4 local matrix -> (10,) pose."""
    sc = np.linalg.norm(m[:3, :3], axis=0)
    r = m[:3, :3] / sc
    if np.linalg.det(r) < 0:
        sc[0] = -sc[0]
        r[:, 0] = -r[:, 0]
    return np.concatenate([mat_to_quat(r), m[:3, 3], sc])
