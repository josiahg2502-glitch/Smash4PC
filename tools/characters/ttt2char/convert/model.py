"""A Tekken 7 fighter's default costume -> the added fighter's body files: model (.nud), textures (.nut) and
skeleton (.bon). Everything goes into the body files, no costume parts.

1. Tekken 7 side: the costume's skeletal meshes are read from the paks and merged into one skinned mesh with a union
   skeleton. Coordinates go to TTT2's: Y and Z swapped, centimetres -> millimetres, by way of metres in float32 (the
   precision the converted files have always had). Each material's colour is baked into one albedo texture with the
   costume's colours (the formulas of Tekken 7's character shaders, recovered from their compiled pixel shaders).
2. TTT2 side: the donor's skeleton gets the Tekken 7 joint positions (skeleton.py); skin weights go to TTT2 bones
   (four per vertex); triangles become strips; each material is a copy of one of the donor's materials (flags,
   shader properties, texture slots) with new textures: the baked albedo, the normal map, and flat textures of the
   template's own average colour for the other slots.
Textures are at most 512 texels on a side, with at most 3 mip levels (longer chains make the game bind garbage).
"""
import collections
import copy

import numpy as np
from PIL import Image

from ..tekken7 import mesh as t7mesh
from ..tekken7.material import Materials, short_ref
from ..tekken7.staticmesh import StaticMesh
from ..tekken7.texture import Texture2D
from ..tekken7.uasset import export_properties, load
from ..ttt2 import bone, nud, nut
from ..ttt2.archive import key_of
from . import chains as hanging, colours as colours2p, skeleton, skin, strips, tiger, weapons

GIDX_BASE = 0x10408000          # texture ids above every id the game's own textures use (0x10404fac at most)
SLOT_GIDX = 0x1000              # each place's added fighter its own range of texture ids (they meet in fights)
MAX_TEX = 512                   # longest side of a converted texture
MIPS = 3                        # mip levels per texture (the game's own fighter textures have 3 or 1)
# a fighter's texture bytes at most: the game's own fighters have 3.4-4.7 MB (a fight's memory is sized for them;
# 12 MB showed a black model on busy stages); the largest textures are halved (normal maps first) until they fit
TEXTURE_BUDGET = 4 << 20
M_TO_MM = 1000.0


# ------------------------------------------------------------------------------------------------ Tekken 7 side
def _ue_to_ttt2_points(p):
    """UE centimetres (x, y, z) -> (x, z, y) metres in float32 (multiply by M_TO_MM for TTT2)."""
    p = np.asarray(p, np.float32)
    return np.stack([p[..., 0], p[..., 2], p[..., 1]], axis=-1) * 0.01


def _ue_to_ttt2_dirs(d):
    d = np.asarray(d, np.float32)
    return np.stack([d[..., 0], d[..., 2], d[..., 1]], axis=-1)


_SWAP = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], np.float64)


def _ue_to_ttt2_matrix(m):
    out = (_SWAP @ m @ _SWAP).copy()
    out[..., :3, 3] *= 0.01
    return out


def merge_skeletons(parts):
    """Union skeleton of several meshes by bone name (names are case-insensitive). parts: [(tag, mesh, weighted
    bone set)] with the owners first (face joints from the face mesh, body joints from the body meshes).

    A part's bone maps to a merged bone of the same name and parent if their bind matrices agree (0.05 cm, 1e-3)
    or if nothing is skinned to it; a weighted bone that disagrees becomes a copy '<name>@<tag>'. Other bones keep
    their part's bind matrix under the (shared) parent. Generic cloth bone names (transform9, Hair_OPT001, ...) are
    reused by different items under different parents, hence the parent test.
    Returns ({'names', 'parents', 'pose' (float32)}, {tag: part bone -> merged bone})."""
    names, parents, poses, by_name, glob = [], [], [], {}, []
    remaps = {}
    for tag, m, weighted in parts:
        sk = m.skeleton
        g = t7mesh.global_matrices(sk['pose'], sk['parents'])
        remap = np.zeros(len(sk['names']), np.int32)
        for i, nm in enumerate(sk['names']):
            par = sk['parents'][i]
            par_new = int(remap[par]) if par >= 0 else -1
            j = by_name.get(nm.lower())
            if j is not None:
                same = (np.abs(glob[j][:3, 3] - g[i][:3, 3]).max() < 0.05 and
                        np.abs(glob[j][:3, :3] - g[i][:3, :3]).max() < 1e-3)
                if parents[j] == par_new and (same or i not in weighted):
                    remap[i] = j
                    continue
                nm = '%s@%s' % (nm, tag)
                pose = t7mesh.matrix_to_pose(np.linalg.inv(glob[par_new]) @ g[i] if par_new >= 0 else g[i])
            elif par_new >= 0:
                pose = t7mesh.matrix_to_pose(np.linalg.inv(glob[par_new]) @ g[i])
            else:
                pose = sk['pose'][i]
            by_name[nm.lower()] = len(names)
            remap[i] = len(names)
            names.append(nm)
            parents.append(par_new)
            poses.append(np.asarray(pose, np.float32))
            gl = t7mesh.local_matrices(np.asarray(pose)[None])[0]
            glob.append(glob[par_new] @ gl if par_new >= 0 else gl)
        remaps[tag] = remap
    return {'names': names, 'parents': parents, 'pose': np.array(poses)}, remaps


def costume_ccis(paks, set_path, follow=True):
    """The CustomizeCharacterItem paths of a costume set: its items and (follow) the items they depend on
    (DependencyItems: worn with them, as Noctis's trousers and boots with his jacket)."""
    cs = export_properties(load(paks, set_path))[0][1] or {}
    out = []
    todo = [short_ref(ref).split('.')[0] for ref in cs.get('CustomCharacterItems', []) if short_ref(ref)]
    while todo:
        path = todo.pop(0)
        if path in out:
            continue
        out.append(path)
        cci = export_properties(load(paks, path))[0][1] or {}
        if follow:
            todo += [short_ref(ref).split('.')[0] for ref in cci.get('DependencyItems') or [] if short_ref(ref)]
    return out


def costume_items(paks, set_path, follow=False):
    """The CharacterItem properties of each item of a costume set (follow: with the items they depend on)."""
    out = []
    for path in costume_ccis(paks, set_path, follow):
        cci = export_properties(load(paks, path))[0][1] or {}
        if short_ref(cci.get('CharacterItem')):
            out.append(export_properties(load(paks, short_ref(cci['CharacterItem']).split('.')[0]))[0][1] or {})
    return out


def dependency_parts(items, parts):
    """[(tag, skeletal mesh)] of the costume's items its definition's parts do not list (items another depends on:
    Noctis's trousers and boots), tagged dep0, dep1, ..."""
    listed = {path.lower() for _, path in parts}
    meshes = [short_ref(ci.get('SourceSkeletalMesh')).split('.')[0] for ci in items
              if ci.get('MeshType') == 'SkeletalMeshAnim' and short_ref(ci.get('SourceSkeletalMesh'))]
    return [('dep%d' % n, path) for n, path in enumerate(m for m in meshes if m.lower() not in listed)]


def dependency_meshes(items):
    """The DependencyMesh of each costume item: the variant of another item's mesh (in that mesh's folder) that
    Tekken 7 draws instead of it while this item is worn, cut to fit under it (Shaheen's hair inside his scarf,
    Josie's under her hair band)."""
    return [short_ref(ci['DependencyMesh']).split('.')[0] for ci in items if short_ref(ci.get('DependencyMesh'))]


def socket_items(items):
    """The costume's static meshes worn on a socket (Leroy's sunglasses): [(static mesh path, ItemOffsets)]. Items
    without a SocketPosition sit on the head: Tekken 7 models them in its body axes about the Head joint (glasses
    without offsets are modelled 12 cm in front of it), the offsets (UE transform: scale, rotation, translation) in
    those axes."""
    return [(short_ref(ci['SourceStaticMesh']).split('.')[0], ci.get('ItemOffsets') or {}) for ci in items
            if ci.get('MeshType') == 'StaticMeshSocket' and short_ref(ci.get('SourceStaticMesh')) and
            ci.get('SocketPosition', 'UNSET_SOCKET') == 'UNSET_SOCKET']


def worn_parts(parts, dependencies):
    """parts [(tag, mesh path)] with each mesh that has a dependency mesh in its folder replaced by it (folders
    compared ignoring case: the game's references and the definitions spell them differently)."""
    by_folder = {p.rsplit('/', 1)[0].lower(): p for p in dependencies}
    return [(tag, by_folder.get(path.rsplit('/', 1)[0].lower(), path)) for tag, path in parts]


def in_front(points, cover, cell=2.0, margin=2.0):
    """Which points (n, 3) lie in front of (+z, the fighter's front) the front surface of the triangles cover (m, 3, 3),
    or less than margin behind it, where it is seen from the front: its largest z on a grid of cell mm squares."""
    lo = cover[..., :2].reshape(-1, 2).min(0)
    size = np.ceil((cover[..., :2].reshape(-1, 2).max(0) - lo) / cell).astype(int) + 1
    front = np.full(size, -np.inf)
    for t in cover:
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t
        d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(d) < 1e-9:                            # seen edge-on
            continue
        i0, j0 = np.floor((t[:, :2].min(0) - lo) / cell).astype(int)
        i1, j1 = np.floor((t[:, :2].max(0) - lo) / cell).astype(int) + 1
        x, y = np.meshgrid(lo[0] + (np.arange(i0, i1) + 0.5) * cell, lo[1] + (np.arange(j0, j1) + 0.5) * cell,
                           indexing='ij')
        l1 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d
        l2 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d
        l3 = 1 - l1 - l2
        z = np.where((l1 >= 0) & (l2 >= 0) & (l3 >= 0), l1 * az + l2 * bz + l3 * cz, -np.inf)
        front[i0:i1, j0:j1] = np.maximum(front[i0:i1, j0:j1], z)
    ij = np.floor((points[:, :2] - lo) / cell).astype(int)
    inside = ((ij >= 0) & (ij < size)).all(1)
    z = np.full(len(points), -np.inf)
    z[inside] = front[ij[inside, 0], ij[inside, 1]]
    return np.isfinite(z) & (points[:, 2] > z - margin)


def load_tekken7_model(paks, parts, merge_order):
    """The costume's meshes as one skinned mesh in TTT2 space.
    parts: [(tag, skeletal mesh path)] in mesh order; merge_order: the tags in skeleton-merge order.
    Returns {'bones': {'names', 'parents', 'world' (n, 4, 4) bind matrices, mm}, 'prims': [{'part', 'material',
    'pos' (mm), 'nrm', 'tan', 'uv', 'joints' (n, 8), 'weights' (n, 8), 'tris'}] (prims share the vertex arrays),
    'parts': {tag: {'names', 'parents', 'remap' (part bone -> merged bone)}}}."""
    loaded = []
    for tag, path in parts:
        m = t7mesh.SkeletalMesh(load(paks, path))
        m.skeleton['names'] = [skeleton.canonical(n) for n in m.skeleton['names']]
        loaded.append((tag, m, m.geometry(0)))
    by_tag = {tag: (m, geo) for tag, m, geo in loaded}
    merged, remaps = merge_skeletons(
        [(t, by_tag[t][0], set(np.unique(by_tag[t][1]['joints'][by_tag[t][1]['weights'] > 0]).tolist()))
         for t in merge_order])
    pos, nrm, tan, uvs, jnt, wts, sections = [], [], [], [], [], [], []
    base = 0
    for tag, m, geo in loaded:
        n = len(geo['positions'])
        pos.append(geo['positions'])
        nrm.append(geo['normals'])
        tan.append(geo['tangents'])
        uvs.append(geo['uvs'][:, 0])
        j, w = remaps[tag][geo['joints']], geo['weights']
        if w.shape[1] == 4:
            j = np.concatenate([j, np.zeros_like(j)], 1)
            w = np.concatenate([w, np.zeros_like(w)], 1)
        jnt.append(j)
        wts.append(w)
        for si, s in enumerate(m.lods[0].sections):
            if s.cloth_mapping:                  # the cloth simulation's copy of a disabled section
                continue
            ref = m.materials[s.material_index]['material']
            sections.append((tag, short_ref(ref).split('.')[-1] if ref else 'None', geo['triangles'][si] + base))
        base += n
    # the precision of the vertex streams: float32 metres, normalised float32 weights
    p = _ue_to_ttt2_points(np.concatenate(pos)).astype(np.float64) * M_TO_MM
    nr = _ue_to_ttt2_dirs(np.concatenate(nrm)).astype(np.float32).astype(np.float64)
    tg = np.concatenate(tan).astype(np.float32).copy()
    tg[:, :3] = _ue_to_ttt2_dirs(tg[:, :3])
    tg[:, 3] = -tg[:, 3]                         # the axis swap is a reflection: the bitangent sign flips
    uv = np.concatenate(uvs).astype(np.float32).astype(np.float64)
    weights = np.concatenate(wts).astype(np.float32)
    weights = weights / np.maximum(weights.sum(1, keepdims=True), 1e-8)
    joints = np.where(weights > 0, np.concatenate(jnt), 0).astype(np.uint16).astype(np.int64)
    streams = dict(pos=p, nrm=nr, tan=tg.astype(np.float64), uv=uv, joints=joints, weights=weights.astype(np.float64))
    prims = [dict(part=tag, material=mat, tris=tri.astype(np.uint32)[:, [0, 2, 1]].astype(np.int64), **streams)
             for tag, mat, tri in sections]
    # bind matrices: the inverse bind matrices are kept in float32
    g = t7mesh.global_matrices(merged['pose'], merged['parents'])
    ibm = np.linalg.inv(_ue_to_ttt2_matrix(g)).astype(np.float32)
    world = np.linalg.inv(ibm.astype(np.float64))
    world[:, :3, 3] *= M_TO_MM
    skeletons = {tag: dict(names=m.skeleton['names'], parents=m.skeleton['parents'], remap=remaps[tag])
                 for tag, m, _ in loaded}
    return {'bones': dict(names=merged['names'], parents=merged['parents'], world=world), 'prims': prims,
            'parts': skeletons}


def load_socket_item(paks, model, tag, path, offsets):
    """A static mesh worn on the head (socket_items) as prims of the model, rigid on its Head joint: placed by the
    item's offsets in Tekken 7's body axes about the joint, scaled with it (its bind scale, 0.98 for Leroy)."""
    sm = StaticMesh(load(paks, path))
    names = model['bones']['names']
    head = names.index('Head')
    world = model['bones']['world'][head]
    scale = float(np.linalg.norm(world[:3, 0]))
    pose = np.array([list(offsets.get('Rotation', (0, 0, 0, 1))) + list(offsets.get('Translation', (0, 0, 0))) +
                     list(offsets.get('Scale3D', (1, 1, 1)))], np.float64)
    m = t7mesh.local_matrices(pose)[0]
    lin = m[:3, :3]
    p = sm.positions.astype(np.float64) @ lin.T + m[:3, 3]           # UE centimetres about the joint
    pos = world[:3, 3] + scale * _ue_to_ttt2_points(p).astype(np.float64) * M_TO_MM
    nrm = _ue_to_ttt2_dirs(sm.normals.astype(np.float64) @ np.linalg.inv(lin))      # (inverse transpose)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
    tan = np.concatenate([_ue_to_ttt2_dirs(sm.tangents[:, :3].astype(np.float64) @ lin.T),
                          -sm.tangents[:, 3:].astype(np.float64)], 1)  # (the axis swap flips the bitangent sign)
    tan[:, :3] /= np.maximum(np.linalg.norm(tan[:, :3], axis=1, keepdims=True), 1e-9)
    n = len(pos)
    joints = np.zeros((n, 8), np.int64)
    joints[:, 0] = head
    weights = np.zeros((n, 8))
    weights[:, 0] = 1.0
    streams = dict(pos=pos, nrm=nrm, tan=tan, uv=sm.uvs[:, 0].astype(np.float64), joints=joints, weights=weights)
    return [dict(part=tag, material=short_ref(sm.materials[slot]).split('.')[-1] if sm.materials[slot] else 'None',
                 tris=tri.astype(np.int64)[:, [0, 2, 1]], **streams) for slot, tri in sm.triangles() if len(tri)]


# ------------------------------------------------------------------------------------------------ albedo baking
MASK_MATERIALS = ('M_CH_Aniso', 'M_CH_aniso', 'M_CH_Base', 'M_CH_Base_mask', 'M_CH_Aniso_machieru',
                  'M_CH_Base_machieru')


def srgb_to_linear(x):
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


class Textures:
    """Tekken 7 textures by object path, decoded once."""

    def __init__(self, paks):
        self.paks = paks
        self._cache = {}

    def exists(self, path):
        return bool(path) and self.paks.find(path) is not None

    def get(self, path):
        """-> (RGBA uint8 array, sRGB flag)"""
        if path not in self._cache:
            t = Texture2D(load(self.paks, path))
            self._cache[path] = (t.decode(), t.srgb)
        return self._cache[path]

    def linear(self, path):
        """-> (linear RGB float32, alpha float32)"""
        img, srgb = self.get(path)
        a = img.astype(np.float32) / 255.0
        return (srgb_to_linear(a[..., :3]) if srgb else a[..., :3]), a[..., 3]


def _col(v):
    return np.array([v['r'], v['g'], v['b']], np.float32)


def _resize_to(a, shape):
    if a.shape[:2] == shape:
        return a
    if a.ndim == 2:
        return np.asarray(Image.fromarray(a.astype(np.float32)).resize((shape[1], shape[0]), Image.BILINEAR))
    return np.stack([_resize_to(a[..., c], shape) for c in range(a.shape[-1])], -1)


def bake_albedo(eff, textures, grey=False):
    """Linear albedo (H, W, 3) and alpha (H, W) of a material instance with its parameters in effect, or (None,
    None) without a colour texture.

    Colour-customisable materials (MF_CH_ColorCustomize):
        C = lerp(lerp(lerp(Color02, Color01, M.r), Color03, M.g), Color04, M.b)    (M = mask texture)
        k = max(1, 1 + 20 (max(M.r, M.g, M.b) - 0.9))
        albedo = D (1 - M.a) + k D C M.a          (the pattern layer is empty in default costumes)
      The *_machieru ones also multiply by Multiply * lerp(DetalDiffuse(uv * Detail_Normal_Scale), 1,
      detail_diffuse_pow).
    Hair (pass 1): lo = sat(BaseLowerContrast); s = (min(Up, 0) + min(Lo, 0) + 1) / (1 - (sat(Up) + sat(Lo)));
        albedo = sat((T - lo) s - min(Lo, 0)) * Color01; Noctis's (HairTipsAdd, a fixed gradient texture G):
        albedo = sat(T + HairTipsAdd G) * Color01.
    Skin: lerp(D, luma(D), Desaturation) * Multiply (the view-dependent terms left out).
    Anything else: its colour texture as it is.
    grey: a colour-customisable material's colour texture taken by its brightness alone (D's luma), its colours giving
    the hue (a definition's GREY: Noctis's outfit, blue-grey in Tekken 7 where its textures are mauve)."""
    base = eff['base']
    t, v, s = eff['textures'], eff['vectors'], eff['scalars']
    masked = base in MASK_MATERIALS or (not base.startswith('M_CH_AnisoHair') and t.get('DiffuseMap')
                                        and t.get('MaskMap') and 'Color01' in v and 'Color02' in v)
    if masked:                               # (M_CH_Aniso_mask, M_CH_Base_machieru_mask and the like too)
        D, Da = textures.linear(t['DiffuseMap'])
        if grey:
            D = np.repeat((D * np.array([0.2126, 0.7152, 0.0722], np.float32)).sum(-1, keepdims=True), 3, -1)
        M, Ma = textures.linear(t['MaskMap'])
        M, Ma = _resize_to(M, D.shape[:2]), _resize_to(Ma, D.shape[:2])
        C = _col(v['Color02'])[None, None] + M[..., 0:1] * (_col(v['Color01']) - _col(v['Color02']))
        C = C + M[..., 1:2] * (_col(v['Color03']) - C)
        C = C + M[..., 2:3] * (_col(v['Color04']) - C)
        k = np.maximum(1.0, 1.0 + 20.0 * (M.max(-1, keepdims=True) - 0.9))
        out = D * (1 - Ma[..., None]) + k * D * C * Ma[..., None]
        if 'machieru' in base and t.get('DetalDiffuse'):
            det, _ = textures.linear(t['DetalDiffuse'])
            scale = s.get('Detail_Normal_Scale', 1.0)
            h, w = D.shape[:2]
            yy = ((np.arange(h) + 0.5) / h * scale * det.shape[0]).astype(np.int64) % det.shape[0]
            xx = ((np.arange(w) + 0.5) / w * scale * det.shape[1]).astype(np.int64) % det.shape[1]
            dt = det[yy][:, xx]
            p = s.get('detail_diffuse_pow', 0.0)
            out = out * s.get('Multiply', 1.0) * (dt + p * (1 - dt))
        return np.clip(out, 0, 1), Da
    if base.startswith('M_CH_AnisoHair'):
        T, Ta = textures.linear(t['ColorTexture'])
        lo_, up_ = s.get('BaseLowerContrast', 0.0), s.get('BaseUpperContrast', 0.0)
        lo = np.clip(lo_, 0, 1)
        sc = (min(up_, 0) + min(lo_, 0) + 1) / (1 - (np.clip(up_, 0, 1) + np.clip(lo_, 0, 1)))
        T = np.clip((T - lo) * sc - min(lo_, 0), 0, 1)
        tips = [x for x in eff.get('fixed', []) if textures.exists(x)]
        if s.get('HairTipsAdd') and tips:        # lighter tips: a gradient along the strands (Noctis's hair shader)
            G, _ = textures.linear(tips[0])
            T = np.clip(T + s['HairTipsAdd'] * _resize_to(G, T.shape[:2]), 0, 1)
        return T * _col(v['Color01']), Ta
    if base == 'M_CH_skin_wet_SSP':
        D, Da = textures.linear(t['Diffuse'])
        luma = (D * np.array([0.3, 0.59, 0.11], np.float32)).sum(-1, keepdims=True)
        out = (D + s.get('Desaturation', 0.0) * (luma - D)) * s.get('Multiply', 1.0)
        return np.clip(out, 0, 1), Da
    for slot in ('DiffuseMap', 'Diffuse', 'Color', 'ColorTexture'):
        if textures.exists(t.get(slot)):
            return textures.linear(t[slot])
    for name in sorted(v):                       # no texture but a glow: its colour (Akuma's eyes)
        if 'emissive' in name.lower():
            return np.clip(np.full((32, 32, 3), _col(v[name]), np.float32), 0, 1), None
    return None, None


def albedo_image(rgb, alpha):
    """The baked albedo as an 8-bit sRGB image (RGBA only where some texel is not opaque)."""
    a = (linear_to_srgb(rgb) * 255 + 0.5).astype(np.uint8)
    if alpha is not None and (alpha < 1).any():
        a = np.concatenate([a, (np.clip(alpha, 0, 1) * 255 + 0.5).astype(np.uint8)[..., None]], -1)
    return Image.fromarray(a)


# ------------------------------------------------------------------------------------------------ TTT2 side
def pow2_size(w, h, cap=MAX_TEX):
    def p2(x):
        return 1 << max(2, int(round(np.log2(max(4, x)))))
    s = min(1.0, cap / max(w, h))
    return min(cap, p2(w * s)), min(cap, p2(h * s))


class TextureBank:
    """The new textures, numbered from `first` in the order they are made; identical requests share one."""

    def __init__(self, first=GIDX_BASE):
        self.first, self.textures, self.by_key = first, [], {}
        self.sources, self.normals = [], set()          # each texture's image; the normal maps' indices

    def add(self, key, rgba, fmt):
        if key not in self.by_key:
            self.by_key[key] = self.first + len(self.textures)
            if key[0] == 'nrm':
                self.normals.add(len(self.textures))
            self.sources.append(np.asarray(rgba, np.uint8))
            self.textures.append(nut.make_texture(rgba, self.by_key[key], fmt, mips=MIPS))
        return self.by_key[key]

    def keep(self, ids):
        """Keeps only the textures `ids` (numbered again from `first`, in their order) -> {old id: new id}."""
        order = sorted(set(ids))
        new = {old: self.first + i for i, old in enumerate(order)}
        at = [old - self.first for old in order]
        self.textures = [dict(self.textures[i], gidx=new[old]) for i, old in zip(at, order)]
        self.sources = [self.sources[i] for i in at]
        self.normals = {j for j, i in enumerate(at) if i in self.normals}
        self.by_key = {key: new[v] for key, v in self.by_key.items() if v in new}
        return new

    def fit(self, budget):
        """Halves the largest textures (a normal map before an image of its size) until all fit in `budget` bytes
        -> how many were halved."""
        def size(i):
            return sum(len(m) for m in self.textures[i]['mips'])
        total, halved = sum(size(i) for i in range(len(self.textures))), 0
        while total > budget:
            big = [i for i, t in enumerate(self.textures) if min(t['w'], t['h']) > 64]
            if not big:
                break
            i = max(big, key=lambda i: (size(i) * (2 if i in self.normals else 1), i))
            t, before = self.textures[i], size(i)
            h, w = self.sources[i].shape[:2]
            self.sources[i] = (self.sources[i].reshape(h // 2, 2, w // 2, 2, 4).astype(np.float32).mean((1, 3))
                               + 0.5).astype(np.uint8)
            self.textures[i] = dict(nut.make_texture(self.sources[i], t['gidx'], t['fmt'], mips=len(t['mips'])),
                                    **{k: v for k, v in t.items() if k not in ('fmt', 'w', 'h', 'mips', 'gidx')})
            total += size(i) - before
            halved += 1
        return halved

    def constant(self, rgba, fmt, size=32):
        img = np.zeros((size, size, 4), np.uint8)
        img[:] = rgba
        return self.add(('const', tuple(int(x) for x in rgba), fmt), img, fmt)

    def image(self, key, im, fmt, alpha):
        if ('img', key, fmt, alpha) in self.by_key:
            return self.by_key[('img', key, fmt, alpha)]
        im = im.convert('RGBA')
        a = np.array(im.resize(pow2_size(*im.size), Image.LANCZOS))
        if not alpha:
            a[..., 3] = 255
        return self.add(('img', key, fmt, alpha), a, fmt)

    def normal(self, key, im):
        """A Tekken 7 normal map (R = X, G = Y towards +v) -> TTT2's BC3 layout (A = X, G = Y towards -v)."""
        if ('nrm', key) in self.by_key:
            return self.by_key[('nrm', key)]
        im = im.convert('RGB')
        w, h = pow2_size(*im.size)
        a = np.array(im.resize((w, h), Image.LANCZOS))
        out = np.zeros((h, w, 4), np.uint8)
        out[..., 3] = a[..., 0]
        out[..., 1] = 255 - a[..., 1]
        return self.add(('nrm', key), out, 2)


def find_mesh(model, name):
    for m in model['meshes']:
        if m['name'] == name:
            return m
    raise ValueError('the donor model has no mesh %s' % name)


def load_templates(archives, templates, texture_sources):
    """Material templates from the donor's files. templates: {kind: (entry name or key, mesh name, polygon)};
    texture_sources: NUTs (names or keys) holding the templates' textures (a later one wins).
    -> {kind: (material, vertex type, slots [{'fmt', 'mean' RGBA or None}])}"""
    def read(ref):
        return archives.read(key_of(ref) if isinstance(ref, str) else ref)
    found = {}
    for src in texture_sources:
        for t in nut.read(read(src))['textures']:
            found[t['gidx']] = t
    models, out = {}, {}
    for kind, (src, mesh_name, poly_index) in templates.items():
        if src not in models:
            models[src] = nud.read(read(src))
        poly = find_mesh(models[src], mesh_name)['polys'][poly_index]
        slots = []
        for t in poly['materials'][0]['textures']:
            tex = found.get(t['id'])
            mean = nut.decode(tex).reshape(-1, 4).mean(0).round().astype(int) if tex is not None else None
            slots.append(dict(fmt=tex['fmt'] if tex is not None else 0, mean=mean))
        out[kind] = (copy.deepcopy(poly['materials'][0]), poly['vtype'], slots)
    return out


ORDER = {'mouth': 0, 'skin': 0, 'cloth': 0, 'colour': 0.5, 'eye': 1, 'hair': 2, 'overlay': 3}   # draw order in a mesh
WRAP_REPEAT = 1                 # a NUD texture's wrap mode that repeats (the templates' 3 clamps)
HOLES = 0.02                    # cloth whose colour texture is transparent over more than this share has holes (lace)


def classify(name, base, alpha):
    """What a Tekken 7 material becomes when the definition's MATERIALS does not say: (template kind, normal map?,
    alpha-tested?) or None (left out), from its parent material `base` and its name; alpha: its colour texture's
    alpha (None: opaque)."""
    n = name.lower()
    if base in ('M_CH_Eye_ADD', 'M_CH_tear') or 'tear' in n.split('_') or n.endswith('_add'):
        return None                                   # reflections and wet layers over the eyes
    if base == 'M_CH_Eye_IN' or n.endswith(('_eyes', '_eye')) or '_eyes_' in n:
        return ('eye', False, False)
    if 'teeth' in n:
        return ('mouth', False, False)
    if 'mouth' in n or 'tongue' in n:
        return ('mouth', True, False)
    if base == 'M_CH_skin_wet_SSP' or '_skin_' in n:
        return ('skin', True, False)
    if base in ('M_CH_trans', 'M_CH_simple_trans', 'M_CH_lip'):
        return ('overlay', False, True)                # eyebrows, lashes, shadow, lips: blended over the face
    if base.startswith('M_CH_AnisoHair'):
        return ('hair', False, True)
    holes = alpha is not None and float((alpha < 0.5).mean()) > HOLES
    return ('cloth', True, holes)
VERTEX_BUDGET = 42000           # vertices of a fighter's model the game skins: each frame it streams the skinned
                                # vertices of the fighters on screen (and some stages' animals) into a stream-out
                                # buffer, 80 bytes each, and in a match's first frame those of all four (the port
                                # makes the buffers 28 MiB: roster.cpp; its own fighters have 20-34k): over it, 2P
                                # colour layers are left out, the largest first (a definition's VERTEX_BUDGET: its
                                # own, Noctis's weapons)
LAYER_OFFSET = 1.0              # mm: how far the 2P colour layer sits outside the polygon it covers (less flickers:
                                # the depth buffer cannot tell them apart at fighting distance)


def convert(paks, archives, d, slot=0, log=print):
    """d: the character definition, slot: its number (pack.place). -> (nud bytes, nut bytes, bon bytes, colours):
    colours = None without 2P colour layers (colours.py), else {palette number: (player 1's rgb, player 2's rgb)}
    (shared.py puts them in the colour grid)."""
    t7 = d.TEKKEN7
    follow = getattr(d, 'DEPENDENCY_ITEMS', False)     # (a definition's: items worn with others, Noctis's trousers)
    items = costume_items(paks, t7['costume'], follow)
    extra = dependency_parts(items, t7['parts']) if follow else []
    for tag, path in extra:
        log('  %s: worn with the costume (an item another depends on)' % path.split('/')[-1])
    worn = worn_parts(list(t7['parts']) + extra, dependency_meshes(items))
    for (_, path), (_, now) in zip(t7['parts'], worn):
        if now != path:
            log('  %s: drawn as %s (worn under the costume\'s other items)' % (path.split('/')[-1], now.split('/')[-1]))
    k = load_tekken7_model(paks, worn, list(t7['merge_order']) + [tag for tag, _ in extra])
    log('  Tekken 7 model: %d bones, %d sections' % (len(k['bones']['names']), len(k['prims'])))
    chains = [(part, hanging.find(k, part, root), directions) for part, root, directions in getattr(d, 'CHAINS', [])]
    if chains:                                   # hair and tails Tekken 7 simulates: posed hanging (chains.py)
        moved = hanging.drape(k, [(bones, directions) for _, bones, directions in chains])
    donor = bone.read(archives.read_name(d.DONOR + '.bon'))
    skel = skeleton.build(k['bones'], donor)
    weight_maps = {tag: skeleton.weight_map(k['bones'], skel['names'], lambda i, tag=tag: tag)[0]
                   for tag, _ in worn}
    if chains:                                   # ... and swinging extra bones
        parents = [int(weight_maps[part][k['bones']['parents'][bones[0]]]) for part, bones, _ in chains]
        skel, made = hanging.append_bones(skel, k, [bones for _, bones, _ in chains], parents)
        for (_, bones, _), new in zip(chains, made):
            for wm in weight_maps.values():
                wm[bones] = new
        log('  hanging chains: %s (%d bones, OPT %d..; %d vertices moved)' % (
            ', '.join(root for _, root, _ in d.CHAINS), sum(map(len, made)), hanging.OPT_FIRST, moved))
    hidden = None
    if getattr(d, 'HIDE', None):                 # chains left out: the triangles skinned to them
        bones = [b for part, root in d.HIDE for b in hanging.find(k, part, root)]
        s0 = k['prims'][0]
        hidden = ((np.isin(s0['joints'], bones) & (s0['weights'] > 0)) * s0['weights']).sum(1) > 0.5
        log('  left out: %s (%d vertices)' % (', '.join(root for _, root in d.HIDE), int(hidden.sum())))
    covered = {}                                 # part -> its vertices in front of an item worn over it (COVERED)
    for part, material in getattr(d, 'COVERED', []):
        s0 = k['prims'][0]
        tris = np.concatenate([p['tris'] for p in k['prims'] if p['material'] == material])
        if hidden is not None:
            tris = tris[~hidden[tris].any(1)]
        front = in_front(s0['pos'], s0['pos'][tris])
        covered[part] = front if part not in covered else covered[part] | front
        log('  %s: %d triangles in front of %s left out' % (part, sum(
            int(front[p['tris']].any(1).sum()) for p in k['prims'] if p['part'] == part), material))
    parts, prims = list(worn), list(k['prims'])
    sockets = [('item%d' % n, path, offsets) for n, (path, offsets) in enumerate(socket_items(items))]
    for tag, path, offsets in sockets:           # static meshes worn on the head (glasses): rigid on its Head joint
        prims += load_socket_item(paks, k, tag, path, offsets)
        weight_maps[tag] = skeleton.weight_map(k['bones'], skel['names'], lambda i, tag=tag: tag)[0]
        log('  %s: worn on %s (offsets %s)' % (path.split('/')[-1],
                                               skel['names'][weight_maps[tag][k['bones']['names'].index('Head')]],
                                               offsets))
    if getattr(d, 'TIGER', None):                # a summoned model: extra bones of the fighter's (tiger.py)
        tk = load_tekken7_model(paks, [('tiger', d.TIGER['mesh'])], ['tiger'])
        skel, first = tiger.append_bones(skel, tk['bones'])
        weight_maps['tiger'] = first + np.arange(len(tk['bones']['names']))
        parts.append(('tiger', d.TIGER['mesh']))
        prims += [dict(p, pos=tiger.hidden(p['pos'])) for p in tk['prims']]
        log('  tiger: %d extra bones (OPT %d..), %d sections' % (len(tk['bones']['names']), tiger.OPT_FIRST,
                                                               len(tk['prims'])))
    mesh_order = dict(d.MESHES)
    for n, (tag, _) in enumerate(extra):
        mesh_order.setdefault(tag, ('%s_%s' % (d.CODE, tag), 'KOSHI_jnt', 40 + n))
    for n, (tag, _, _) in enumerate(sockets):
        mesh_order.setdefault(tag, ('%s_%s' % (d.CODE, tag), 'ATAMA_jnt_assist', 50 + n))
    loaded = {}                                  # weapon meshes by path
    for n, w in enumerate(getattr(d, 'WEAPONS', [])):   # weapons its animations pose: extra bones (weapons.py)
        tag = 'weapon%d' % n
        if w.get('mesh'):
            if w['mesh'] not in loaded:
                loaded[w['mesh']] = load_tekken7_model(paks, [(tag, w['mesh'])], [tag])
            wk = loaded[w['mesh']]
        else:                                    # a weapon only hitting: its bones are the costume's
            wk = k
        skel, index, number, points = weapons.append_bones(skel, wk['bones'], w)
        kept = []
        if w.get('mesh') and w.get('draw', True):
            weight_maps[tag] = np.full(len(wk['bones']['names']), index, np.int64)
            own = wk['bones']['names'].index(w['bone'])
            kept = [dict(q, part=tag) for q in (weapons.triangles(q, own) for q in wk['prims']) if len(q['tris'])]
            parts.append((tag, w['mesh']))
            prims += kept
            mesh_order[tag] = ('%s_wpn%d' % (d.CODE, n), w['parent'], 100 + n)
        log('  weapon %s: OPT %d under %s (%s), hit points OPT %s, %d triangles drawn' % (
            w['bone'], number, w['parent'], 'pair %d' % w['pair'] if w.get('pair') is not None else 'still', points,
            sum(len(q['tris']) for q in kept)))
    names = [n[4:] if n.startswith('cha:') else n for n in skel['names']]
    bidx = {n: i for i, n in enumerate(names)}
    templates = load_templates(archives, d.TEMPLATES, d.TEMPLATE_TEXTURES)
    materials = Materials(paks)
    colours = materials.costume_colours(t7['costume'])
    colours_2p = materials.costume_colours(t7['costume_2p']) if t7.get('costume_2p') else None
    palette = colours2p.Palette(archives.read(colours2p.KEY_GRID),
                                [archives.read(k).decode('utf-8') for k in (colours2p.KEY_1P, colours2p.KEY_2P)]) \
        if colours_2p else None
    textures = Textures(paks)
    mat_paths = {}
    for tag, path in parts:
        m = t7mesh.SkeletalMesh(load(paks, path))
        for x in m.materials:
            if x['material']:
                ref = short_ref(x['material'])
                mat_paths.setdefault(ref.split('.')[-1], ref)
    for _, path, _ in sockets:
        for ref in StaticMesh(load(paks, path)).materials:
            if short_ref(ref):
                mat_paths.setdefault(short_ref(ref).split('.')[-1], short_ref(ref))
    face = sorted((name for name in mat_paths if 'skin_face' in name.lower()), key=len)
    own = None
    if face:                                      # its face's colour picks the gain (skin.py)
        rgb, _ = bake_albedo(materials.effective(mat_paths[face[0]], colours), textures)
        own = rgb.reshape(-1, 3).mean(0) if rgb is not None else None
    skin_gain, skin_hue = skin.gain(paks, archives, d.SEX, materials, lambda eff: bake_albedo(eff, textures), own=own,
                                    log=log)
    if getattr(d, 'SKIN_HUE', None) == 'own':    # (Gigas is red)
        skin_hue = None
    bank = TextureBank(GIDX_BASE + SLOT_GIDX * slot)
    built = {}
    cutoffs = {}                    # hair material -> Tekken 7's alpha threshold (0..255)
    layers, palettes = {}, {}       # material -> its 2P colour layer (fit, palette numbers); the palette entries

    kinds = {}
    flat = getattr(d, 'FLAT', {})
    # Tekken 7 lights its fighters with rim terms besides the colour (Fresnel on M_CH_Base and its kin, its hair's)
    # that TTT2's templates do not have: an outfit and hair as dark as Noctis's (colours 0.02..0.06 of the textures')
    # are black in TTT2, their prints and lighter tips gone; a definition's GAIN {template kind or material name: linear
    # gain} lifts them
    gains = dict(getattr(d, 'GAIN', {}))
    grey = set(getattr(d, 'GREY', ()))

    def bake(name, eff):
        """bake_albedo, or for a material the definition's FLAT names (one without a colour texture: Leroy's lenses)
        that colour parameter of it, flat."""
        if name in flat:
            return np.full((32, 32, 3), np.clip(_col(eff['vectors'][flat[name]]), 0, 1), np.float32), None
        return bake_albedo(eff, textures, grey=name in grey)

    def kind_of(name):
        """The definition's MATERIALS entry, else classify()."""
        if name in d.MATERIALS:
            return d.MATERIALS[name]
        if name not in kinds:
            eff = materials.effective(mat_paths[name], colours)
            try:
                rgb, al = bake(name, eff)
            except (KeyError, ValueError):
                rgb, al = None, None
            kinds[name] = classify(name, eff['base'], al) if rgb is not None else None   # nothing to draw
            log('  material %s (%s): %s' % (name, eff['base'], kinds[name]))
        return kinds[name]

    def build_material(name):
        kind, use_normal, alpha = kind_of(name)
        mat, vtype, slots = templates[kind]
        if name not in built:
            eff = materials.effective(mat_paths[name], colours)
            rgb, al = bake(name, eff)
            if rgb is None:
                raise ValueError('material %s has no colour texture' % name)
            if kind == 'skin':                   # TTT2's skin colours (skin.py)
                rgb = skin.hue(np.clip(rgb * skin_gain, 0, 1), skin_hue)
            elif name in gains or kind in gains:  # (a definition's GAIN)
                rgb = np.clip(rgb * gains.get(name, gains.get(kind)), 0, 1)
            built[name] = albedo_image(rgb, al), eff['textures'].get('Normal') if use_normal else None
            if 'AlphaThreshold' in eff['scalars']:
                cutoffs[name] = int(round(255 * min(max(eff['scalars']['AlphaThreshold'], 0.0), 1.0)))
            if colours_2p is not None and kind == 'cloth' and 'colour' in templates:
                rgb2, _ = bake(name, materials.effective(mat_paths[name], colours_2p))
                rgb2 = np.clip(rgb2 * gains.get(name, gains.get(kind, 1.0)), 0, 1)
                c1 = np.asarray(built[name][0].convert('RGB'), np.float64) / 255.0
                c2 = np.asarray(albedo_image(rgb2, None).convert('RGB'), np.float64) / 255.0
                f = colours2p.fit(c1, c2, palette)
                if f is not None:
                    nn = colours2p.FIRST_PALETTE + 2 * len(layers)
                    layers[name] = (f, (nn, nn + 1))
                    for k, (_, rgb1, _, rgb2) in enumerate(f['groups']):
                        palettes[nn + k] = ([float(x) for x in rgb1], [float(x) for x in rgb2])
        albedo, normal = built[name]
        mat = copy.deepcopy(mat)
        if kind == 'hair' and mat['alpha_test'] and name in cutoffs:
            mat['ref'] = cutoffs[name]  # where Tekken 7 cuts its strands (the template's own cut is higher)
        if kind == 'cloth' and alpha and not mat['alpha_test'] and 'colour_alpha' in templates:
            for key in ('alpha_test', 'alpha_func', 'ref'):    # cloth with holes (lace, frills): alpha-tested
                mat[key] = templates['colour_alpha'][0][key]
        ids = []
        for i, slot in enumerate(slots):
            if i == 0:
                ids.append(bank.image(name, albedo, 2 if alpha else 0, alpha))
            elif i == 1 and slot['mean'] is not None and abs(slot['mean'][1] - 128) < 8 and \
                    abs(slot['mean'][3] - 128) < 8:
                # a normal map slot (R = 0, G = Y, B = 0, A = X)
                if textures.exists(normal):
                    ids.append(bank.normal(normal, Image.fromarray(textures.get(normal)[0])))
                else:
                    ids.append(bank.constant((0, 128, 0, 128), 2))
            else:
                mean = slot['mean'] if slot['mean'] is not None else np.array([128, 128, 128, 255])
                ids.append(bank.constant(tuple(mean), slot['fmt']))
        for t, gid in zip(mat['textures'], ids):
            t['id'] = gid
            t['wrap_s'] = t['wrap_t'] = WRAP_REPEAT        # Tekken 7 repeats textures (UVs run past 0..1)
        return mat, vtype, kind

    def layer_material(name):
        """The colourable, alpha-tested material of a 2P colour layer."""
        f, nns = layers[name]
        albedo, normal = built[name]
        mat, vtype, slots = templates['colour']
        mat = copy.deepcopy(mat)
        alpha_from = templates.get('colour_alpha', templates['colour'])[0]
        for key in ('src', 'dst', 'alpha_test', 'alpha_func', 'ref'):
            mat[key] = alpha_from[key]
        size = pow2_size(*albedo.size)

        def image(a, mode):
            return np.asarray(Image.fromarray(a, mode).resize(size, Image.LANCZOS))
        grey = (np.clip(f['shade'], 0, 1) * 255 + 0.5).astype(np.uint8)
        shade = np.stack([image(grey, 'L')] * 3 + [image((f['alpha'] * 255).astype(np.uint8), 'L')], -1)
        mask = np.zeros(shade.shape, np.uint8)
        mask[..., 0] = 255
        mask[..., 2] = image((f['b'] * 255).astype(np.uint8), 'L')
        mask[..., 3] = 255
        ids = []
        for i, slot in enumerate(slots):
            if i == 0:
                ids.append(bank.add(('layer-shade', name), shade, 2))
            elif i == 1 and textures.exists(normal):
                ids.append(bank.normal(normal, Image.fromarray(textures.get(normal)[0])))
            elif i == 3:
                ids.append(bank.add(('layer-mask', name), mask, 0))
            else:
                mean = slot['mean'] if slot['mean'] is not None else np.array([128, 128, 128, 255])
                ids.append(bank.constant(tuple(mean), slot['fmt']))
        for t, gid in zip(mat['textures'], ids):
            t['id'] = gid
            t['wrap_s'] = t['wrap_t'] = WRAP_REPEAT        # Tekken 7 repeats textures (UVs run past 0..1)
        (_, a1, _, _), (_, b1, _, _) = f['groups']
        props = []
        for n, v in mat['props']:
            if n == 'NU_vReg_04':
                v = list(a1) + [float(colours2p.ID_BASE + nns[0])]
            elif n == 'NU_vReg_07':
                v = list(b1) + [float(colours2p.ID_BASE + nns[1])]
            props.append((n, [float(x) for x in v]))
        mat['props'] = props
        return mat, vtype

    meshes, dropped = {}, []
    seen = set()                    # the triangles drawn so far (a hair material's second pass draws them again)
    for prim in prims:
        name = prim['material']
        if kind_of(name) is None:
            dropped.append(name)
            continue
        if hidden is not None and len(hidden) == len(prim['pos']):
            prim = dict(prim, tris=prim['tris'][~hidden[prim['tris']].any(1)])
            if not len(prim['tris']):
                continue
        if prim['part'] in covered and len(covered[prim['part']]) == len(prim['pos']):
            prim = dict(prim, tris=prim['tris'][~covered[prim['part']][prim['tris']].any(1)])
            if not len(prim['tris']):
                continue
        corners = np.sort(np.round(prim['pos'][prim['tris']], 1).reshape(len(prim['tris']), -1), 1)
        shape = hash(corners[np.lexsort(corners.T[::-1])].tobytes())
        if shape in seen:
            dropped.append(name + ' (a second pass)')
            continue
        seen.add(shape)
        mat, vtype, kind = build_material(name)
        # Tekken 7 splits vertices its meshes share (copies with the same position, normal, uv and skin weights);
        # they are welded: the game skins a fighter's vertices into a buffer made for its own fighters' (~30k)
        used = np.unique(prim['tris'])
        same = np.concatenate([np.round(prim['pos'][used], 3), np.round(prim['nrm'][used], 4),
                               np.round(prim['uv'][used], 6), weight_maps[prim['part']][prim['joints'][used]],
                               np.round(prim['weights'][used], 4)], 1)
        _, first, which = np.unique(same, axis=0, return_index=True, return_inverse=True)
        remap = np.full(len(prim['pos']), -1, np.int64)
        remap[used] = which.reshape(-1)
        tris = remap[prim['tris']]
        tris = tris[(tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])]
        used = used[first]
        n = len(used)
        if n >= 0xFFFF:
            raise ValueError('section of %s has too many vertices for one NUD polygon' % name)
        pos = prim['pos'][used]
        nrm = prim['nrm'][used]
        # winding: TTT2's front faces are counter-clockwise around the vertex normal; orient by the normals
        g = np.cross(pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]])
        if float(((g * nrm[tris].sum(1)).sum(1) < 0).mean()) > 0.5:
            tris = tris[:, [0, 2, 1]]
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        uv = prim['uv'][used]
        # skin weights: Tekken 7 bones -> TTT2 bones, merged, the four largest, renormalised
        tj = weight_maps[prim['part']][prim['joints'][used]]
        tw = prim['weights'][used]
        bones = np.zeros((n, 4), np.int64)
        wts = np.zeros((n, 4))
        for v in range(n):
            acc = {}
            for j, w in zip(tj[v], tw[v]):
                if w > 0:
                    acc[int(j)] = acc.get(int(j), 0.0) + float(w)
            top = sorted(acc.items(), key=lambda x: -x[1])[:4]
            s = sum(w for _, w in top)
            for q, (j, w) in enumerate(top):
                bones[v, q] = j
                wts[v, q] = w / s
            for q in range(len(top), 4):
                bones[v, q] = top[0][0]
        # tangent frame: TTT2's tangent is +dP/du, its bitangent -dP/dv
        tan = prim['tan'][used][:, :3]
        tan = tan - nrm * (tan * nrm).sum(1, keepdims=True)
        tan /= np.maximum(np.linalg.norm(tan, axis=1, keepdims=True), 1e-9)
        a, b, c = tris.T
        e1, e2 = pos[b] - pos[a], pos[c] - pos[a]
        du1, dv1 = uv[b, 0] - uv[a, 0], uv[b, 1] - uv[a, 1]
        du2, dv2 = uv[c, 0] - uv[a, 0], uv[c, 1] - uv[a, 1]
        det = du1 * dv2 - du2 * dv1
        ok = np.abs(det) > 1e-12
        dPdv = (e2 * du1[:, None] - e1 * du2[:, None]) / np.where(ok, det, 1)[:, None]
        acc = np.zeros((n, 3))
        for col in (a, b, c):
            np.add.at(acc, col[ok], dPdv[ok])
        bit = np.cross(nrm, tan)
        bit *= np.where((bit * -acc).sum(1) >= 0, 1.0, -1.0)[:, None]
        # the template's vertex type when it has float bone weights, else 0x13 (u32 bones, f32 weights,
        # f32 normal / bitangent / tangent)
        vt = vtype if (vtype & 0xF0) == 0x10 else 0x13
        verts = np.zeros(n, nud.vertex_dtype(vt, 0x10))
        verts['pos'] = pos
        if 'pw' in verts.dtype.names:
            verts['pw'] = 100.0                      # what the game's files store
        verts['nrm'][:, :3] = nrm
        verts['nrm'][:, 3] = 1.0
        if 'tan' in verts.dtype.names:
            verts['tan'][:, :3] = tan
            verts['tan'][:, 3] = 1.0
            verts['bitan'][:, :3] = bit
            verts['bitan'][:, 3] = 1.0
        verts['bones'] = bones
        verts['w'] = wts
        verts['uv'][:, 0, :] = uv
        strip = strips.stripify(tris)
        if not strips.check(tris, strip) or len(strip) >= 0x10000:
            raise ValueError('could not make triangle strips for %s' % name)
        meshes.setdefault(prim['part'], []).append(dict(vtype=vt, uvtype=0x10, pflag=4, tail=bytes(12),
                                                        indices=np.array(strip, np.uint16), verts=verts,
                                                        materials=[mat], _order=ORDER[kind]))
        if name in layers:
            lmat, lvtype = layer_material(name)
            lverts = verts.copy()
            lverts['pos'] = pos + nrm * LAYER_OFFSET
            meshes[prim['part']].append(dict(vtype=vt, uvtype=0x10, pflag=4, tail=bytes(12),
                                             indices=np.array(strip, np.uint16), verts=lverts, materials=[lmat],
                                             _order=ORDER['colour'], _layer=name))
    total = sum(len(p['verts']) for polys in meshes.values() for p in polys)
    by_size = collections.Counter()
    for polys in meshes.values():
        for p in polys:
            if p.get('_layer'):
                by_size[p['_layer']] += len(p['verts'])
    def used_textures():
        return {t['id'] for polys in meshes.values() for p in polys for m in p['materials'] for t in m['textures']}

    plain = []
    budget = getattr(d, 'VERTEX_BUDGET', VERTEX_BUDGET)
    for name, n in by_size.most_common():
        if total <= budget:
            break
        for tag in meshes:
            meshes[tag] = [p for p in meshes[tag] if p.get('_layer') != name]
        total -= n
        plain.append(name)
        layers.pop(name, None)
    if plain:
        log('  over %d vertices: no 2P colour layers over %s (player 2 has player 1\'s colours there)' % (
            budget, ', '.join(plain)))
    # the textures the materials use (a left-out layer's are not), numbered again
    renumber = bank.keep(used_textures())
    slots = {id(t): t for polys in meshes.values() for p in polys for m in p['materials'] for t in m['textures']}
    for t in slots.values():
        t['id'] = renumber[t['id']]
    for polys in meshes.values():
        for p in polys:
            p.pop('_layer', None)
    nmeshes = []
    for tag in sorted(meshes, key=lambda p: mesh_order[p][2]):
        mname, singlebind, _ = mesh_order[tag]
        polys = sorted(meshes[tag], key=lambda p: p['_order'])
        for p in polys:
            del p['_order']
        sph = nud.bounding_sphere(np.concatenate([p['verts']['pos'] for p in polys]))
        nmeshes.append(dict(name=mname, sphere=sph, center=sph[:3], sort_bias=0.0, zero=0, boneflag=0x2c,
                            singlebind=bidx[singlebind], polys=polys))
    allp = np.concatenate([p['verts']['pos'] for m in nmeshes for p in m['polys']])
    before = sum(len(m) for t in bank.textures for m in t['mips'])
    halved = bank.fit(TEXTURE_BUDGET)
    if halved:
        log('  textures: %d KB, more than the game\'s own fighters have: %d halved (%d KB)' % (
            before // 1024, halved, sum(len(m) for t in bank.textures for m in t['mips']) // 1024))
    if len(allp) > budget:
        log('  WARNING: %d vertices: more than the game skins for a fighter (about %d); the other fighters in a fight '
            'may break' % (len(allp), budget))
    model = dict(version=0x0201, sphere=nud.bounding_sphere(allp), bone_first=None, meshes=nmeshes)
    log('  model: %d meshes, %d polygons, %d vertices, %d textures (dropped: %s)' % (
        len(nmeshes), sum(len(m['polys']) for m in nmeshes), len(allp), len(bank.textures),
        ', '.join(dropped) or 'none'))
    if layers:
        log('  2P colours: layers over %s; palette numbers %s' % (', '.join(sorted(layers)), sorted(palettes)))
    out = palettes if layers else None
    return nud.write(model), nut.write(bank.textures), bone.write(skel), out
