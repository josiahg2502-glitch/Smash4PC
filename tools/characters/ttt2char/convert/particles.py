"""Tekken 7 particle systems as TTT2 effect files (ALO), with their textures (NUT) and meshes (NUD).

A Tekken 7 move spawns an effect by id at a body part (0x8036, part << 8 | id); the fighter's effect item
(ECI_<CODE>_unique_0) names the particle system of each id. Here each system becomes one ALO; each of its emitters
one or more ALO emitters (one for the steady spawn rate, one per burst), made from game emitters (a sprite: the
common smoke's; a mesh: the common hit's) with their values replaced:

    Tekken 7 (cm, s, X forward, Y right, Z up)         TTT2 (mm, frames at 60/s, x left, y up, z forward)
    Required: duration x loops, delay                  emitter +64 frames it emits for, +68 delay
    Spawn: rate, bursts                                emitter +60 interval, count track (particle block +52)
    Lifetime                                           spawner +244 life (+246 random range)
    Size, SizeMultiplyLife                             spawner +148/+152 full width/height (+156/+160 random),
                                                       size track +48 (keys in particle-age frames)
    Color, ColorOverLife, ColorScaleOverLife           colour track +0, alpha track +24, emitter +36 RGB scale
                                                       (TTT2 colours stay <= 1: the brightest part is the scale)
    Rotation(Rate), MeshRotation(Rate) (turns)         spawner +164.. rotation, +188.. spin (+176../+200.. random)
    Velocity (+ over life, drag, acceleration)         speed tracks +84/+96/+108 in the emitter frame (particle
                                                       flag 0x40: fixed direction), keyed over the particle's age
    Location, the character-bone offset                particle-block position track +64 (+76 random range)
    LocationPrimitiveSphere / Cylinder                 the position's random range (+76)
    bUseLocalSpace                                     spawner flag 0x1 (particles follow the emitter)
    material (base texture x mask, blend mode)         one texture (alpha = shape), spawner +236 blend
    TypeDataMesh                                       a model particle (type 3) whose emitter name is the NUD
                                                       (one scale for all axes: the axes' proportions are baked
                                                       into the model)

TTT2 puts an effect spawned at a body part in that bone's own frame; Tekken 7 in the fighter's level facing frame
at the bone. The converted effects carry LEVEL_MARK in their root, for which the runtime (roster.cpp, level effects)
places them as Tekken 7 does; a projectile's look also carries PROJECTILE_MARK and rides the projectile. Measured in
game; what Cascade does that ALO cannot (material graphs, distortion, vector fields,
camera-facing meshes) is approximated or left out.

Written for this project from the files themselves (no third-party code).
"""
import collections
import copy
import math
import re
import struct

import numpy as np

from ..tekken7 import mesh as t7mesh, staticmesh, texture as t7tex
from ..tekken7.particles import ParticleSystem, EffectMaterials, effect_table, missile_systems
from ..tekken7.material import short_ref
from ..tekken7.uasset import load
from ..ttt2 import alo, nud, nut
from . import strips

FPS = 60.0
CM = 10.0                          # mm per Tekken 7 centimetre
TURN = 2 * math.pi                 # Cascade rotations are in turns
MAX_COLOUR_SCALE = 4.0             # the game's coloured auras go up to 4
MAX_EMITTERS = 64                  # per effect file
MAX_PER_FRAME = 4                  # particles per emission
LOOP_CAP = 2.0                     # seconds an endlessly looping Tekken 7 emitter runs (the move's effect ends it)
GAMMA = 2.2                        # Tekken 7's sRGB textures and display, as a power
EXPOSURE = 1.0                     # brightness of Tekken 7's effects before its tone mapping (by eye, against the
                                   # game's own pictures)
MAX_ALPHA_GAIN = 16.0              # particle opacity above 1 thickens a shape up to this
TEXTURE_SIDE = 128                 # largest texture side (effects are soft; a fighter's effects share a
                                   # memory budget with the stage: 11 MB of them left a stage unloaded)
SHEET_SIDE = 256                   # ... of a sheet of sub-images (flipbooks)
CURVE_KEYS = 8                     # keys per curve
MODEL_BUDGET = 40                  # meshes a fighter may draw as models (model_budget)
# Mesh particles of a fighter's costume item meshes (Noctis's weapons glowing as he summons and dismisses them: 0.3 to
# 0.9 MB each, 10 MB in all, more than the effect library holds: the match did not load) are left out; the fighter's
# model draws the item itself (convert/weapons.py)
ITEM_MESHES = '/Character/Item/Meshes/'
FIRST_ID = 0x800                   # the effect textures' ids: after the fighter's model's (model.SLOT_GIDX each)

# game emitters the converted ones are made from (spawner flags, particle flags, blend word)
SPRITE_TEMPLATE = ('cmn/alo/eff_smoke00.alo', 0)
MODEL_TEMPLATE = 0x9F462002            # the common hit's tube (data000): a model emitter that starts by itself
BLEND_ADD, BLEND_ALPHA = 0x0010C089, 0x0010C049
MODEL_BLEND_ADD, MODEL_BLEND_ALPHA = 0x02000089, 0x02000049
# a model particle draws its emitter's texture slot, blended as its model's material says (src, dst): the game's
# effect models use (1, 2) additive, (1, 1) alpha-blended, (0, 0) opaque
MODEL_ADD, MODEL_ALPHA, MODEL_OPAQUE = (1, 2), (1, 1), (0, 0)
VERTEX_WHITE = (255, 255, 255, 255)  # an effect model's vertex colour multiplies it as is (the game's 127: half bright)
PFLAG_DIRECTED = 0x27043           # particle flags: fixed directions (0x40), the game's usual model/sprite bits
SFLAG_SPRITE = 0x30400             # spawner flags: camera-facing sprite (0x400), one random for both sizes
SFLAG_MODEL = 0x30000
SFLAG_LOCAL = 0x1                  # particles follow the emitter
ROOT_AUTOSTART = 4                 # root s16 +132: the effect starts by itself (0: it waits for a signal)
LEVEL_MARK = (700, 0x54374658)     # root u32 (zero in the game's files): the runtime places the effect in the
                                   # fighter's level facing frame at its bone, as Tekken 7 does (roster.cpp)
PROJECTILE_MARK = (704, 0x50524F4A)  # ... and this one where the fighter's newest projectile is, while it is
FLOOR_MARK = (708, 0x464C4F52)     # ... and this one at the fighter's floor (Tekken 7: FloorHeight), height 0 there
FLOOR_SUFFIX = '_floor'            # the second file of an effect: its emitters placed from the floor


def to_ttt2(v):
    """A Tekken 7 effect-frame vector (X forward, Y right, Z up) -> TTT2 (x left, y up, z forward)."""
    return [-v[1], v[2], v[0]]


def frotation(pitch, yaw, roll):
    """The engine's rotation matrix of a rotator (degrees; roll about X, then pitch about Y, then yaw about Z) for
    row vectors in its axes: v' = v @ M."""
    sp, cp = math.sin(math.radians(pitch)), math.cos(math.radians(pitch))
    sy, cy = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    sr, cr = math.sin(math.radians(roll)), math.cos(math.radians(roll))
    return np.array([[cp * cy, cp * sy, sp],
                     [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
                     [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp]])


def emitter_frame(req):
    """(rotation, origin) of an emitter in its system (Required EmitterRotation, a rotator, and EmitterOrigin, cm):
    the engine places spawn offsets and start velocities in it (origin + offset @ rotation); meshes keep their own
    orientation (Geese's AirBullet windline_06: rotation yaw 180, its mesh already pointing forward)."""
    r = (req.get('EmitterRotation') if req is not None else None) or {}
    o = (req.get('EmitterOrigin') if req is not None else None) or {}
    if isinstance(o, dict):
        o = [o.get('x', 0.0), o.get('y', 0.0), o.get('z', 0.0)]
    rot = None
    if any(abs(float(r.get(k) or 0.0)) > 1e-6 for k in ('pitch', 'yaw', 'roll')):
        rot = frotation(float(r.get('pitch') or 0.0), float(r.get('yaw') or 0.0), float(r.get('roll') or 0.0))
    return rot, [float(x or 0.0) for x in o]


def in_frame(v, rot):
    """A vector of an emitter's frame in its system's (emitter_frame's rotation; None: the same)."""
    return list(np.asarray(v, float) @ rot) if rot is not None else list(v)


def turns_to_ttt2(v):
    """A Cascade mesh rotation (turns: roll about X, pitch about Y, yaw about Z; the engine's pitch lifts the nose,
    its yaw and roll turn clockwise) -> TTT2's angles about x, y, z (radians, right-handed in its frame: measured
    in game, a +90 degree turn about x lifts a backward mesh up)."""
    return [-v[1] * TURN, -v[2] * TURN, -v[0] * TURN]


def _set(blk, fmt, off, *vals):
    raw = bytearray(blk['raw'])
    struct.pack_into(fmt, raw, off, *vals)
    blk['raw'] = bytes(raw)


def _u16(x):
    return max(0, min(0xFFFF, int(round(x))))


def _keys(points, n=CURVE_KEYS):
    """[(frame, values...)] thinned to at most n keys (first and last kept), frames rounded and made distinct."""
    pts = sorted(points, key=lambda p: p[0])
    if len(pts) > n:
        idx = sorted({round(i * (len(pts) - 1) / (n - 1)) for i in range(n)})
        pts = [pts[i] for i in idx]
    out = []
    for p in pts:
        f = int(round(p[0]))
        if out and f <= out[-1][0]:
            if f == out[-1][0]:
                out[-1] = [f] + list(p[1:])
            continue
        out.append([f] + list(p[1:]))
    return out or [[0] + list(points[0][1:])]


class Textures:
    """The effect textures of a fighter: Tekken 7 textures baked to RGBA, written as single-texture NUT files with
    texture ids of the fighter's range (`first`, counting up)."""

    def __init__(self, paks, first):
        self.paks = paks
        self.next_id = first
        self.files = {}            # file name -> nut bytes
        self.sources = {}          # material_source key -> float RGBA
        self._by_key = {}          # (texture paths, mode) -> (file name, gidx, w, h)

    def image(self, path):
        return t7tex.Texture2D(load(self.paks, short_ref(path).split('.')[0])).decode()

    def linear_image(self, path):
        """A texture as float RGBA 0..1, its colour linear (sRGB textures decoded)."""
        t = t7tex.Texture2D(load(self.paks, short_ref(path).split('.')[0]))
        img = t.decode().astype(np.float32) / 255.0
        if t.srgb:
            img[..., :3] = img[..., :3] ** GAMMA
        return img

    def lit(self, src, peak, peak_alpha, additive):
        """The texture an emitter draws: its source (Source) lit as Tekken 7 draws it at the emitter's brightest
        (particle colour `peak`, opacity `peak_alpha`; both may exceed 1), tone-mapped (lit_texels). Emitters whose
        peaks are close share one."""
        q = lambda x: round(math.log2(max(x, 1.0 / 64)) * 4) / 4                   # quarter steps of brightness
        p = tuple(2.0 ** q(x) if x > 1.0 / 128 else 0.0 for x in peak)
        pa = 2.0 ** q(peak_alpha)
        key = ('lit', src.key, p, pa, additive)
        if key not in self._by_key:
            self.bake(key, np.round(lit_texels(src.img, p, min(pa, MAX_ALPHA_GAIN), additive) * 255.0), src.side)
        return self._by_key[key]

    def bake(self, key, rgba, largest=TEXTURE_SIDE):
        if key in self._by_key:
            return self._by_key[key]
        h, w = rgba.shape[:2]
        side = lambda x: max(16, min(largest, 1 << int(math.ceil(math.log2(max(x, 1))))))
        tw, th = side(w), side(h)
        from PIL import Image
        img = np.asarray(Image.fromarray(rgba.astype(np.uint8), 'RGBA').resize((tw, th), Image.LANCZOS))
        gidx = self.next_id
        self.next_id += 1
        name = 'tfm_t7_%04x.nut' % (gidx & 0xFFFF)
        self.files[name] = nut.write([nut.make_texture(img, gidx, fmt=2, min_size=4)])
        self._by_key[key] = (name, gidx, tw, th)
        return self._by_key[key]


def _grey(rgb):
    """Whether a (linear) colour image is grey: a mask whose colour comes from the particle."""
    d = (np.clip(rgb, 0.0, 1.0) ** (1.0 / GAMMA) * 255.0).astype(np.float32)
    return float(np.ptp(d.reshape(-1, 3).mean(0))) < 8 and float(np.abs(d[..., 0] - d[..., 2]).mean()) < 6


MAIN_TEXTURES = ('BaseTexture', 'Diffuse Texture', 'base', 'MaterialExpressionTextureSampleParameter2D_0',
                 'SubUVTexture')
# Tekken 7 effects that only bend what is behind them (heat haze, shock rings): normal-map textures, refraction
# materials. TTT2's effects cannot refract; such emitters are left out (drawn, they show the normal map's colours).
DISTORTION_TEXTURE = re.compile(r'_NOR_|nomal|normal|distortion|distotion', re.I)
DISTORTION_MATERIAL = re.compile(r'_Ref_|Refraction|Distortion_only', re.I)
MASK_TEXTURES = ('MaskTexture', 'All AlphaMask Texture', 'AlphaMask Texture')
WHITE = 'T_EF__COL_white8'
WHITE_SIDE = 32                    # a plain white texture's side (smaller ones are below a BC block: a crash)


class Source:
    """What a material draws before the particle's colour: linear colour and the shape in alpha (float RGBA 0..1),
    the largest side it is baked at, its colour factor and extra colour (_power) and texture scroll (_scroll)."""

    def __init__(self, key, img, side, power=(1.0, None), scroll=(0.0, 0.0), subtractive=False):
        self.key, self.img, self.side, self.power, self.scroll = key, img, side, power, scroll
        self.subtractive = subtractive  # the texture is lit by 1 - the particle colour (Converter._colour)


def material_source(textures, mat, sheet=False):
    """A material's colour and shape (Source), or None when it has no texture of its own. Tekken 7's effect
    textures keep the shape in alpha when they have one (else in the brightness, the colour then at full
    brightness); grey ones become white so that the particle colour tints them. The masks (when the material's
    UseMaskTexture switch is on) multiply the shape; AlphaPower steepens it (the engine's SCurve, which the
    materials call). Materials with the UseSubColor switch take 1 - the particle colour (Eliza's white swirls and
    floor glow are black and nothing in Tekken 7; her green smoke over a cyan texture a dark blue)."""
    tex = {k: v for k, v in mat['textures'].items() if v and not DISTORTION_TEXTURE.search(v.split('/')[-1])
           and 'Dist' not in k}
    main = next((tex[k] for k in MAIN_TEXTURES if tex.get(k) and WHITE not in tex[k]), None)
    if main is None:
        main = next((v for k, v in tex.items() if WHITE not in v and 'Mask' not in k and 'Vanish' not in k), None)
    if main is None:
        return None
    use_mask = mat.get('switches', {}).get('UseMaskTexture', True)
    masks = tuple(tex[k] for k in MASK_TEXTURES if tex.get(k) and WHITE not in tex[k] and tex[k] != main) \
        if use_mask else ()
    tiling = mat['vectors'].get('BaseTexTilling') or {}
    tu, tv = (round(min(4.0, max(0.0, float(tiling.get(c, 1.0) if tiling.get(c) is not None else 1.0))), 2)
              for c in 'rg')
    power = float(mat['scalars'].get('AlphaPower') or 1.0)
    side = SHEET_SIDE if sheet else TEXTURE_SIDE
    key = (main, masks, tu, tv, round(power, 3), side)
    if key not in textures.sources:
        try:
            img = _shape(_smaller(textures.linear_image(main), 2 * side))
        except (KeyError, ValueError):
            return None
        for mask in masks:
            try:
                m = _shape(textures.linear_image(mask))[..., 3]
                from PIL import Image
                img[..., 3] *= np.asarray(Image.fromarray(m.astype(np.float32), 'F').resize(
                    (img.shape[1], img.shape[0]), Image.BILINEAR), np.float32)
            except (KeyError, ValueError):
                pass
        if power != 1.0:
            img[..., 3] = s_curve(img[..., 3], min(power, 16.0))
        img = _tiled(img, tu, tv)
        textures.sources[key] = img
    return Source(key, textures.sources[key], side, _power(mat), _scroll(mat), _subtractive(mat))


def _tiled(img, tu, tv):
    """A texture as its material's tiling shows it over UV 0..1: repeated (tiling above 1, whole repeats), a part of
    it stretched (below 1), or, tiling 0, one row or column, which the material's scroll moves through the texture
    (a band passing over a shell: Akuma's hadouken cap): the root mean square along that axis (the plain average
    lets a thin bright band vanish)."""
    from PIL import Image
    for axis, t in ((1, tu), (0, tv)):
        n = img.shape[axis]
        if t < 0.01:
            img = np.repeat(np.sqrt((img ** 2).mean(axis=axis, keepdims=True)), n, axis=axis)
        elif t < 0.99:
            part = img[:, :max(1, int(round(n * t)))] if axis == 1 else img[:max(1, int(round(n * t)))]
            img = np.dstack([np.asarray(Image.fromarray(part[..., c], 'F').resize((img.shape[1], img.shape[0]),
                                                                                    Image.BILINEAR), np.float32)
                             for c in range(4)])
        elif t > 1.01:
            reps = [1, 1, 1]
            reps[axis] = int(round(t))
            img = np.tile(img, reps)
    return img


def _subtractive(mat):
    return bool(mat.get('switches', {}).get('UseSubColor'))


def _smaller(img, largest):
    """An image no larger than `largest` a side (halved by averaging)."""
    while max(img.shape[:2]) > largest and min(img.shape[:2]) >= 2 and img.shape[0] % 2 == 0 and img.shape[1] % 2 == 0:
        img = img.reshape(img.shape[0] // 2, 2, img.shape[1] // 2, 2, 4).mean((1, 3))
    return img


def s_curve(x, power):
    """The engine's SCurve material function: contrast around 0.5, steeper with the power."""
    x = np.clip(x, 0.0, 1.0)
    return np.where(x < 0.5, 0.5 * (2.0 * x) ** power, 1.0 - 0.5 * (2.0 * (1.0 - x)) ** power)


def lit_texels(img, peak, peak_alpha, additive):
    """Tekken 7's look of a Source's texels at a particle colour and opacity, for TTT2's screen. The effect
    materials are unlit: emitted colour = texture colour x particle colour x the material's power (often far above
    1), opacity = texture alpha x particle alpha (above 1 thickens the shape, then clamps); the frame is then tone
    mapped (the engine's filmic curve: Narkowicz's fit of ACES) and shown with gamma. Translucent: (colour,
    opacity); additive: the colour weighed by its opacity, opacity 1 (TTT2 adds colour x alpha)."""
    e = img[..., :3] * np.asarray(peak, np.float32) * EXPOSURE
    a = np.clip(img[..., 3:4] * peak_alpha, 0.0, 1.0)
    if additive:
        e = e * a
        a = np.ones_like(a)
    tm = np.clip(e * (2.51 * e + 0.03) / (e * (2.43 * e + 0.59) + 0.14), 0.0, 1.0)
    return np.concatenate([tm ** (1.0 / GAMMA), a], axis=-1)


def _scroll(mat):
    """The material's texture scroll in UV per second (BaseTexScroll, or the fire materials' Diffuse TexScrol)."""
    v = mat['vectors'].get('BaseTexScroll')
    if v:
        return float(v.get('r') or 0.0), float(v.get('g') or 0.0)
    s = mat['scalars']
    return float(s.get('Diffuse TexScrol(U)') or 0.0), float(s.get('Diffuse TexScrol(V)') or 0.0)


def _particle_scroll(em, mat, life_s):
    """The texture scroll (UV per second) of a material the particle scrolls: Tekken 7's effect shaders offset the
    texture by BaseTexScroll x the particle's dynamic parameter 'Scroll', a curve over its life, with no time term
    (Geese's Reppuu Ken wave: 0.3 -> 0 runs the fire from the crest back along the tail) -> the curve's change over
    the life as a speed (a flat curve: none). None: no such parameter, or BaseTexScroll is not the scroll."""
    v = mat['vectors'].get('BaseTexScroll')
    if not v:
        return None
    for m in em.all('ParameterDynamic'):
        for p in m.get('DynamicParams') or []:
            if p.get('ParamName') != 'Scroll' or p.get('bUseEmitterTime') or \
                    (p.get('ValueMethod') or 'EDPV_UserSet') != 'EDPV_UserSet':
                continue
            table = (p.get('ParamValue') or {}).get('Table') or {}
            values = [float(x) for x in table.get('Values') or []]
            stride = max(1, int(table.get('EntryStride') or 1))     # (a range: low and high an entry)
            entries = [sum(values[i:i + stride]) / stride for i in range(0, len(values) - stride + 1, stride)]
            if not entries:
                continue
            change = (entries[-1] - entries[0]) / max(1.0 / FPS, life_s)
            return float(v.get('r') or 0.0) * change, float(v.get('g') or 0.0) * change
    return None


def _shape(img):
    """Linear RGBA float image -> its colour (white when grey; at full brightness when the brightness is the
    shape) and the shape in alpha."""
    rgb, a = img[..., :3], img[..., 3]
    if float(np.ptp(a)) < 8 / 255.0:     # no shape in alpha: the brightness is the shape
        lum = rgb.max(axis=2)
        a = lum * a.mean()
        rgb = np.clip(rgb / np.maximum(lum[..., None], 1e-3), 0.0, 1.0)
    if _grey(rgb):
        rgb = np.ones_like(rgb)
    return np.dstack([rgb, a]).astype(np.float32)


def _power(mat):
    s = mat['scalars']
    p = 1.0
    for k in ('ColorBoost', 'Color Power', 'ColorPower'):
        v = s.get(k)
        if isinstance(v, (int, float)) and v > 0:
            p *= float(v)
    c = mat['vectors'].get('add_color')
    return min(p, 8.0), ([float(c['r']), float(c['g']), float(c['b'])] if c else None)


class Converter:
    """Converts a fighter's Tekken 7 particle systems (one Converter per fighter; textures and meshes are shared by
    its effects)."""

    def __init__(self, paks, archives, first_texture_id, log=None):
        self.paks, self.archives, self.log = paks, archives, log or (lambda s: None)
        self.materials = EffectMaterials(paks)
        self.textures = Textures(paks, first_texture_id)
        self.models = {}                   # file name -> nud bytes
        self._mesh_names = {}
        self._meshes = {}                  # mesh path -> StaticMesh (or None)
        self.models_allowed = None         # the meshes drawn as models (None: all; see model_budget)
        sdoc = alo.read(archives.read_name(SPRITE_TEMPLATE[0]))
        self.root = copy.deepcopy(sdoc['root'])
        self.sprite = alo.clone_emitter(sdoc, SPRITE_TEMPLATE[1], root=True)
        self.model_tmpl = self._model_template()
        self.nud_template = nud.read(archives.read_name('kaz/alo/model/mfm_kazbeam00.nud'))
        self.stats = {'systems': 0, 'emitters': 0, 'dropped': 0}

    def _model_template(self):
        """A root model emitter: the common hit's tube, with the sprite's texture layers."""
        e = alo.clone_emitter(alo.read(self.archives.read(MODEL_TEMPLATE)), 0, root=True)
        e['spawner']['layers'] = copy.deepcopy(self.sprite['spawner']['layers'])
        return e

    def model_budget(self, paths, budget):
        """Keeps the `budget` meshes the systems at `paths` use most as models; the others become sprites. The
        effect library has room for 200 models in a match (sub_08BF5510's pool), shared by the common effects, the
        stage (its debris) and every fighter, each loaded twice in Practice: over it, models fail to load (a stage
        lost its floor)."""
        use = collections.Counter()
        for path in paths:
            try:
                ps = ParticleSystem(self.paks, path)
            except (KeyError, ValueError):
                continue
            for em in ps.emitters:
                td = em.type_data
                if td is None or td.cls != 'TypeDataMesh' or not td.get('Mesh') or ITEM_MESHES in short_ref(td.get('Mesh')):
                    continue
                mesh = short_ref(td.get('Mesh'))
                if _drawn_as_model(em, self._static_mesh(mesh)):
                    use[mesh] += 1
        self.models_allowed = {m for m, _ in use.most_common(budget)}
        return len(use)

    # -------------------------------------------------------------------------------------------- one system
    def _plain(self, solid, mat):
        """The Source of a material whose textures are all white (its shape comes from its graph): white for a
        mesh (the mesh is the shape), else a soft round glow (most such sprite materials fade radially)."""
        if solid:
            return Source(('white',), np.ones((WHITE_SIDE, WHITE_SIDE, 4), np.float32), WHITE_SIDE, _power(mat),
                          subtractive=_subtractive(mat))
        n = 2 * WHITE_SIDE
        y, x = np.mgrid[0:n, 0:n] + 0.5
        r = np.hypot(x - n / 2, y - n / 2) / (n / 2)
        img = np.ones((n, n, 4), np.float32)
        img[..., 3] = np.clip(1.0 - r, 0.0, 1.0) ** 2
        return Source(('glow',), img, n, _power(mat), subtractive=_subtractive(mat))

    def _doc(self, projectile=False, floor=False):
        """An effect with no emitters yet: it starts by itself, in the fighter's level facing frame at its body part
        (LEVEL_MARK); a projectile's look also rides the projectile (PROJECTILE_MARK); a floor part has its height
        from the fighter's floor (FLOOR_MARK)."""
        doc = {'root': copy.deepcopy(self.root), 'textures': [], 'models': [], 'emitters': [], 'pool': alo.POOL_HEAD,
               'extra': []}
        _set(doc['root'], '>h', 132, ROOT_AUTOSTART)
        _set(doc['root'], '>I', *LEVEL_MARK)
        if projectile:
            _set(doc['root'], '>I', *PROJECTILE_MARK)
        if floor:
            _set(doc['root'], '>I', *FLOOR_MARK)
        return doc

    def _docs(self, projectile=False):
        """An effect's two files: the emitters placed from the body part, those placed from the floor."""
        return self._doc(projectile), self._doc(projectile, floor=True)

    def _add_system(self, docs, path, cap_frames=None):
        """Appends the emitters of a Tekken 7 particle system to docs (_docs; at most MAX_EMITTERS each): those
        Tekken 7 places at the floor's height (FloorHeight, or the character-bone module's FloorHeight) to the
        second."""
        ps = ParticleSystem(self.paks, path)
        for em in ps.emitters:
            doc = docs[1] if _on_floor(em) else docs[0]
            try:
                made = self.emitter(em, cap_frames)
            except Invisible:
                continue
            except (KeyError, ValueError, ZeroDivisionError) as error:
                self.log('effects: %s: emitter %s left out (%s)' % (ps.name, em.name, error))
                self.stats['dropped'] += 1
                continue
            for e, tname, mname in made:
                if len(doc['emitters']) >= MAX_EMITTERS:
                    break
                self._append(doc, e, tname, mname)

    @staticmethod
    def _append(doc, e, tname, mname):
        doc['emitters'].append(e)
        for key, name in (('textures', tname), ('models', mname)):
            if name and name not in [r['name'] for r in doc[key]]:
                doc[key].append({'raw': bytes(alo.REF_SIZE), 'name': name})

    def _write(self, docs):
        """-> {file name suffix: ALO bytes} of the docs with emitters ('' the body part's, FLOOR_SUFFIX)."""
        out = {}
        for suffix, doc in zip(('', FLOOR_SUFFIX), docs):
            if doc['emitters']:
                self.stats['emitters'] += len(doc['emitters'])
                out[suffix] = alo.write(doc)
        self.stats['systems'] += bool(out)
        return out

    def system(self, path, cap_frames=None):
        """-> {file name suffix: ALO bytes} (_write); empty when nothing of the system can be shown. cap_frames:
        Tekken 7 stops a move's effect when the move ends: the frames left of the longest move that spawns it."""
        docs = self._docs()
        self._add_system(docs, path, cap_frames)
        return self._write(docs)

    def projectile(self, path, spec):
        """The look of a projectile: the emitters of the missile's particle system, for spec['life'] frames at
        most, riding the projectile (the runtime puts the effect where the projectile is, every frame, and out of
        sight when it is gone). -> {file name suffix: ALO bytes} (_write)."""
        docs = self._docs(projectile=True)
        self._add_system(docs, path, max(1, int(spec['life'])))
        return self._write(docs)

    def thrown(self, spec, trail=None):
        """The look of a thrown weapon (the definition's THROWN): the weapon's own mesh (spec['item']: the triangles
        of its bone in its Tekken 7 mesh, tip forward) and the Tekken 7 effect it trails (`trail`: a particle system
        path), riding the projectile like projectile(). -> {file name suffix: ALO bytes} (_write)."""
        item = spec['item']
        model, tex = self._item_model(item['mesh'], item['bone'], item.get('tip'))
        if model is None:
            return {}
        life = max(1, int(spec['life']))
        docs = self._docs(projectile=True)
        doc = docs[0]
        e = copy.deepcopy(self.model_tmpl)
        alo.unpin(e)
        sp, pa = e['spawner'], e['particle']
        sp['modifiers'] = []
        _set(sp, '>B', 251, 0)
        _set(sp, '>I', 136, SFLAG_MODEL | SFLAG_LOCAL)
        _set(sp, '>I', 236, MODEL_BLEND_ALPHA)
        _set(pa, '>I', alo.START_FLAGS, PFLAG_DIRECTED)
        _set(pa, '>B', alo.SHAPE, 0)
        pa.pop('shape', None)
        e['name'] = model
        _set(sp, '>HH', alo.LIFE, _u16(life), 0)
        _set(sp, '>ffff', alo.SIZE, 1.0, 1.0, 0.0, 0.0)
        _set(sp, '>12f', 164, *([0.0] * 12))
        _set(sp, '>6f', 212, *([0.0] * 6))
        _set(sp, '>fff', 12, 0.0, 0.0, 0.0)
        _set(sp, '>f', 36, 0.0)
        _set(sp, '>fff', 120, 0.0, 0.0, 0.0)      # no random speeds
        alo.set_emitter_colour(e, [1.0, 1.0, 1.0])
        for o, keys in ((alo.COLOUR_TRACK, [[0, 1.0, 1.0, 1.0]]), (alo.ALPHA_TRACK, [[0, 1.0]]),
                        (alo.SIZE_TRACK, [[0, 1.0]]), (84, [[0, 0.0]]), (96, [[0, 0.0]]), (108, [[0, 0.0]])):
            alo._set_keys(sp['tracks'][o], keys)
        for o in (4, 16, 28, 40):
            alo._set_keys(pa['tracks'][o], [[0, 0.0]])
        alo._set_keys(pa['tracks'][64], [[0, 0.0, 0.0, 0.0]])
        _set(pa, '>fff', 76, 0.0, 0.0, 0.0)
        _set(e, '>HHHHHH', alo.INTERVAL, 0xFFFF, 0, 1, 0, 0, 0)
        alo._set_keys(pa['tracks'][alo.COUNT_TRACK], [[0, 1]])
        self._texture(sp, tex, (0.0, 0.0), {}, life)
        self._append(doc, e, tex[0], model)
        if trail:
            self._add_system(docs, trail, life)
        return self._write(docs)

    def _item_model(self, mesh_path, bone, tip=None):
        """(model file name, texture) of the triangles of `bone` in a Tekken 7 skeletal mesh, centred, lying along
        z with the tip (bone `tip`, else the far end) forward."""
        key = ('item', mesh_path, bone)
        if key in self._mesh_names:
            return self._mesh_names[key]
        sk = t7mesh.SkeletalMesh(load(self.paks, mesh_path))
        g = sk.geometry()
        names = sk.skeleton['names']
        if bone not in names:
            self.log('effects: %s has no bone %s' % (mesh_path.split('/')[-1], bone))
            self._mesh_names[key] = (None, None)
            return self._mesh_names[key]
        b = names.index(bone)
        main = g['joints'][np.arange(len(g['joints'])), np.argmax(g['weights'], 1)]
        tris = [t[(main[t] == b).all(1)] for t in g['triangles']]
        sec = max(range(len(tris)), key=lambda i: len(tris[i]))          # the material holding most of it
        used = np.unique(np.concatenate([t for t in tris if len(t)]))
        p = g['positions'] * CM                                         # mm, UE axes
        centre = (p[used].min(0) + p[used].max(0)) / 2
        q = p - centre
        pos = np.stack([q[:, 0], q[:, 2], q[:, 1]], 1)                    # UE (x, y, z) -> TTT2 (x, z, y): y forward
        ext = pos[used].max(0) - pos[used].min(0)
        axis = int(np.argmax(ext))                                       # its long axis -> z
        perm = {0: [1, 2, 0], 1: [2, 0, 1], 2: [0, 1, 2]}[axis]
        pos = pos[:, perm]
        if tip in names:
            gm = t7mesh.global_matrices(sk.skeleton['pose'], sk.skeleton['parents'])
            t = gm[names.index(tip)][:3, 3] * CM - centre
            t = np.array([t[0], t[2], t[1]])[perm]
            if t[2] < 0:                                                 # the tip forward
                pos[:, 0] *= -1
                pos[:, 2] *= -1
        nrm = g['normals'][:, [0, 2, 1]][:, perm]
        m = copy.deepcopy(self.nud_template)
        mesh = m['meshes'][0]
        poly = mesh['polys'][0]
        dt = poly['verts'].dtype
        sel = used
        remap = -np.ones(len(pos), np.int64)
        remap[sel] = np.arange(len(sel))
        v = np.zeros(len(sel), dt)
        v['pos'] = pos[sel]
        v['nrm'][:, :3] = nrm[sel]
        v['nrm'][:, 3] = 1.0
        if 'color' in dt.names:
            v['color'] = VERTEX_WHITE
        v['uv'][:, 0, :] = g['uvs'][sel, 0, :]
        all_tris = remap[np.concatenate([t for t in tris if len(t)])]
        flipped = all_tris if np.linalg.det(np.eye(3)[[0, 2, 1]][perm]) > 0 else all_tris[:, ::-1]
        poly['verts'] = v
        poly['indices'] = np.array(strips.stripify(flipped), np.uint16)
        mat = self.materials.describe(sk.materials[sec]['material']) if sec < len(sk.materials) else None
        diffuse = next((v for k, v in (mat or {}).get('textures', {}).items() if 'Diffuse' in k or 'BaseColor' in k),
                       None)
        tex = None
        if diffuse:
            img = self.textures.image(diffuse).astype(np.float32)
            img[..., 3] = 255.0
            tex = self.textures.bake(('item', diffuse), img, SHEET_SIDE)
            for material in poly['materials']:          # a solid thing: opaque
                material['src'], material['dst'] = MODEL_OPAQUE
                for t in material['textures']:
                    t['id'] = tex[1]
        c = (v['pos'].min(0) + v['pos'].max(0)) / 2
        r = float(np.linalg.norm(v['pos'] - c, axis=1).max())
        m['sphere'] = mesh['sphere'] = (float(c[0]), float(c[1]), float(c[2]), r)
        mesh['center'] = (float(c[0]), float(c[1]), float(c[2]))
        name = 'mfm_t7_%03d.nud' % len(self.models)
        self.models[name] = nud.write(m)
        self._mesh_names[key] = (name, tex)
        return self._mesh_names[key]

    # -------------------------------------------------------------------------------------------- one emitter
    def emitter(self, em, cap_frames=None):
        """-> [(ALO emitter, texture file name, model file name)] for a Tekken 7 emitter (none when it shows
        nothing); cap_frames: the frames after the effect starts by which it stops emitting (the end of the moves
        that spawn it, a projectile's life)."""
        req = em.module('Required')
        if req is None:
            return []
        td = em.type_data
        mesh = None
        mat_ref = req.get('Material')
        plane = None
        pre = None
        if td is not None and td.cls == 'TypeDataMesh' and td.get('Mesh'):
            pre = td.dist('RollPitchYawRange', 3, [0.0, 0.0, 0.0])
            mesh = short_ref(td.get('Mesh'))
            if ITEM_MESHES in mesh:          # the fighter's own item (Noctis's summoned weapons): its model draws it
                raise Invisible()
            sm = self._static_mesh(mesh)
            if sm is not None and not td.get('bOverrideMaterial') and sm.materials:
                mat_ref = sm.materials[0]
            plane = _plane(sm)
            if not _drawn_as_model(em, sm):  # a flat quad: drawn as a sprite of its size (models are few in TTT2)
                mesh = None
            else:
                plane = None
        mat = self.materials.describe(mat_ref) if mat_ref else None
        if mat is None or DISTORTION_MATERIAL.search(mat['base'] or ''):
            return []
        sheet = int(req.get('SubImages_Horizontal') or 1) * int(req.get('SubImages_Vertical') or 1) > 1
        src = material_source(self.textures, mat, sheet)
        if src is None and any(v and WHITE not in v for v in mat['textures'].values()):
            return []                      # only a distortion texture: a refraction effect
        additive = mat['blend'] == 'BLEND_Additive'
        duration_s = req.get('EmitterDuration', 1.0) or 0.0
        loops = req.get('EmitterLoops', 0) or 0      # Cascade's default: 0, endless
        delay_s = float(req.get('EmitterDelay') or 0.0)
        total_s = duration_s * loops if loops > 0 else max(duration_s, LOOP_CAP)
        if cap_frames:                     # it stops emitting at the cap (the move's end, the projectile's)
            total_s = min(total_s, max(0.0, cap_frames / FPS - delay_s))
        life = em.module('Lifetime')
        life_d = life.dist('Lifetime', 1, 1.0) if life else None
        if life_d is not None and life_d.peak() <= 0.0:
            life_d = None                  # lifetime 0: the particle lives as long as the system (a bullet's body)
            life_s = cap_frames / FPS if cap_frames else total_s
        else:
            life_s = life_d.mean()[0] if life_d else 1.0
        life_s = max(1.0 / FPS, life_s)
        life_f = life_s * FPS
        model = None
        if mesh is not None and self.models_allowed is not None and mesh not in self.models_allowed:
            sm = self._static_mesh(mesh)       # over the fighter's budget of models: a sprite of the mesh's size
            if sm is None:
                return []
            ext = sm.positions.max(0) - sm.positions.min(0)
            plane = (float(max(ext)), float(max(ext)))
            mesh = None
        if src is None:                    # white textures only: the material draws its shape itself
            src = self._plain(mesh is not None, mat)
        base = copy.deepcopy(self.model_tmpl if mesh is not None else self.sprite)
        alo.unpin(base)
        tex = self._colour(em, base, src, additive, life_f)
        shape = None
        if mesh is not None:
            shape = self._model_shape(em)
            model = self._mesh_nud(mesh, tex[1], MODEL_ADD if additive else MODEL_ALPHA, shape[0],
                                   tuple(pre.mean()) if pre is not None else (0.0, 0.0, 0.0))
            if model is None:
                return []
        sp, pa = base['spawner'], base['particle']
        sp['modifiers'] = []
        _set(sp, '>B', 251, 0)
        # flags, blend, type
        sflags = (SFLAG_MODEL if model else SFLAG_SPRITE) | (SFLAG_LOCAL if req.get('bUseLocalSpace') else 0)
        _set(sp, '>I', 136, sflags)
        _set(sp, '>I', 236, (MODEL_BLEND_ADD if additive else MODEL_BLEND_ALPHA) if model else
             (BLEND_ADD if additive else BLEND_ALPHA))
        _set(pa, '>I', alo.START_FLAGS, PFLAG_DIRECTED)
        _set(pa, '>B', alo.SHAPE, 0)
        pa.pop('shape', None)
        base['name'] = model
        # life
        _set(sp, '>HH', alo.LIFE, _u16(life_f), _u16(life_d.spread()[0] * FPS if life_d else 0))
        self._size(em, sp, model is not None, life_f, plane, shape)
        self._rotation(em, sp, model is not None, pre if model is not None else None)
        frame = emitter_frame(req)
        self._motion(em, sp, pa, life_f, frame[0])
        self._location(em, pa, frame)
        self._texture(sp, tex, _particle_scroll(em, mat, life_s) or src.scroll, req, life_f)
        # timing: one emitter for the rate, one per burst
        out = []
        spawn = em.module('Spawn')
        rate = 0.0
        if spawn is not None:
            rd = spawn.dist('Rate', 1, 0.0)
            scale = spawn.dist('RateScale', 1, 1.0)
            rate = max(0.0, (sum(lo[0] + hi[0] for _, lo, hi in rd.samples) / (2 * len(rd.samples)))) * scale.mean()[0]
        tname = tex[0]
        if rate > 0 and total_s > 0:
            e = copy.deepcopy(base)
            per = rate / FPS
            interval = max(1, int(round(1 / per))) if per < 1 else 1
            count = max(1, min(MAX_PER_FRAME, int(round(per)))) if per >= 1 else 1
            _set(e, '>HHHHHH', alo.INTERVAL, interval, 0, _u16(total_s * FPS), 0, _u16(delay_s * FPS), 0)
            alo._set_keys(e['particle']['tracks'][alo.COUNT_TRACK], [[0, count]])
            out.append((e, tname, model))
        for b in (spawn.get('BurstList') or []) if spawn is not None else []:
            n = int(b.get('Count') or 0)
            low = b.get('CountLow', -1)
            if low is not None and low >= 0:
                n = int(round((n + low) / 2))
            if n <= 0:
                continue
            t = float(b.get('Time') or 0.0) * duration_s
            if cap_frames and (delay_s + t) * FPS > cap_frames:
                continue
            e = copy.deepcopy(base)
            _set(e, '>HHHHHH', alo.INTERVAL, 0xFFFF, 0, 1, 0, _u16((delay_s + t) * FPS), 0)
            alo._set_keys(e['particle']['tracks'][alo.COUNT_TRACK], [[0, min(n, 64)]])
            out.append((e, tname, model))
        return out

    # -------------------------------------------------------------------------------------------- pieces
    @staticmethod
    def _model_shape(em):
        """A mesh particle's size: Cascade scales each axis (StartSize x SizeMultiplyLife), a TTT2 model particle
        only all of them alike. -> (proportions (X, Y, Z: Tekken 7 axes) baked into its model, the scale, its random
        half range, [(life fraction, scale factor)]): the proportions of each axis's mean over the life, the factor
        the mean of the axes' factors (exact when they grow alike; a streak growing along one axis gets a bit of
        that growth across too)."""
        m = em.module('Size')
        d = m.dist('StartSize', 3, [1.0, 1.0, 1.0]) if m else None
        start = [abs(x) for x in (d.mean() if d else [1.0, 1.0, 1.0])]
        spread = d.spread() if d else [0.0, 0.0, 0.0]
        mult = em.module('SizeMultiplyLife')
        ld = mult.dist('LifeMultiplier', 3, [1.0, 1.0, 1.0]) if mult else None
        times = [i / (CURVE_KEYS - 1) for i in range(CURVE_KEYS)]
        axes = [[start[a] * (abs(ld.mean(t)[a]) if ld else 1.0) for t in times] for a in range(3)]
        means = [max(1e-3, sum(v) / len(v)) for v in axes]
        scale = math.exp(sum(math.log(x) for x in means) / 3)
        proportions = tuple(float('%.2g' % (x / scale)) for x in means)     # (rounded: fewer models)
        factor = [(t, sum(axes[a][k] / means[a] for a in range(3)) / 3) for k, t in enumerate(times)]
        return proportions, scale, sum(spread) / 3.0 * scale / max(1e-3, sum(start) / 3.0), factor

    def _size(self, em, sp, is_model, life_f, plane=None, shape=None):
        if shape is not None:              # a model: one scale (_model_shape)
            _, scale, spread, factor = shape
            _set(sp, '>ffff', alo.SIZE, scale, scale, spread, spread)
            alo._set_keys(sp['tracks'][alo.SIZE_TRACK], _keys([(t * life_f, f) for t, f in factor]))
            return
        m = em.module('Size')
        d = m.dist('StartSize', 3, [1.0, 1.0, 1.0]) if m else None
        mean = d.mean() if d else [1.0, 1.0, 1.0]
        spread = d.spread() if d else [0.0, 0.0, 0.0]
        if plane is not None:              # a quad mesh scaled by the size: its extents (cm) times the scale
            mean = [mean[0] * plane[0], mean[1] * plane[1], mean[2]]
            spread = [spread[0] * plane[0], spread[1] * plane[1], spread[2]]
        if is_model:                       # a scale: one for all axes (the model's own size is in mm)
            s = sum(mean) / 3.0
            r = sum(spread) / 3.0
            _set(sp, '>ffff', alo.SIZE, s, s, r, r)
        else:                              # full width and height in mm
            _set(sp, '>ffff', alo.SIZE, mean[0] * CM, mean[1] * CM if mean[1] else mean[0] * CM, spread[0] * CM,
                 spread[1] * CM)
        mult = em.module('SizeMultiplyLife')
        keys = [[0, 1.0]]
        if mult is not None:
            ld = mult.dist('LifeMultiplier', 3, [1.0, 1.0, 1.0])
            axes = [0, 1, 2] if is_model else [0, 1]
            keys = _keys([(t * life_f, sum(lo[a] for a in axes) / len(axes)) for t, lo, hi in ld.samples])
        alo._set_keys(sp['tracks'][alo.SIZE_TRACK], keys)

    def _colour(self, em, e, src, additive, life_f):
        """The particles' colour and opacity over their life: Tekken 7's are often far above 1 (bright, thick
        effects); the brightest of them is baked into the emitter's texture (Textures.lit), the tracks keep each
        moment's share of it. -> the lit texture."""
        sp = e['spawner']
        c = em.module('Color')
        col = c.dist('StartColor', 3, [1.0, 1.0, 1.0]) if c else Distribution1(3)
        alpha = c.dist('StartAlpha', 1, 1.0) if c else Distribution1(1)
        base_rgb, rgb_spread = col.mean(), col.spread()
        base_a = alpha.mean()[0]
        col_curve = alpha_curve = None
        ol = em.module('ColorOverLife')
        if ol is not None:
            col_curve = ol.dist('ColorOverLife', 3)
            alpha_curve = ol.dist('AlphaOverLife', 1)
        sc = em.module('ColorScaleOverLife')
        scale_curve = sc.dist('ColorScaleOverLife', 3) if sc else None
        ascale_curve = sc.dist('AlphaScaleOverLife', 1) if sc else None
        power, add_colour = src.power
        times = sorted({0.0, 1.0} | {t for d in (col_curve, alpha_curve, scale_curve, ascale_curve) if d
                                     for t, _, _ in d.samples})
        cols, alphas = [], []
        for t in times:
            rgb = list(col_curve.mean(t)) if col_curve else list(base_rgb)
            if scale_curve:
                rgb = [a * b for a, b in zip(rgb, scale_curve.mean(t))]
            if src.subtractive:
                rgb = [max(0.0, 1.0 - x) for x in rgb]
            if add_colour:
                rgb = [a * b for a, b in zip(rgb, add_colour)]
            rgb = [max(0.0, x * power) for x in rgb]
            a = alpha_curve.mean(t)[0] if alpha_curve else base_a
            if ascale_curve:
                a *= ascale_curve.mean(t)[0]
            cols.append(rgb)
            alphas.append(max(0.0, a))
        if additive and max(max(rgb) for rgb in cols) <= 1e-3 or max(alphas) <= 1e-3:
            raise Invisible()
        peak = [max(rgb[k] for rgb in cols) for k in range(3)]
        peak_a = max(alphas)
        share = lambda x, p: min(1.0, max(0.0, x / p)) if p > 1e-6 else 0.0
        alo.set_emitter_colour(e, [1.0, 1.0, 1.0])
        alo._set_keys(sp['tracks'][alo.COLOUR_TRACK],
                      _keys([(t * life_f, *[share(x, p) for x, p in zip(rgb, peak)]) for t, rgb in zip(times, cols)]))
        alo._set_keys(sp['tracks'][alo.ALPHA_TRACK],
                      _keys([(t * life_f, share(a, peak_a)) for t, a in zip(times, alphas)]))
        _set(sp, '>fff', 12, *[share(s * power, max(peak)) for s in rgb_spread])
        _set(sp, '>f', 36, share(alpha.spread()[0], peak_a))
        return self.textures.lit(src, peak, peak_a, additive)

    def _rotation(self, em, sp, is_model, pre=None):
        """Start rotation and spin. pre: a mesh particle's TypeDataMesh RollPitchYawRange (degrees), its mean baked
        into the model (_mesh_nud): its random half range adds to the start rotation's, and under a baked yaw of 180
        degrees the mesh rotation's pitch and roll turn the other way (the engine adds both rotators up, then rolls,
        pitches and yaws; the model is yawed first)."""
        rot = [0.0, 0.0, 0.0]
        rot_r = [0.0, 0.0, 0.0]
        spin = [0.0, 0.0, 0.0]
        spin_r = [0.0, 0.0, 0.0]
        r = em.module('Rotation')
        if r is not None:
            d = r.dist('StartRotation', 1, 0.0)
            rot[2], rot_r[2] = d.mean()[0] * TURN, d.spread()[0] * TURN
        rr = em.module('RotationRate')
        if rr is not None:
            d = rr.dist('StartRotationRate', 1, 0.0)
            spin[2], spin_r[2] = d.mean()[0] * TURN / FPS, d.spread()[0] * TURN / FPS
        if is_model:
            mr = em.module('MeshRotation')
            if mr is not None:
                d = mr.dist('StartRotation', 3, [0.0, 0.0, 0.0])
                rot = turns_to_ttt2(d.mean())
                rot_r = [abs(x) for x in turns_to_ttt2(d.spread())]
            mrr = em.module('MeshRotationRate')
            if mrr is not None:
                d = mrr.dist('StartRotationRate', 3, [0.0, 0.0, 0.0])
                spin = [x / FPS for x in turns_to_ttt2(d.mean())]
                spin_r = [abs(x) / FPS for x in turns_to_ttt2(d.spread())]
            if pre is not None:
                roll, pitch, yaw = pre.mean()
                if abs(roll) < 1e-3 and abs(pitch) < 1e-3 and abs(abs(yaw) - 180.0) < 1e-3:
                    rot[0], rot[2], spin[0], spin[2] = -rot[0], -rot[2], -spin[0], -spin[2]
                rot_r = [a + abs(b) for a, b in zip(rot_r, turns_to_ttt2([x / 360.0 for x in pre.spread()]))]
        _set(sp, '>12f', 164, *(rot + rot_r + spin + spin_r))
        _set(sp, '>6f', 212, 0, 0, 0, 0, 0, 0)

    def _motion(self, em, sp, pa, life_f, rot=None):
        """Velocity in the emitter frame, keyed over the particle's age: start velocity x velocity over life,
        drag, constant acceleration (world z up -> y), and the path of LocationDirect (a position keyed over the
        life: its rate of change). rot: the emitter's rotation in its system (emitter_frame), which turns the start
        velocities and the path."""
        v0 = [0.0, 0.0, 0.0]
        vr = [0.0, 0.0, 0.0]
        for m in em.all('Velocity') + em.all('Velocity_Seeded'):
            d = m.dist('StartVelocity', 3, [0.0, 0.0, 0.0])
            v0 = [a + b for a, b in zip(v0, to_ttt2(in_frame(d.mean(), rot)))]
            vr = [a + abs(b) for a, b in zip(vr, to_ttt2(in_frame(d.spread(), rot)))]
        cone = em.module('VelocityCone')
        if cone is not None:                    # a cone around the emitter's axis (Cascade: X? here: forward)
            vel = cone.dist('Velocity', 1, 0.0).mean()[0]
            v0[2] += vel
        vol = em.module('VelocityOverLifetime')
        vol_d = vol.dist('VelOverLife', 3) if vol else None
        drag = 0.0
        for m in em.all('AccelerationDrag'):
            drag += m.dist('DragCoefficientRaw', 1, 0.0).mean()[0]
        dsc = em.module('AccelerationDragScaleOverLife')
        if dsc is not None:
            drag *= dsc.dist('DragScaleRaw', 1, 1.0).mean()[0]
        acc = [0.0, 0.0, 0.0]
        for m in em.all('Acceleration') + em.all('AccelerationConstant'):
            d = m.dist('Acceleration', 3, [0.0, 0.0, 0.0])
            acc = [a + b for a, b in zip(acc, to_ttt2(d.mean()))]
        direct = em.module('LocationDirect')
        path = direct.dist('Location', 3) if direct is not None else None
        life_s = max(1.0 / FPS, life_f / FPS)
        steps = CURVE_KEYS
        keys = [[], [], []]
        for i in range(steps):
            t = i / (steps - 1)
            age_s = t * life_f / FPS
            mult = vol_d.mean(t) if vol_d else [1.0, 1.0, 1.0]
            mult = [mult[1], mult[2], mult[0]] if vol_d else mult
            damp = math.exp(-drag * age_s)
            along = [0.0, 0.0, 0.0]
            if path is not None and len(path.samples) > 1:      # cm per life fraction -> cm per second
                h = 0.5 / (steps - 1)
                a = to_ttt2(in_frame(path.mean(max(0.0, t - h)), rot))
                b = to_ttt2(in_frame(path.mean(min(1.0, t + h)), rot))
                along = [(y - x) / ((min(1.0, t + h) - max(0.0, t - h)) * life_s) for x, y in zip(a, b)]
            for k in range(3):
                v = (v0[k] * mult[k] * damp + acc[k] * age_s + along[k]) * CM / FPS
                keys[k].append((t * life_f, v))
        moving = any(abs(v) > 1e-4 for kk in keys for _, v in kk)
        for k, off in enumerate(alo.VELOCITY_TRACKS):
            alo._set_keys(sp['tracks'][off], _keys(keys[k]) if moving else [[0, 0.0]])
        _set(sp, '>fff', 120, *[x * CM / FPS for x in vr])
        for o in (4, 16, 28, 40):
            alo._set_keys(pa['tracks'][o], [[0, 0.0]])

    def _location(self, em, pa, frame=(None, (0.0, 0.0, 0.0))):
        """Spawn offset and its random half range: the location modules' offsets turned by the emitter's rotation,
        plus its origin (emitter_frame). (The character-bone module's offset is left as it is: unverified.)"""
        rot, origin = frame
        mean = [0.0 + x for x in to_ttt2(origin)]           # (0.0 + -0.0: no negative zeros from a zero origin)
        spread = [0.0, 0.0, 0.0]
        for cls, prop in (('Location', 'StartLocation'), ('Location_Seeded', 'StartLocation'),
                          ('PM_CharacterBone', 'LocationOffset'), ('LocationDirect', 'Location')):
            r = None if cls == 'PM_CharacterBone' else rot
            for m in em.all(cls):
                d = m.dist(prop, 3, [0.0, 0.0, 0.0])
                mean = [a + b for a, b in zip(mean, to_ttt2(in_frame(d.mean(), r)))]
                spread = [a + abs(b) for a, b in zip(spread, to_ttt2(in_frame(d.spread(), r)))]
        sph = em.module('LocationPrimitiveSphere')
        cyl = em.module('LocationPrimitiveCylinder') or em.module('LocationPrimitiveCylinder_Seeded')
        if sph is not None:                     # a sphere: as a box of its size around its centre
            r = sph.dist('StartRadius', 1, 0.0).mean()[0]
            c = to_ttt2(in_frame(sph.dist('StartLocation', 3, [0.0, 0.0, 0.0]).mean(), rot))
            mean = [a + b for a, b in zip(mean, c)]
            spread = [a + r * 0.7 for a in spread]
        if cyl is not None:
            r = cyl.dist('StartRadius', 1, 0.0).mean()[0]
            h = cyl.dist('StartHeight', 1, 0.0).mean()[0]
            c = to_ttt2(in_frame(cyl.dist('StartLocation', 3, [0.0, 0.0, 0.0]).mean(), rot))
            mean = [a + b for a, b in zip(mean, c)]
            axis = cyl.get('HeightAxis') or 'PMLPC_HEIGHTAXIS_Z'
            ext = [r, r, r]
            ext[{'PMLPC_HEIGHTAXIS_X': 2, 'PMLPC_HEIGHTAXIS_Y': 0}.get(axis, 1)] = h / 2
            spread = [a + b for a, b in zip(spread, ext)]
        alo._set_keys(pa['tracks'][64], [[0] + [x * CM for x in mean]])
        _set(pa, '>fff', 76, *[x * CM for x in spread])

    def _texture(self, sp, tex, scroll, req, life_f):
        """The texture slot of layer 0 (and a flipbook when the material is a sheet of sub-images); scroll: UV
        per second."""
        lay = sp['layers'][0]
        u = lay['uvs'][0]
        if tex is None:
            u['name'] = None
            return
        name, gidx, w, h = tex[:4]
        u['name'] = name
        _set(u, '>hh', 64, w, h)
        su, sv = scroll
        if su or sv:                       # the texture slot's track +8: UV scroll per frame
            alo._set_keys(u['tracks'][8], [[0, su / FPS, sv / FPS]])
        nh = int(req.get('SubImages_Horizontal') or 1)
        nv = int(req.get('SubImages_Vertical') or 1)
        if nh * nv > 1 and nh <= 16 and nv <= 16:
            cells = nh * nv
            per = max(1, int(round(life_f / cells)))
            frames = []
            for k in range(cells):
                raw = bytearray(alo.FRAME_SIZE)
                struct.pack_into('>HHff', raw, 0, per, 0, (k % nh) / nh, (k // nh) / nv)
                frames.append({'raw': bytes(raw), 'texture': name})
            head = bytearray(alo.ANIM_SIZE)
            struct.pack_into('>HHHH', head, 8, cells - 1, cells, 1, 0)
            lay['anim'] = {'raw': bytes(head), 'frames': frames, 'names': [{'raw': bytes(alo.NAME_SIZE), 'name': name}]}
            raw = bytearray(lay['raw'])
            raw[12] = 1
            lay['raw'] = bytes(raw)
            lay.pop('still', None)
            cu = 1.0 / nh
            cv = 1.0 / nv
            _set(u, '>8f', 32, 0.0, 0.0, 0.0, cv, cu, cv, cu, 0.0)

    # -------------------------------------------------------------------------------------------- meshes
    def _static_mesh(self, path):
        if path not in self._meshes:
            try:
                self._meshes[path] = staticmesh.StaticMesh(load(self.paks, path.split('.')[0]))
            except (KeyError, ValueError) as error:
                self.log('effects: mesh %s not read (%s)' % (path, error))
                self._meshes[path] = None
        return self._meshes[path]

    def _mesh_nud(self, path, tex_id, blend=MODEL_ADD, proportions=(1.0, 1.0, 1.0), pre=(0.0, 0.0, 0.0)):
        """The effect model of a Tekken 7 mesh (positions in mm in the emitter frame, each Tekken 7 axis stretched
        by `proportions`, then turned by `pre`: the emitter's TypeDataMesh RollPitchYawRange, (roll, pitch, yaw)
        degrees; Geese's wave meshes are modelled crest backward and yawed 180) -> its file name. One model a mesh,
        blend, proportions and turn: a model particle draws the texture of its emitter's texture slot (tex_id only
        fills the model's material) blended as the material says."""
        pre = tuple(float('%.4g' % x) for x in pre)
        key = (path, blend, tuple(proportions)) + ((pre,) if any(pre) else ())
        if key in self._mesh_names:
            return self._mesh_names[key]
        sm = self._static_mesh(path)
        if sm is None or not len(sm.indices):
            return None
        p = sm.positions * np.asarray(proportions, np.float32)
        n = sm.normals / np.asarray(proportions, np.float32)               # (normals of the stretched mesh)
        n = n / np.maximum(1e-6, np.linalg.norm(n, axis=1, keepdims=True))
        if any(pre):
            turn = frotation(pre[1], pre[2], pre[0]).astype(np.float32)
            p, n = p @ turn, n @ turn
        pos = np.stack([-p[:, 1], p[:, 2], p[:, 0]], 1) * CM
        nrm = np.stack([-n[:, 1], n[:, 2], n[:, 0]], 1)
        m = copy.deepcopy(self.nud_template)
        mesh = m['meshes'][0]
        poly = mesh['polys'][0]
        dt = poly['verts'].dtype
        v = np.zeros(len(pos), dt)
        v['pos'] = pos
        v['nrm'][:, :3] = nrm
        v['nrm'][:, 3] = 1.0
        if 'color' in dt.names:
            if sm.colors is not None:
                v['color'] = sm.colors[:, :4]
            else:
                v['color'] = VERTEX_WHITE
        v['uv'][:, 0, :] = sm.uvs[:, 0, :]
        tris = np.concatenate([t for _, t in sm.triangles()])[:, ::-1]      # mirrored axes: keep the winding
        strip = strips.stripify(tris)
        if len(strip) >= 0x10000 or len(pos) >= 0xFFFF:
            self.log('effects: mesh %s too big (%d vertices)' % (path, len(pos)))
            return None
        poly['verts'] = v
        poly['indices'] = np.array(strip, np.uint16)
        for mat in poly['materials']:
            mat['src'], mat['dst'] = blend
            for t in mat['textures']:
                t['id'] = tex_id
        c = (pos.min(0) + pos.max(0)) / 2
        r = float(np.linalg.norm(pos - c, axis=1).max())
        m['sphere'] = mesh['sphere'] = (float(c[0]), float(c[1]), float(c[2]), r)
        mesh['center'] = (float(c[0]), float(c[1]), float(c[2]))
        name = 'mfm_t7_%03d.nud' % len(self.models)
        self.models[name] = nud.write(m)
        self._mesh_names[key] = name
        return name


EFFECT_NAME = 'eff_t7_%d'            # the converted effect of a Tekken 7 id, in the pack's alo/
HIT_EFFECT = 0                     # id 0: the fighter's own hit effect (TTT2 spawns it on hits; the donor's otherwise)


def convert_fighter(paks, archives, code, ids, first_texture_id, projectiles=None, log=None, copies=None):
    """The effects a fighter's moves spawn (Tekken 7 ids `ids`) and the looks of its projectiles ({effect id:
    moveset.Converter.projectile_look's spec}) -> (pack files {'alo/...': bytes}, {id: [effect names]}: one or two
    files an id, the item database spawns them all); ids the fighter's effect item does not name, or whose systems
    show nothing, are left out. copies: {id: (Tekken 7 id, frames it may emit for)} (moveset.Converter.
    split_effects): an effect stopped with the move that spawns it."""
    log = log or (lambda s: None)
    table = effect_table(paks, code)
    conv = Converter(paks, archives, first_texture_id, log=log)
    missiles = missile_systems(paks, code) if projectiles else {}
    used = [table[i] for i in ids if i in table] + [missiles[p['missile']] for p in (projectiles or {}).values()
                                                     if p.get('missile') in missiles]
    meshes = conv.model_budget(used, MODEL_BUDGET)
    if meshes > MODEL_BUDGET:
        log('effects: %d meshes; the %d used most are models, the others sprites' % (meshes, MODEL_BUDGET))
    files, names = {}, {}

    def add(i, made):
        for suffix, data in sorted(made.items()):
            name = EFFECT_NAME % i + suffix
            names.setdefault(i, []).append(name)
            files['alo/%s.alo' % name] = data

    for i in sorted(ids):
        source, cap = (copies or {}).get(i, (i, None))
        path = table.get(source)
        if path is None:
            continue
        try:
            add(i, conv.system(path, cap))
        except (KeyError, ValueError) as error:
            log('effects: id %d (%s) left out: %s' % (i, path.split('/')[-1], error))
    for i, spec in sorted((projectiles or {}).items()):
        if 'item' in spec:                 # a thrown weapon (the definition's THROWN)
            try:
                add(i, conv.thrown(spec, table.get(spec.get('trail'))))
            except (KeyError, ValueError) as error:
                log('effects: thrown %s left out: %s' % (spec['item']['bone'], error))
            continue
        path = missiles.get(spec['missile'])
        if path is None:
            continue
        try:
            add(i, conv.projectile(path, spec))
        except (KeyError, ValueError) as error:
            log('effects: projectile %d (%s) left out: %s' % (i, path.split('/')[-1], error))
    for name, data in conv.textures.files.items():
        files['alo/texture/' + name] = data
    for name, data in conv.models.items():
        files['alo/model/' + name] = data
    log('effects: %d of the %d ids its moves and projectiles spawn (%d files, %d emitters, %d left out), %d textures '
        '(%d KB), %d meshes (%d KB)' % (len(names), len(ids) + len(projectiles or {}),
                                        sum(map(len, names.values())), conv.stats['emitters'], conv.stats['dropped'],
                                        len(conv.textures.files), sum(map(len, conv.textures.files.values())) // 1024,
                                        len(conv.models), sum(map(len, conv.models.values())) // 1024))
    return files, names


def emitting_frames(paks, code, ids):
    """{id: the frames after its start until which a fighter's Tekken 7 effect emits (rate emitters and later
    bursts), None when it only bursts at its start} (moveset.Converter.split_effects)."""
    table = effect_table(paks, code)
    out = {}
    for i in ids:
        if i not in table:
            continue
        try:
            ps = ParticleSystem(paks, table[i])
        except (KeyError, ValueError):
            continue
        end = 0.0
        for em in ps.emitters:
            req, spawn = em.module('Required'), em.module('Spawn')
            if req is None or spawn is None:
                continue
            duration = req.get('EmitterDuration', 1.0) or 0.0
            loops = req.get('EmitterLoops', 0) or 0
            delay = float(req.get('EmitterDelay') or 0.0)
            rd = spawn.dist('Rate', 1, 0.0)
            if max(hi[0] for _, lo, hi in rd.samples) > 0:
                end = max(end, delay + (duration * loops if loops > 0 else max(duration, LOOP_CAP)))
            for b in spawn.get('BurstList') or []:
                end = max(end, delay + float(b.get('Time') or 0.0) * duration)
        out[i] = int(math.ceil(end * FPS)) if end * FPS > 2 else None
    return out


def _on_floor(em):
    """Whether Tekken 7 places an emitter's particles from the floor's height (its FloorHeight module, or the
    character-bone module's FloorHeight) rather than from the body part's."""
    if em.module('FloorHeight') is not None:
        return True
    cb = em.module('PM_CharacterBone')
    return bool(cb is not None and cb.get('FloorHeight'))


def _drawn_as_model(em, sm):
    """Whether a mesh emitter's mesh is drawn as a model: a solid shape, or a quad lying on the floor (a glow under
    a projectile: a camera-facing sprite of it would stand up as a disc; a quad that turns to face the camera or
    its velocity is no floor); other quads become sprites."""
    if sm is None:
        return False
    if _plane(sm) is None:
        return True
    ext = sm.positions.max(0) - sm.positions.min(0)
    facing = em.type_data.get('CameraFacingOption') if em.type_data is not None else None
    return _on_floor(em) and int(np.argmin(ext)) == 2 and not facing


def _plane(sm):
    """(width, height) in cm of a flat quad mesh (the effects' planes: 4 to 12 vertices in one plane), else
    None."""
    if sm is None or len(sm.positions) > 12:
        return None
    ext = sm.positions.max(0) - sm.positions.min(0)
    flat = int(np.argmin(ext))
    if ext[flat] > 0.05 * max(ext.max(), 1e-6):
        return None
    w, h = [float(ext[a]) for a in range(3) if a != flat]
    return (w or 1.0, h or 1.0)


class Invisible(Exception):
    """An emitter whose particles add nothing to the picture (black and additive, or fully transparent)."""


class Distribution1:
    """A distribution of ones (a module that is missing)."""

    def __init__(self, dim):
        self.dim = dim

    def mean(self, t=0.0):
        return [1.0] * self.dim

    def spread(self, t=0.0):
        return [0.0] * self.dim
