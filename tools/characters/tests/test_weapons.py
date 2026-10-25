"""Unit tests of the weapons' extra bones (convert/weapons.py) on made-up skeletons.

    python3 -m unittest discover tools/characters/tests
"""
import os
import struct
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char import pack                              # noqa: E402
from ttt2char.convert import chains, weapons           # noqa: E402
from ttt2char.ttt2 import bone                         # noqa: E402

SWAP = np.eye(4)[[0, 2, 1, 3]]


def matrix(t, r):
    m = np.eye(4)
    m[:3, :3] = bone.matrix_from_euler_xyz(np.asarray(r, np.float64))
    m[:3, 3] = t
    return m


def fighter():
    """A two-bone TTT2 skeleton: the root and a right hand turned about all axes."""
    return dict(scale=1.0, parent=[-1, 0], t=np.array([[0, 1000, 0], [-500, 400, 50]], np.float32),
                r=np.array([[0, 0, 0], [0.3, -0.7, 1.1]], np.float32), s=np.ones((2, 3), np.float32), opt=[0],
                params=[1.0] * 8, names=['cha:HARA_ROT1', 'cha:TE_R_jnt'], names_raw=None)


def tekken7(hand_world, d, scale=0.95):
    """Tekken 7 bones (in TTT2 space, as model.load_tekken7_model keeps them): its hand bound where the fighter's is,
    its axes the fighter's turned by d (P = d^T), a weapon bone under it, the weapon's tip 92 cm along its x."""
    def t7(axes, at):
        m = np.eye(4)
        m[:3, :3] = axes * scale
        m[:3, 3] = at
        return m @ SWAP             # as load_tekken7_model keeps them (SWAP G SWAP: times SWAP, the axes in TTT2 space)
    hand = t7(hand_world[:3, :3] @ d, hand_world[:3, 3])
    weapon_axes = hand_world[:3, :3] @ bone.matrix_from_euler_xyz(np.array([0.4, 0.2, -0.9])) @ np.diag([1, 1, -1])
    at = hand_world[:3, 3] + hand_world[:3, :3] @ np.array([80.0, 30.0, 10.0])
    weapon = t7(weapon_axes, at)
    tip = t7(weapon_axes, at + weapon_axes[:, 0] * 920 * scale)
    return dict(names=['R_Hand', 'ATH_R_weapon001', 'ATH_R_weapon001_top'], parents=[-1, 0, 1],
                world=[hand, weapon, tip])


class Weapons(unittest.TestCase):
    def test_bones_points_and_the_runtime(self):
        skel = fighter()
        hand = bone.world_matrices(skel)[1]
        t7 = tekken7(hand, np.diag([1.0, -1.0, 1.0]))
        w = dict(bone='ATH_R_weapon001', parent='TE_R_jnt', pair=1, points={0x70: 'ATH_R_weapon001_top', 0x6f: 45})
        s, index, number, used = weapons.append_bones(skel, t7, w)
        self.assertEqual((index, number, used, s['opt'][-3:]), (2, 801, [850, 851], [801, 850, 851]))
        self.assertEqual(s['names'][2:], ['cha:OPT801_ATH_R_weapon001', 'cha:OPT850_hit6f', 'cha:OPT851_hit70'])
        self.assertEqual(s['parent'], [-1, 0, 1, 2, 2])
        world = bone.world_matrices(bone.read(bone.write(s)))
        t7w = [np.asarray(m) @ SWAP for m in t7['world']]           # columns: Tekken 7's axes, in TTT2 space
        a = t7w[1][:3, :3] / np.linalg.norm(t7w[1][:3, 0])
        self.assertTrue(np.allclose(world[2][:3, :3], a @ weapons.M, atol=1e-5))  # Tekken 7's axes, y reversed
        self.assertTrue(abs(np.linalg.det(world[2][:3, :3]) - 1) < 1e-5)
        self.assertTrue(np.allclose(world[2][:3, 3], t7w[1][:3, 3], atol=1e-2))
        self.assertTrue(np.allclose(world[4][:3, 3], t7w[2][:3, 3], atol=1e-2))     # the tip, Tekken 7's own
        self.assertTrue(np.allclose(world[3][:3, 3], t7w[1][:3, 3] + a[:, 0] * 450 * 0.95, atol=1e-2))  # 45 cm
        # the runtime writes a pair as the local transform, unchanged: the pair of Tekken 7's bind pose (in TTT2's
        # frame of the parent) puts the bone where it is bound
        local = np.linalg.inv(world[1]) @ world[2]
        r, t = hand[:3, :3].T @ a @ weapons.M, hand[:3, :3].T @ (t7w[1][:3, 3] - hand[:3, 3])
        self.assertTrue(np.allclose(r, local[:3, :3], atol=1e-5) and np.allclose(t, local[:3, 3], atol=1e-2))
        # character.ini, and the hit locations the moves get
        ini = pack.character_ini(type('D', (), dict(NAME='X', CODE='abc', DONOR='lar', SEX='male')), 3,
                                 weapons=weapons.of_skeleton(s))
        self.assertIn('weapons = 1:1:0,1\n', ini)
        self.assertEqual(weapons.points_of(s['names']), {0x6f: 0x46, 0x70: 0x47})

    def test_still_weapons_and_limits(self):
        skel = fighter()
        t7 = tekken7(bone.world_matrices(skel)[1], np.eye(3))
        still = dict(bone='ATH_R_weapon001', parent='TE_R_jnt', pair=None, points={1: 10, 2: 20})
        s, _, n1, _ = weapons.append_bones(skel, t7, still)
        s, _, n2, _ = weapons.append_bones(s, t7, dict(still, points={3: 30, 4: 40}))
        self.assertEqual((n1, n2), (820, 821))
        self.assertEqual([k for k, _, _, _ in weapons.of_skeleton(s)], [20, 21])
        with self.assertRaises(ValueError):                                        # only 70..73
            weapons.append_bones(s, t7, dict(still, points={5: 50}))

    def test_summoned_weapons_share_slots(self):
        skel = fighter()
        t7 = tekken7(bone.world_matrices(skel)[1], np.eye(3))
        sword = dict(bone='ATH_R_weapon001', parent='TE_R_jnt', pair=3, summon=3,
                     points={0x63: ('ATH_R_weapon001_top', 0), 0x66: (20, 1)})
        dagger = dict(sword, pair=5, summon=5, points={0x64: (30, 0)})
        s, _, _, _ = weapons.append_bones(skel, t7, sword)
        s, _, _, used = weapons.append_bones(s, t7, dagger)
        self.assertEqual(used, [852])
        self.assertEqual(s['names'][-1], 'cha:OPT852_hit64_0')
        self.assertEqual(weapons.points_of(s['names']), {0x63: 0x46, 0x66: 0x47, 0x64: 0x46})   # slots, not numbers
        listed = weapons.of_skeleton(s, [sword, dagger])
        self.assertEqual(listed, [(3, 1, [(0, 0), (1, 1)], 3), (5, 1, [(2, 0)], 5)])
        ini = pack.character_ini(type('D', (), dict(NAME='X', CODE='abc', DONOR='lar', SEX='male')), 3,
                                 weapons=listed)
        self.assertIn('weapons = 3:1:0,1:3 5:1:2>0:5\n', ini)
        with self.assertRaises(ValueError):                                        # slots are 0..3
            weapons.append_bones(s, t7, dict(dagger, points={0x65: (40, 4)}))

    def test_triangles(self):
        joints = np.array([[1, 0], [1, 2], [2, 0], [1, 0]])
        weights = np.array([[1.0, 0.0], [0.7, 0.3], [1.0, 0.0], [0.6, 0.4]])
        prim = dict(joints=joints, weights=weights, tris=np.array([[0, 1, 3], [0, 2, 3]]))
        self.assertEqual(weapons.triangles(prim, 1)['tris'].tolist(), [[0, 1, 3]])

    def test_still_spring_entries(self):
        def mop(numbers, links, fill):
            out = struct.pack('>f', 5.0) + bytes(28) + struct.pack('>I', len(numbers)) + bytes(12)
            for n in numbers:
                out += struct.pack('>I', n) + bytes([fill]) * (chains.MOP_ENTRY - 4)
            out += struct.pack('>I', len(links)) + bytes(12)
            return out + b''.join(bytes([x]) * chains.MOP_COLLIDER for x in links)
        out = chains.spring_file(mop([0], [5], 1), mop([600, 160], [73], 2), [800, 850], template=600, links=False)
        n, = struct.unpack_from('>I', out, 0x20)
        self.assertEqual([struct.unpack_from('>I', out, 0x30 + 0x60 * k)[0] for k in range(n)], [0, 800, 850])
        at = 0x30 + 0x60 * n
        self.assertEqual(struct.unpack_from('>I', out, at)[0], 1)                  # the donor's collider only


if __name__ == '__main__':
    unittest.main()
