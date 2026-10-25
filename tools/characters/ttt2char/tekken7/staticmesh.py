"""Cooked UStaticMesh (Unreal Engine 4.14 as Tekken 7 cooks it): the first LOD's triangles, positions, normals,
UVs and vertex colours. Its effects draw particles as such meshes (rings, slashes, aura shells, planes).

After the tagged properties (`Materials`: one material per section) and the object GUID flag:
    FStripDataFlags, bool32 cooked, i32 body setup (FPackageIndex), FGuid lighting GUID, i32 (0), then the render
    data: TArray LODs:
        FStripDataFlags
        TArray sections: i32 material, u32 first index, u32 triangle count, u32 min vertex, u32 max vertex,
            bool32 collision, bool32 cast shadow
        f32 max deviation
        positions: u32 stride (12), u32 count, bulk array of FVector
        vertices: FStripDataFlags, u32 UV sets, u32 stride, u32 count, bool32 full-precision UVs, bool32
            high-precision tangents, bulk array of {u32 tangent X, u32 tangent Z (packed normals: byte / 127.5 -
            1), UV sets as half2 or float2}
        colours: FStripDataFlags, u32 stride (0 or 4), u32 count, [bulk array of FColor (B, G, R, A)]
        indices: bool32 32-bit, bulk array of bytes (u16 or u32 indices)
        (more index buffers follow: not read)
Units are centimetres, Z up.

Written for this project from the files themselves (no third-party code).
"""
import numpy as np

from .uasset import ParseError, read_object
from .mesh import unpack_normal


class StaticMesh:
    def __init__(self, pkg, export=None):
        self.export = export or pkg.find_export('StaticMesh')
        self.name = self.export.name
        self.props, r = read_object(pkg, self.export)
        self.materials = list(self.props.get('Materials') or [])
        r.strip_flags()
        if not r.bool32():
            raise ParseError('%s is not a cooked mesh' % self.name)
        r.i32()
        r.guid()
        r.i32()
        if r.i32() < 1:
            raise ParseError('%s has no LOD' % self.name)
        r.strip_flags()
        self.sections = r.tarray(lambda: dict(material=r.i32(), first=r.u32(), triangles=r.u32(), min=r.u32(),
                                              max=r.u32(), collision=r.bool32(), shadow=r.bool32()))
        r.f32()
        stride, n = r.u32(), r.u32()
        if stride != 12:
            raise ParseError('%s: position stride %d' % (self.name, stride))
        self.positions = r.bulk_array(np.dtype(('<f4', (3,))))[2].reshape(-1, 3).astype(np.float32)
        r.strip_flags()
        sets, stride, count = r.u32(), r.u32(), r.u32()
        full_uv, precise = r.bool32(), r.bool32()
        tangent = '<u8' if precise else '<u4'
        dt = np.dtype([('tx', tangent), ('tz', tangent), ('uv', '<f4' if full_uv else '<f2', (sets, 2))])
        if dt.itemsize != stride or count != n:
            raise ParseError('%s: vertex stride %d (expected %d), %d vertices for %d positions'
                             % (self.name, stride, dt.itemsize, count, n))
        verts = r.bulk_array(dt)[2]
        if precise:
            raise ParseError('%s: high-precision tangents are not supported' % self.name)
        tz = unpack_normal(verts['tz'])
        self.normals = tz[:, :3].copy()
        self.normals /= np.maximum(np.linalg.norm(self.normals, axis=1, keepdims=True), 1e-8)
        tangent = unpack_normal(verts['tx'])[:, :3]
        tangent /= np.maximum(np.linalg.norm(tangent, axis=1, keepdims=True), 1e-8)
        self.tangents = np.concatenate([tangent, np.where(tz[:, 3:] < 0, -1.0, 1.0)], 1).astype(np.float32)
        self.uvs = verts['uv'].astype(np.float32)
        r.strip_flags()
        stride, count = r.u32(), r.u32()
        self.colors = None
        if count:
            if stride != 4 or count != n:
                raise ParseError('%s: colour stride %d, %d colours' % (self.name, stride, count))
            bgra = r.bulk_array(np.dtype(('u1', (4,))))[2].reshape(-1, 4)
            self.colors = bgra[:, [2, 1, 0, 3]].copy()                    # RGBA
        wide = r.bool32()
        raw = r.bulk_array()[2]
        self.indices = np.frombuffer(raw, '<u4' if wide else '<u2').astype(np.uint32)
        if self.indices.size and self.indices.max() >= n:
            raise ParseError('%s: index %d beyond %d vertices' % (self.name, self.indices.max(), n))

    def triangles(self):
        """[(material slot, (k, 3) vertex indices)] of the sections."""
        return [(s['material'], self.indices[s['first']:s['first'] + 3 * s['triangles']].reshape(-1, 3))
                for s in self.sections]
