"""Hair and tails Tekken 7 simulates -> swinging extra bones of the added fighter.

Tekken 7 simulates some chains of a costume's bones (Lucky Chloe's twin tails and her tail) and models them straight
out from the body: in the bind pose her twin tails stand 1.3 m out sideways and her tail points up behind her, and
only its physics lets them fall. TTT2 swings a fighter's extra bones that have an entry in its spring file (Lili's
hair: chains of `cha:OPT<n>_..` bones off the head, along their local X axes, every entry the same with flag 0, and
one collider round the neck). So, for each chain a definition names (CHAINS):
* the chain is posed hanging: each link turned toward the definition's direction for it, the vertices with it
  (linear blend skinning), so the bind pose is the rest pose the springs swing around;
* its bones join the skeleton as `cha:OPT<n>_<name>` (n from OPT_FIRST; the first one's parent: the TTT2 bone its
  Tekken 7 parent's skin weights go to) and take the chain's skin weights;
* the spring file gets an entry per bone, a copy of Lili's hair entry, and Lili's colliders.

Written for this project from the files themselves (no third-party code).
"""
import re
import struct

import numpy as np

from ..ttt2 import bone

OPT_FIRST = 300                     # the first chain bone's number (cha:OPT300_..)
NAME_LENGTH = 31                    # a .bon name's longest
SPRING_TEMPLATE = ('lil', 160)      # the entry the chain bones copy: Lili's first hair bone (flag 0: swings)

# The spring file (big-endian): f32 5.0 and 28 zero bytes, u32 count and 12 zero bytes, count x 0x60-byte entries
# (u32 OPT number first), then u32 count and 12 zero bytes, count x 0x50-byte colliders (u32 1, u32 bone index, ..)
MOP_COUNT, MOP_ENTRIES, MOP_ENTRY, MOP_COLLIDER = 0x20, 0x30, 0x60, 0x50


def find(k, part, root):
    """The merged bone indices of a chain: part `part`'s bone `root` and, link by link, its child named like it
    (Hair_L_OPT001 -> Hair_L_OPT002 .. Hair_L_OPT007end). k: model.load_tekken7_model's result."""
    sk = k['parts'][part]
    names, parents = list(sk['names']), list(sk['parents'])
    if root not in names:
        raise ValueError('the Tekken 7 part %s has no bone %s' % (part, root))
    prefix = re.sub(r'\d+$', '', root)
    chain = [names.index(root)]
    while True:
        kids = [j for j, p in enumerate(parents) if p == chain[-1] and names[j].startswith(prefix)]
        if not kids:
            break
        chain.append(kids[0])
    return [int(sk['remap'][j]) for j in chain]


def _unit(v):
    v = np.asarray(v, np.float64)
    return v / np.linalg.norm(v)


def turn(a, b):
    """The rotation matrix turning unit vector a onto unit vector b by the shortest arc."""
    c = float(a @ b)
    if c < -1 + 1e-9:                                   # opposite: half a turn about any axis across a
        axis = np.cross(a, [1.0, 0.0, 0.0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0.0, 1.0, 0.0])
        axis = _unit(axis)
        return 2 * np.outer(axis, axis) - np.eye(3)
    v = np.cross(a, b)
    x = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + x + x @ x / (1 + c)


def drape(k, chains):
    """Poses chains hanging. chains: [(merged bone indices, link directions)]: link i (bone i -> bone i + 1) turns
    toward direction i (the last direction serves the links after it); the last bone keeps the last link's turn.
    Changes k's bind matrices and its (shared) vertex streams in place."""
    world = k['bones']['world']
    before = world.copy()
    for bones, directions in chains:
        at = before[bones[0]][:3, 3].copy()
        R = np.eye(3)
        for i, b in enumerate(bones):
            if i + 1 < len(bones):
                link = before[bones[i + 1]][:3, 3] - before[b][:3, 3]
                length = float(np.linalg.norm(link))
                want = _unit(directions[min(i, len(directions) - 1)])
                R = turn(link / length, want)
            world[b][:3, :3] = R @ before[b][:3, :3]
            world[b][:3, 3] = at
            if i + 1 < len(bones):
                at = at + length * want
    moved = sorted({b for bones, _ in chains for b in bones})
    delta = np.tile(np.eye(4), (len(world), 1, 1))
    delta[moved] = world[moved] @ np.linalg.inv(before[moved])
    s = k['prims'][0]
    touched = np.isin(s['joints'], moved) & (s['weights'] > 0)
    v = np.nonzero(touched.any(1))[0]
    m = (s['weights'][v][:, :, None, None] * delta[s['joints'][v]]).sum(1)
    s['pos'][v] = np.einsum('nij,nj->ni', m[:, :3, :3], s['pos'][v]) + m[:, :3, 3]
    nrm = np.einsum('nij,nj->ni', m[:, :3, :3], s['nrm'][v])
    s['nrm'][v] = nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
    s['tan'][v, :3] = np.einsum('nij,nj->ni', m[:, :3, :3], s['tan'][v, :3])
    return len(v)


def append_bones(skel, k, chains, parents):
    """The fighter's skeleton (a .bon dict) with the chains' bones after its own: [(merged bone indices)] after
    drape; parents: the TTT2 bone index of each chain's first bone's parent. -> (skeleton, [[new bone index]])."""
    hw = bone.world_matrices(skel)
    s = dict(skel)
    for key in ('names', 'parent', 'opt'):
        s[key] = list(skel[key])
    s['names_raw'] = None                       # bone.write makes them from the names
    ts, rs, ss = list(skel['t']), list(skel['r']), list(skel['s'])
    out, frames = [], {}
    number = OPT_FIRST
    for bones, parent in zip(chains, parents):
        made = []
        for i, b in enumerate(bones):
            w = np.array(k['bones']['world'][b], np.float64)
            w[:3, :3] /= np.linalg.norm(w[:3, :3], axis=0)          # Tekken 7's bone scales: the vertices have them
            up = hw[parent] if i == 0 else frames[made[-1]]
            local = np.linalg.inv(up) @ w
            rot = local[:3, :3] / np.linalg.norm(local[:3, :3], axis=0)
            index = len(s['names'])
            frames[index] = w
            name = k['bones']['names'][b].split('@')[0]
            s['names'].append(('cha:OPT%d_%s' % (number, name))[:NAME_LENGTH])
            s['parent'].append(parent if i == 0 else made[-1])
            s['opt'].append(number)
            ts.append(local[:3, 3])
            rs.append(bone.euler_xyz_from_matrix(rot))
            ss.append(np.ones(3))
            made.append(index)
            number += 1
        out.append(made)
    s['t'], s['r'], s['s'] = np.array(ts, np.float32), np.array(rs, np.float32), np.array(ss, np.float32)
    return s, out


def empty_spring_file(template_mop):
    """A spring file with no entries and no colliders (for a donor without one), with the template's header."""
    return template_mop[:MOP_COUNT] + struct.pack('>I', 0) + bytes(12) + struct.pack('>I', 0) + bytes(12)


def spring_file(mop, template_mop, numbers, template=SPRING_TEMPLATE[1], links=True):
    """A spring file with an entry per chain bone (OPT number in `numbers`), copied from the template's entry for
    number `template`, after `mop`'s entries, and (links) the template's colliders after `mop`'s."""
    def entries(m):
        n, = struct.unpack_from('>I', m, MOP_COUNT)
        return [m[MOP_ENTRIES + MOP_ENTRY * i:MOP_ENTRIES + MOP_ENTRY * (i + 1)] for i in range(n)]

    def colliders(m):
        at = MOP_ENTRIES + MOP_ENTRY * struct.unpack_from('>I', m, MOP_COUNT)[0]
        n, = struct.unpack_from('>I', m, at)
        return [m[at + 16 + MOP_COLLIDER * i:at + 16 + MOP_COLLIDER * (i + 1)] for i in range(n)]

    entry = next((e for e in entries(template_mop) if struct.unpack_from('>I', e)[0] == template), None)
    if entry is None:
        raise ValueError('the template spring file has no entry %d' % template)
    out = entries(mop)
    for number in numbers:
        e = bytearray(entry)
        struct.pack_into('>I', e, 0, number)
        out.append(bytes(e))
    links = colliders(mop) + (colliders(template_mop) if links else [])
    return (mop[:MOP_COUNT] + struct.pack('>I', len(out)) + bytes(12) + b''.join(out)
            + struct.pack('>I', len(links)) + bytes(12) + b''.join(links))
