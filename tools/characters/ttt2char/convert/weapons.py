"""Weapons Tekken 7 fighters carry (Negan's bat, Kunimitsu II's blades, Leroy's cane, Master Raven's katana): extra
bones of the fighter's, posed by its body animations, with hit locations of their own.

Tekken 7 draws such a weapon as part of the costume: a mesh skinned to an ATH_* bone under a hand (the hips, the
back), and the fighter's moves pose that bone with the extra channels of their body animations: after the 23 body
channels, a pair (translation, rotation) per weapon bone, relative to its parent bone; animations without the pair
leave the bone where Tekken 7's bind pose has it. The pairs are TTT2's own: TTT2 poses Yoshimitsu's katanas (extra
bones 1200, 1210 under his hands) from such pairs and writes them into the bones' records unchanged, and Tekken 7's
item moves carry the very same animations as TTT2's (Co_IHomeRun). So the weapon bone's local transform is the pair
(the rotation R = Rz Ry Rx of its angles), in TTT2's frame of the parent. Checked on Tekken 7's own fighters: the
pairs' resting values are the bind poses' (Kunimitsu II's right sword, hip sword: within 2 degrees and 1 mm; Negan's
right bat: its rotation), Kunimitsu II's kunai flies straight at the opponent, Negan's bat strikes reach in front.
The bone's frame is Tekken 7's bone's with y reversed (M below; TTT2's frames are Tekken 7's so, the hands' bind poses
show). Which pair poses which bone: Tekken 7's order (Negan: 0 the left bat, 1 the right; Kunimitsu II: 0..2 her
left hand's, 3 the right sword, 4 the hip sword), found by the resting values.

TTT2 samples those channels for every fighter but uses them only for some fighters' extra bones; the runtime poses
the added fighter's extra bone OPT 800 + k with pair k (title/roster.cpp, weapons). So:
* the weapon's bone joins the skeleton as `cha:OPT8xx_<name>` under its TTT2 parent (OPT 800 + pair; a weapon no
  animation poses, 820 on), bound as above; the weapon's triangles are skinned to it (a weapon mesh often holds more
  copies than one: Negan's has a bat for each hand and a knife);
* the points the weapon hits with are extra bones under it (`cha:OPT85x_hit<code>`: where Tekken 7's point bones are
  bound, `_top`, `_hilp`, or cm along its length). TTT2's hit test (sub_03C74F50) takes a bone's position only for
  hit locations 0..21; 27..77 are points it keeps per player (player +3296), 70..77 those of a held item (+3416 +
  12 k), which only items write (74..77 while one is attached; nothing writes 70..73 for the added fighter). So point j
  hits as location 70 + j, and the runtime writes the point's position there every frame; above 77 a location has no
  position at all (the hit test keeps the previous one's);
* extra bones need still entries in the spring file (an extra bone without one swings under gravity).

A fighter that summons its weapons (Noctis: a sword, a great sword and a dagger for each hand, a spear and a shield,
each shown from a move's summon property, Tekken 7's 0x82a7, until a dismiss, 0x82a8) has a weapon per summon
(`summon`: its number); the runtime collapses a weapon not summoned. A hand holds one of them at a time, so its
weapons' points share slots (a point's `(where, slot)`): a slot takes the summoned weapon's point.

Written for this project from the files themselves (no third-party code).
"""
import numpy as np

from ..ttt2 import bone

OPT_FIRST = 800                     # the weapon bone posed by the animations' extra pair k: cha:OPT(800 + k)_..
STILL_FIRST = 820                   # weapons no animation poses: cha:OPT820_.. on
POINT_FIRST = 850                   # the hit points: cha:OPT850_.. on, hitting as TTT2's locations 70 on
POINT_LOCATION = 0x46
POINTS = 4                          # 70..73
POINT_NUMBERS = 50                  # point bones 850..899
NAME_LENGTH = 31
M = np.diag([1.0, -1.0, 1.0])       # columns: the weapon bone's axes in Tekken 7's bone's
_SWAP = np.eye(4)[[0, 2, 1, 3]]     # Tekken 7 bone coordinates (x, y, z) as model.load_tekken7_model keeps them


def point_name(number, code, slot=None):
    """A point's bone: hitting as location 70 + its slot (number - 850 unless one is given)."""
    return 'cha:OPT%d_hit%02x' % (number, code) + ('_%d' % slot if slot is not None else '')


def _point(name):
    """(number, Tekken 7 code, slot) of a point's bone name, else None."""
    head, _, tail = name.partition('_')
    if not (head.startswith('cha:OPT') and head[7:].isdigit() and tail.startswith('hit')):
        return None
    number = int(head[7:])
    code, _, slot = tail[3:].partition('_')
    return number, int(code, 16), int(slot) if slot else number - POINT_FIRST


def _numbers(skel):
    """{extra bone number: bone index} (by the names, cha:OPT<n>_..)."""
    index = {}
    for i, name in enumerate(skel['names']):
        head = name.split('_', 1)[0]
        if head.startswith('cha:OPT') and head[7:].isdigit():
            index[int(head[7:])] = i
    return index


def points_of(names):
    """{Tekken 7 hit location code: TTT2 hit location} of a skeleton's weapon points (by point_name)."""
    out = {}
    for name in names:
        point = _point(name)
        if point:
            out[point[1]] = POINT_LOCATION + point[2]
    return out


def of_skeleton(skel, definitions=()):
    """[(k = number - 800, parent bone index, [(point number - 850, slot)], summon or None)] of a skeleton's
    weapons, for character.ini; definitions: the fighter's WEAPONS (their summons, by the bones' names)."""
    index = _numbers(skel)
    out = []
    for n in sorted(index):
        if OPT_FIRST <= n < POINT_FIRST:
            b = index[n]
            points = [(m - POINT_FIRST, _point(skel['names'][index[m]])[2]) for m in sorted(index)
                      if m >= POINT_FIRST and skel['parent'][index[m]] == b]
            summon = next((w.get('summon') for w in definitions
                           if (OPT_FIRST + w['pair'] == n if w.get('pair') is not None else
                               ('cha:OPT%d_%s' % (n, w['bone']))[:NAME_LENGTH] == skel['names'][b])), None)
            out.append((n - OPT_FIRST, skel['parent'][b], points, summon))
    return out


def number_of(skel, weapon):
    """The extra bone number a weapon gets: OPT 800 + its pair, or the next free from 820 (a still one)."""
    if weapon.get('pair') is not None:
        return OPT_FIRST + weapon['pair']
    used = _numbers(skel)
    return next(n for n in range(STILL_FIRST, POINT_FIRST) if n not in used)


def append_bones(skel, t7bones, weapon):
    """The fighter's skeleton with a weapon's bone and its hit points after its own.
    t7bones: {'names', 'parents', 'world'} of the Tekken 7 skeleton holding the weapon's bone (model.load_tekken7_model:
    TTT2 space, mm); weapon: the definition's entry {'bone', 'parent' (a TTT2 bone), 'pair' (None: no animation poses
    it), 'points': {code: a Tekken 7 bone's name, or cm along the weapon, or (either, slot)}}.
    -> (skeleton, the weapon bone's index, its number, the point numbers used)."""
    names = [n[4:] if n.startswith('cha:') else n for n in skel['names']]
    parent = names.index(weapon['parent'])
    t7 = {n: i for i, n in enumerate(t7bones['names'])}
    i = t7[weapon['bone']]
    world_t7 = np.array(t7bones['world'], np.float64)

    def axes(k):
        a = (world_t7[k] @ _SWAP)[:3, :3]                          # columns: Tekken 7's axes, in TTT2 space
        return a / np.linalg.norm(a[:, 0]), float(np.linalg.norm(a[:, 0]))

    a, scale = axes(i)
    hw = bone.world_matrices(skel)
    world = np.eye(4)
    world[:3, :3] = a @ M
    world[:3, 3] = world_t7[i][:3, 3]
    number = number_of(skel, weapon)
    s = dict(skel)
    for key in ('names', 'parent', 'opt'):
        s[key] = list(skel[key])
    s['names_raw'] = None
    ts, rs, ss = list(skel['t']), list(skel['r']), list(skel['s'])

    def add(name, opt, up, local):
        s['names'].append(name[:NAME_LENGTH])
        s['parent'].append(up)
        s['opt'].append(opt)
        ts.append(local[:3, 3])
        rs.append(bone.euler_xyz_from_matrix(local[:3, :3]))
        ss.append(np.ones(3))
        return len(s['names']) - 1

    index = add('cha:OPT%d_%s' % (number, weapon['bone']), number, parent, np.linalg.inv(hw[parent]) @ world)
    used = []
    for code, where in sorted(weapon.get('points', {}).items()):
        n = POINT_FIRST + len([o for o in s['opt'] if o >= POINT_FIRST])
        slot = None
        if isinstance(where, tuple):
            where, slot = where
            if not 0 <= slot < POINTS or n >= POINT_FIRST + POINT_NUMBERS:
                raise ValueError('weapon point slot %d (the slots are 0..%d)' % (slot, POINTS - 1))
        elif n >= POINT_FIRST + POINTS:
            raise ValueError('more than %d weapon points' % POINTS)
        if isinstance(where, str):
            x = world_t7[t7[where]][:3, 3]
        else:                                                      # cm along the weapon (Tekken 7's x)
            x = world_t7[i][:3, 3] + a[:, 0] * where * 10.0 * scale
        local = np.eye(4)
        local[:3, 3] = world[:3, :3].T @ (x - world[:3, 3])
        add(point_name(n, code, slot), n, index, local)
        used.append(n)
    s['t'], s['r'], s['s'] = np.array(ts, np.float32), np.array(rs, np.float32), np.array(ss, np.float32)
    return s, index, number, used


def triangles(prim, bone_index):
    """A primitive with only the triangles all of whose corners are weighted mostly to the weapon's bone."""
    main = prim['joints'][np.arange(len(prim['joints'])), np.argmax(prim['weights'], 1)]
    keep = (main[prim['tris']] == bone_index).all(1)
    return dict(prim, tris=prim['tris'][keep])
