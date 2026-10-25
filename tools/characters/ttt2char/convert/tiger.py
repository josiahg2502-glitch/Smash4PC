"""A Tekken 7 fighter's summoned model (Kazumi's tiger) -> extra bones of the added fighter, shown by its moves.

Tekken 7 draws the tiger as an actor of its own (CharacterAlly) with animations of its own, started by the fighter's
moves (move property 0x828f = the tiger animation's index). TTT2 animates a fighter's extra (OPT) bones from part 3
of its motion pack (Devil Jin's wings), and the runtime does that for the added fighter (title/roster.cpp, extra
bones). So:
* the tiger's skeleton joins the fighter's as bones `cha:OPT600_<name>`.. (the root's parent: HARA_ROT1, the only
  root), listed in the skeleton's OPT list; its meshes are skinned to them (model.py);
* the bones and the vertices wait HIDE_Y below the floor: the bind pose shows nothing;
* each tiger animation becomes a raw part-3 animation: per bone three channels (translation, rotation as radians
  with R = Rz Ry Rx, scale 1), the transform relative to its parent (the root's relative to HARA_ROT1's bind pose),
  and one more frame with the bind pose that puts the tiger back below the floor;
* the moves' 0x828f become 0x82de = 0x4000 + the animation's index in part 3 (moveset.py, opt_anims);
* the spring file (<code>.mop) keeps the donor's entries and gets one per tiger bone, a copy of Devil Jin's first
  wing bone's (flag 8: no spring; a bone in the OPT list without an entry swings under gravity).

Written for this project from the files themselves (no third-party code).
"""
import struct

import numpy as np

from ..tekken7 import anim, mesh as t7mesh
from ..tekken7.uasset import load
from ..ttt2 import bone
from . import model as M, skeleton

OPT_FIRST = 600                     # the first extra bone's number (cha:OPT600_..)
HIDE_Y = -100000.0                  # mm: where the tiger waits, below the floor
ROOT_PARENT = 0                     # HARA_ROT1
NAME_LENGTH = 31                    # a .bon name's longest

# The spring file (big-endian): f32 5.0 and 28 zero bytes, u32 count and 12 zero bytes, count x 0x60-byte entries
# (u32 OPT number first), then u32 count and 12 bytes of another list (links; none here).
MOP_ENTRIES, MOP_ENTRY = 0x30, 0x60
SPRING_TEMPLATE = ('dvj', 600)      # the entry the tiger's bones copy: Devil Jin's first wing bone (flag 8)


def append_bones(skel, tiger_bones):
    """The fighter's skeleton (a .bon dict) with the tiger's bones after its own, hidden; -> (skeleton, first index).
    tiger_bones: {'names', 'parents', 'world' (n, 4, 4) TTT2 bind matrices, mm} (model.load_tekken7_model)."""
    first = len(skel['names'])
    hw = bone.world_matrices(skel)
    hide = np.eye(4)
    hide[1, 3] = HIDE_Y
    world = np.array([hide @ w for w in tiger_bones['world']])
    s = dict(skel)
    for key in ('names', 'parent', 'opt'):
        s[key] = list(skel[key])
    s['names_raw'] = None                       # bone.write makes them from the names
    ts, rs, ss = list(skel['t']), list(skel['r']), list(skel['s'])
    for i, name in enumerate(tiger_bones['names']):
        p = tiger_bones['parents'][i]
        local = np.linalg.inv(hw[ROOT_PARENT] if p < 0 else world[p]) @ world[i]
        rot = local[:3, :3] / np.linalg.norm(local[:3, :3], axis=0)
        full = ('cha:OPT%d_%s' % (OPT_FIRST + i, name))[:NAME_LENGTH]
        s['names'].append(full)
        s['parent'].append(ROOT_PARENT if p < 0 else first + p)
        s['opt'].append(OPT_FIRST + i)
        ts.append(local[:3, 3])
        rs.append(bone.euler_xyz_from_matrix(rot))
        ss.append(np.ones(3))
    s['t'], s['r'], s['s'] = np.array(ts, np.float32), np.array(rs, np.float32), np.array(ss, np.float32)
    return s, first


def hidden(points):
    """Vertex positions (mm) where the hidden bind pose puts them."""
    return points + np.array([0.0, HIDE_Y, 0.0])


def world_frames(a, skeleton):
    """(frames, bones, 4, 4) TTT2-space world matrices (mm, not hidden) of a decoded animation on the Tekken 7
    skeleton ({'names', 'parents', 'pose'})."""
    ref = np.array(skeleton['pose'], np.float64)
    out = []
    for f in range(a['frames']):
        pose = ref.copy()
        for b, (pos, q) in a['tracks'].items():
            pose[b, 0:4] = q[f]
            pose[b, 4:7] = pos[f]
        w = M._ue_to_ttt2_matrix(t7mesh.global_matrices(pose, skeleton['parents'])).astype(np.float64)
        w[:, :3, 3] *= M.M_TO_MM
        out.append(w)
    return np.array(out)


def channels(worlds, skel, first, order):
    """Part-3 channels of the tiger bones (skel indices first.., Tekken 7 bones `order`) from world matrices, then
    the bind frame. -> [(descriptor, (frames, 3) values)]: per bone translation (11), rotation (7), scale (3)."""
    frames = len(worlds) + 1
    hara = bone.world_matrices(skel)[ROOT_PARENT]
    out = []
    for k, t7 in enumerate(order):
        i = first + k
        p = skel['parent'][i]
        t, r = np.zeros((frames, 3)), np.zeros((frames, 3))
        for f in range(frames - 1):
            parent = hara if p == ROOT_PARENT else worlds[f][order[p - first]]
            local = np.linalg.inv(parent) @ worlds[f][t7]
            t[f] = local[:3, 3]
            r[f] = bone.euler_xyz_from_matrix(local[:3, :3] / np.linalg.norm(local[:3, :3], axis=0))
        t[-1], r[-1] = skel['t'][i], skel['r'][i]
        out += [(11, t), (7, r), (3, np.ones((frames, 3)))]
    return out


def raw_animation(chans):
    """[(descriptor, (frames, 3))] -> a raw (0xC8) big-endian animation."""
    frames = len(chans[0][1])
    out = struct.pack('>HHI', 0xC8, len(chans), frames)
    out += struct.pack('>%dI' % len(chans), *[d for d, _ in chans])
    return out + np.stack([v for _, v in chans], 1).astype('>f4').tobytes()


def animations(paks, d, skel, log=print):
    """The tiger's part-3 animations. -> ([raw animation], {Tekken 7 tiger animation index: index in part 3})."""
    sk = t7mesh.SkeletalMesh(load(paks, d.TIGER['mesh'])).skeleton
    first = next(i for i, n in enumerate(skel['names']) if n.startswith('cha:OPT%d_' % OPT_FIRST))
    order = []                                  # the Tekken 7 bone of each tiger bone (names may be cut short)
    for full in skel['names'][first:]:
        name = full.split('_', 1)[1]
        match = [t for t, n in enumerate(sk['names']) if skeleton.canonical(n) == name]   # (as model.py spells it)
        if not match and len(full) == NAME_LENGTH:
            match = [t for t, n in enumerate(sk['names']) if skeleton.canonical(n).startswith(name)]
        if len(match) != 1:
            raise ValueError('tiger bone %s is not one bone of the Tekken 7 skeleton' % full)
        order.append(match[0])
    anims, index = [], {}
    for t7, path in sorted(d.TIGER['animations'].items()):
        a = anim.decode(load(paks, path))
        anims.append(raw_animation(channels(world_frames(a, sk), skel, first, order)))
        index[t7] = len(anims) - 1
        log('  tiger animation %d: %s, %d frames' % (t7, path.rsplit('/', 1)[-1], a['frames']))
    return anims, index


def spring_file(donor_mop, template_mop, count):
    """The added fighter's spring file: the donor's entries, then one per extra bone (OPT 600..) copied from the
    template's entry for SPRING_TEMPLATE's number."""
    n, = struct.unpack_from('>I', donor_mop, 0x20)
    entries = [donor_mop[MOP_ENTRIES + MOP_ENTRY * k:MOP_ENTRIES + MOP_ENTRY * (k + 1)] for k in range(n)]
    tn, = struct.unpack_from('>I', template_mop, 0x20)
    template = next((template_mop[MOP_ENTRIES + MOP_ENTRY * k:MOP_ENTRIES + MOP_ENTRY * (k + 1)] for k in range(tn)
                     if struct.unpack_from('>I', template_mop, MOP_ENTRIES + MOP_ENTRY * k)[0] == SPRING_TEMPLATE[1]),
                    None)
    if template is None:
        raise ValueError('the template spring file has no entry %d' % SPRING_TEMPLATE[1])
    for k in range(count):
        e = bytearray(template)
        struct.pack_into('>I', e, 0, OPT_FIRST + k)
        entries.append(bytes(e))
    return donor_mop[:0x20] + struct.pack('>I', len(entries)) + bytes(12) + b''.join(entries) + bytes(16)
