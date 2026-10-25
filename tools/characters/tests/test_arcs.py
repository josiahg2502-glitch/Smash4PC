"""Unit tests of ttt2char.convert.arcs (Tekken 7's 2D arcs) on made-up animations (no game files needed).

    python3 -m pytest -q tools/characters/tests/test_arcs.py
"""
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.convert import arcs  # noqa: E402
from ttt2char.ttt2 import animation  # noqa: E402


def made_up(frames, typ=animation.RAW, hips=1000.0, seed=1):
    """A decoded animation: 23 body channels with TTT2's descriptors, random rotations, hips at `hips`."""
    rng = np.random.default_rng(seed)
    desc = [11, 11, 5, 7, 7, 7, 11, 7, 7, 7, 7, 6, 7, 7, 7, 6, 7, 7, 6, 7, 7, 6, 7]
    v = rng.uniform(-1.0, 1.0, (frames, len(desc), 3)).astype(np.float32)
    v[:, 1, 1] = hips
    return dict(type=typ, frames=frames, descriptors=desc, values=v)


class Arcs(unittest.TestCase):
    def test_params(self):
        self.assertEqual(arcs.params(0x82466400), (130, 7.0, 100))
        self.assertEqual(arcs.params(0x6E3C6400), (110, 6.0, 100))
        self.assertEqual(arcs.params(0x00960000), (0, 15.0, 0))

    def test_height_is_ttt2_physics(self):
        # TTT2's physics (sub_04336854): vy -= g, then y += vy, once a frame, from vy = a
        a, g, y, vy = 130.0, 7.0, 0.0, 130.0
        for n in range(1, 40):
            vy -= g
            y += vy
            self.assertAlmostEqual(arcs.height(n, a, g), y, places=3)

    def test_touchdown(self):
        self.assertEqual(arcs.touchdown(130, 7.0), 37)     # Akuma: 20 A / B = 37.1 frames
        self.assertEqual(arcs.touchdown(110, 6.0), 36)     # Eliza
        self.assertGreater(arcs.height(arcs.touchdown(110, 6.0) - 1, 110, 6.0), 0)
        self.assertLessEqual(arcs.height(arcs.touchdown(110, 6.0), 110, 6.0), 0)

    def test_ground_or_air(self):
        self.assertTrue(arcs.is_ground_arc(False, 9))
        self.assertFalse(arcs.is_ground_arc(True, 10))       # an air move's own arc (dive kick)
        self.assertFalse(arcs.is_ground_arc(False, 0x8009))  # on the second counter: kept for the runtime

    def test_bake_raw(self):
        dec = made_up(44)
        f0, a, g, hold = 9, 130, 7.0, 43
        desc, out, ext = arcs.bake(dec, f0, a, g, hold)
        self.assertEqual(ext, f0 + arcs.touchdown(a, g) - hold)
        self.assertEqual(out.shape[0], 44 + ext)
        self.assertEqual(desc, dec['descriptors'])
        # before the arc: unchanged; the held frame repeats; the arc is in the hips height only
        np.testing.assert_array_equal(out[:f0], dec['values'][:f0])
        for f in range(hold, hold + ext + 1):
            np.testing.assert_array_equal(np.delete(out[f], 1, 0), np.delete(dec['values'][hold], 1, 0))
        for f in range(f0, out.shape[0]):
            self.assertAlmostEqual(float(out[f, 1, 1]), 1000.0 + max(0.0, arcs.height(f - f0, a, g)), places=2)
        self.assertAlmostEqual(float(out[f0 + arcs.touchdown(a, g), 1, 1]), 1000.0, places=3)

    def test_bake_compressed_keeps_the_pose(self):
        dec = made_up(20, animation.COMPRESSED)
        desc, out, _ = arcs.bake(dec, 5, 30, 6.0, 19)
        for c, d in enumerate(dec['descriptors']):
            if animation.is_rotation(d):
                self.assertEqual(desc[c], 7)
                want = animation.local_rotation(dec['values'][:5, c], animation.COMPRESSED)
                got = animation.local_rotation(out[:5, c], animation.RAW)
                np.testing.assert_allclose(got, want, atol=1e-5)
        raw = arcs.encode(desc, out)
        back = animation.decode(raw, '>')
        self.assertEqual(back['type'], animation.RAW)
        np.testing.assert_array_equal(back['values'], out)

    def test_absolute_sabun(self):
        whole = made_up(44, hips=1271.0)
        whole['values'][:5, 1, 1] = 750.0                    # take-off and landing frames lower
        delta = made_up(40, hips=0.0, seed=2)
        delta['values'][:5, 1, 1] = 700.0                    # absolute take-off frames
        delta['values'][5:38, 1, 1] = np.linspace(-180, 180, 33)
        delta['values'][38:, 1, 1] = 1000.0                  # absolute landing frames
        out, base, n = arcs.absolute_sabun(delta, whole)
        self.assertEqual(base, 1271.0)
        self.assertEqual(n, 33)
        np.testing.assert_allclose(out['values'][5:38, 1, 1], 1271.0 + np.linspace(-180, 180, 33), rtol=1e-6)
        np.testing.assert_array_equal(out['values'][:5, 1, 1], 700.0)
        np.testing.assert_array_equal(out['values'][38:, 1, 1], 1000.0)


if __name__ == '__main__':
    unittest.main()
