"""Unit tests of the Tekken 7 effect converter's pure pieces on made-up data (no game files needed).

    python3 -m unittest discover tools/characters/tests
"""
import math
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.convert import particles as P            # noqa: E402


class FakeModule:
    def __init__(self, props):
        self.props = props

    def get(self, name):
        return self.props.get(name)


class FakeEmitter:
    def __init__(self, modules):
        self.modules = modules

    def module(self, cls):
        return self.modules.get(cls)


class LitTexels(unittest.TestCase):
    def test_bright_colours_saturate_toward_white(self):
        img = np.zeros((1, 2, 4), np.float32)
        img[0, 0] = (0.5, 0.1, 1.0, 1.0)                # a purple texel, opaque
        img[0, 1] = (0.5, 0.1, 1.0, 0.1)                # the same, thin
        out = P.lit_texels(img, (9.3, 3.6, 10.0), 5.0, additive=False)
        r, g, b, a = out[0, 0]
        self.assertGreater(r, 0.95)                     # 4.65 tone-mapped: nearly full
        self.assertGreater(b, 0.95)
        self.assertGreater(g, 0.5)                      # 0.36: a mid value, lifted by the curve and gamma
        self.assertLess(g, r)
        self.assertAlmostEqual(float(a), 1.0)
        self.assertAlmostEqual(float(out[0, 1, 3]), 0.5, places=5)   # opacity above 1 thickens: 0.1 x 5

    def test_additive_weighs_colour_by_opacity(self):
        img = np.ones((1, 1, 4), np.float32)
        img[0, 0, 3] = 0.0
        out = P.lit_texels(img, (1.0, 1.0, 1.0), 1.0, additive=True)
        self.assertTrue(np.allclose(out[0, 0, :3], 0.0, atol=1e-3))
        self.assertAlmostEqual(float(out[0, 0, 3]), 1.0)

    def test_sub_colour_switch(self):
        self.assertTrue(P._subtractive({'switches': {'UseSubColor': True}}))
        self.assertFalse(P._subtractive({'switches': {}}))

    def test_black_stays_black(self):
        img = np.ones((1, 1, 4), np.float32)
        out = P.lit_texels(img, (0.0, 0.0, 0.0), 1.0, additive=False)
        self.assertTrue(np.allclose(out[0, 0, :3], 0.0, atol=1e-3))
        self.assertAlmostEqual(float(out[0, 0, 3]), 1.0)


class Shapes(unittest.TestCase):
    def test_s_curve(self):
        x = np.array([0.0, 0.25, 0.5, 0.75, 1.0])
        self.assertTrue(np.allclose(P.s_curve(x, 1.0), x))
        steep = P.s_curve(x, 10.0)
        self.assertLess(steep[1], 0.01)
        self.assertGreater(steep[3], 0.99)
        self.assertAlmostEqual(float(steep[2]), 0.5)

    def test_tiling_zero_is_one_value_along_the_axis(self):
        img = np.zeros((4, 2, 4), np.float32)
        img[1, :, 3] = 1.0                              # one bright row of four
        out = P._tiled(img, 1.0, 0.0)
        self.assertTrue(np.allclose(out[..., 3], 0.5))  # root mean square of (0, 1, 0, 0)
        self.assertEqual(out.shape, img.shape)

    def test_tiling_repeats_and_crops(self):
        img = np.arange(16, dtype=np.float32).reshape(2, 2, 4)
        self.assertEqual(P._tiled(img, 2.0, 1.0).shape, (2, 4, 4))
        half = P._tiled(img, 0.5, 1.0)                  # the first column stretched over the whole width
        self.assertEqual(half.shape, img.shape)
        self.assertTrue(np.allclose(half[:, 0], half[:, 1]))


class Frames(unittest.TestCase):
    def test_mesh_rotation(self):
        roll, pitch, yaw = P.turns_to_ttt2([0.25, 0.0, 0.0]), P.turns_to_ttt2([0.0, 0.25, 0.0]), \
            P.turns_to_ttt2([0.0, 0.0, 0.25])
        self.assertTrue(np.allclose(roll, [0.0, 0.0, -math.pi / 2]))
        self.assertTrue(np.allclose(pitch, [-math.pi / 2, 0.0, 0.0]))
        self.assertTrue(np.allclose(yaw, [0.0, -math.pi / 2, 0.0]))

    def test_floor_emitters(self):
        self.assertTrue(P._on_floor(FakeEmitter({'FloorHeight': FakeModule({})})))
        self.assertTrue(P._on_floor(FakeEmitter({'PM_CharacterBone': FakeModule({'FloorHeight': True})})))
        self.assertFalse(P._on_floor(FakeEmitter({'PM_CharacterBone': FakeModule({'FloorHeight': False})})))
        self.assertFalse(P._on_floor(FakeEmitter({})))


class Copies(unittest.TestCase):
    def test_effects_end_with_their_moves(self):
        import collections
        from ttt2char.convert import moveset

        class K:
            tables = {'moves': [{'anim_len': 55}, {'anim_len': 83}, {'anim_len': 0}, {'anim_len': 300}]}

        c = moveset.Converter.__new__(moveset.Converter)
        c.k, c.effect_rows, c.effect_ids, c.stats = K(), [], {108, 104}, collections.Counter()
        rows = [dict(value=v) for v in (108, 108, 108, 104, 0x900 | 104)]
        for row, move, f in zip(rows, (0, 1, 3, 0, 2), (0x8001, 0x8001, 0, 5, 5)):
            c.effect_rows.append((row, row['value'] & 0xFF, c.frames_left(move, f)))
        out = c.split_effects({108: 126, 104: 12}, used={229})
        self.assertEqual(out[108], (108, 126))          # the 300-frame move: the whole effect, under its id
        self.assertEqual(out[228], (108, 84))           # 83 frames -> rounded up to 84, a copy
        self.assertEqual(out[227], (108, 60))           # 55 -> 60
        self.assertEqual([r['value'] for r in rows[:3]], [227, 228, 108])
        self.assertEqual(out[104], (104, 12))           # shorter than its moves: no copy
        self.assertEqual(rows[4]['value'], 0x900 | 104)
        self.assertEqual(c.effect_ids, {108, 104, 228, 227})


if __name__ == '__main__':
    unittest.main()
