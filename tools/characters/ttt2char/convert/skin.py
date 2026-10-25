"""Tekken 7 skin -> TTT2 skin: a colour gain measured on the fighters both games have.

TTT2 draws a skin texture's colours much as they are; Tekken 7's skin shader (subsurface scattering, its lighting and
tone mapping) brightens and desaturates its own skin textures, which are darker and more orange: baked as they are,
an added fighter looks tanned beside TTT2's fighters on any stage. So the skin materials' baked colours are scaled
per channel by the gain that turns Tekken 7's baked faces into TTT2's face textures, measured on the fighters both
games have under the same code, of the fighter's sex (the games' female faces differ more than their male ones):
the median of their ratios (linear colour means) over the NEAREST of them in brightness (TTT2 evens skin out: a
dark Tekken 7 face gains more than a fair one, Marduk x3.3 and Lars x1.4 in red).

Written for this project from the files themselves (no third-party code).
"""
import numpy as np

from ..ttt2 import nud, nut
from ..ttt2.archive import key_of

# fighters with a face of their own in both games (Tekken 7: Character/Common/shader/.../skin/<code>/)
SHARED = {'female': ['ann', 'ask', 'jul', 'leo', 'lil', 'nin', 'xia', 'zaf'],
          'male': ['bob', 'bry', 'dra', 'edd', 'fen', 'gan', 'hei', 'hwo', 'jin', 'kaz', 'lar', 'law', 'lee', 'lei',
                   'mar', 'mig', 'pau', 'ste']}
FACE_MATERIAL = '/Game/Character/Common/shader/MaterialInstance/skin/%s/MI_CH_%s_skin_face'
FACE_MESH = 'cha:kao_O_OBJ'         # TTT2's face mesh; its polygon with the most vertices is the skin
FEWEST = 3                          # pairs needed for a gain (else none: 1, 1, 1)
NEAREST = 5                         # the fighters whose faces are nearest in brightness give the gain
HUE_SPAN = 1.25                     # a fighter's skin keeps its own hue within this factor of theirs (green/red,
                                    # blue/red): Tekken 7's darker skins are far more orange than TTT2 draws any
LUMA = np.array([0.2126, 0.7152, 0.0722])


def _srgb_to_linear(x):
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def ttt2_face(archives, code):
    """The linear mean colour of TTT2 fighter `code`'s face texture, or None."""
    if not archives.has(key_of(code + '.nud')) or not archives.has(key_of(code + '.nut')):
        return None
    model = nud.read(archives.read_name(code + '.nud'))
    polys = [p for m in model['meshes'] if m['name'] == FACE_MESH for p in m['polys']]
    if not polys:
        return None
    gidx = max(polys, key=lambda p: len(p['verts']))['materials'][0]['textures'][0]['id']
    found = [t for t in nut.read(archives.read_name(code + '.nut'))['textures'] if t['gidx'] == gidx]
    if not found:
        return None
    rgb = nut.decode(found[0])[..., :3].astype(np.float64) / 255.0
    return _srgb_to_linear(rgb).reshape(-1, 3).mean(0)


def gain(paks, archives, sex, materials, bake, own=None, log=None):
    """The per-channel gain for the skin of a fighter of `sex` whose Tekken 7 face's mean linear colour is `own`
    (None: the median over all of them). materials: tekken7.material.Materials; bake: model.bake_albedo with its
    textures, (effective material) -> (linear rgb, alpha). -> (gain, their face colour: the hue hue() keeps skin
    near)."""
    ratios, lumas, faces = [], [], []
    for code in SHARED.get(sex, SHARED['female']):
        path = FACE_MATERIAL % (code, code)
        if not paks.find(path):
            continue
        try:
            rgb, _ = bake(materials.effective(path, {}))
            theirs = ttt2_face(archives, code)
        except (KeyError, ValueError):
            continue
        if rgb is None or theirs is None:
            continue
        ours = rgb.reshape(-1, 3).mean(0)
        if (ours > 1e-3).all():
            ratios.append(theirs / ours)
            lumas.append(float(ours @ LUMA))
            faces.append(theirs)
    if len(ratios) < FEWEST:
        if log:
            log('  skin: too few fighters in both games to measure (%d); colours as Tekken 7 has them' % len(ratios))
        return np.ones(3), None
    ratios, faces = np.array(ratios), np.array(faces)
    if own is not None and float(own @ LUMA) > 0:
        near = np.argsort(np.abs(np.log(np.array(lumas)) - np.log(float(own @ LUMA))))[:NEAREST]
        ratios, faces = ratios[near], faces[near]
    g = np.median(ratios, 0)
    target = np.median(faces, 0)
    if log:
        log('  skin: colour gain %.2f %.2f %.2f (from the %d fighters both games have %s)' % (
            g[0], g[1], g[2], len(ratios), 'nearest in brightness' if own is not None else 'of that sex'))
    return g, target


def hue(rgb, target):
    """A skin texture (linear rgb, after the gain) with its mean's green/red and blue/red within HUE_SPAN of the
    target's (TTT2's faces): per material, as a fighter's face and its generic body textures differ."""
    if target is None:
        return rgb
    m = rgb.reshape(-1, 3).mean(0)
    if m[0] <= 1e-4:
        return rgb
    f = np.ones(3)
    for c in (1, 2):
        want = m[0] * np.clip(m[c] / m[0], target[c] / target[0] / HUE_SPAN, target[c] / target[0] * HUE_SPAN)
        f[c] = want / max(m[c], 1e-6)
    return np.clip(rgb * f.astype(rgb.dtype), 0, 1)
