"""Unit tests of the model converter's pure pieces on made-up data (no game files needed).

    python3 -m unittest discover tools/characters/tests
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.convert import model                     # noqa: E402

M = '/Game/Character/Item/Meshes/xyz/'


class WornParts(unittest.TestCase):
    def test_dependency_replaces_the_mesh_in_its_folder(self):
        parts = [('fac', M + 'Face/xyz_fac/Meshes/SK_CH_xyz_fac'),
                 ('hed', M + 'Head/xyz_hed_1p/Meshes/SK_CH_xyz_hed_1p'),
                 ('har', M + 'Hair/xyz_har_1p/Meshes/SK_CH_xyz_har_1p')]
        # the game's reference spells the folder in another case
        worn = model.worn_parts(parts, [M + 'Hair/XYZ_har_1p/Meshes/SK_CH_xyz_har_1p_E'])
        self.assertEqual(worn[:2], parts[:2])
        self.assertEqual(worn[2], ('har', M + 'Hair/XYZ_har_1p/Meshes/SK_CH_xyz_har_1p_E'))

    def test_no_dependency_or_the_same_mesh(self):
        parts = [('har', M + 'Hair/xyz_har_1p/Meshes/SK_CH_xyz_har_1P')]
        self.assertEqual(model.worn_parts(parts, []), parts)
        self.assertEqual(model.worn_parts(parts, [parts[0][1]]), parts)
        self.assertEqual(model.worn_parts(parts, [M + 'Hair/other/Meshes/SK_CH_xyz_har_1p_B']), parts)


class CostumeItems(unittest.TestCase):
    def test_socket_items_and_dependencies(self):
        ref = "StaticMesh'/Game/X/SM_CH_xyz_gla_1p.SM_CH_xyz_gla_1p'"
        items = [{'MeshType': 'SkeletalMeshAnim', 'SourceSkeletalMesh': "SkeletalMesh'/Game/X/SK_a.SK_a'"},
                 {'MeshType': 'StaticMeshSocket', 'SourceStaticMesh': ref, 'ItemOffsets': {'Translation': [0, 12, 5]}},
                 {'MeshType': 'StaticMeshSocket', 'SourceStaticMesh': ref, 'SocketPosition': 'R_HAND'},
                 {'MeshType': 'SkeletalMeshAnim', 'DependencyMesh': "SkeletalMesh'/Game/H/SK_b_E.SK_b_E'"}]
        self.assertEqual(model.socket_items(items), [('/Game/X/SM_CH_xyz_gla_1p', {'Translation': [0, 12, 5]})])
        self.assertEqual(model.dependency_meshes(items), ['/Game/H/SK_b_E'])


class InFront(unittest.TestCase):
    def test_points_against_a_cover(self):
        import numpy as np
        # a 100 x 100 mm square facing the front at z = 10, split into two triangles
        a, b, c, d = (0, 0, 10), (100, 0, 10), (100, 100, 10), (0, 100, 10)
        cover = np.array([[a, b, c], [a, c, d]], np.float64)
        points = np.array([[50, 50, 20],      # in front of it
                           [50, 50, 9],       # just behind it (within the margin)
                           [50, 50, -50],     # behind it
                           [150, 50, 20],     # beside it
                           [20, 80, 10.5]], np.float64)
        self.assertEqual(model.in_front(points, cover).tolist(), [True, True, False, False, True])

    def test_holes_in_the_cover_are_not_in_front(self):
        import numpy as np
        # one triangle: the grid's other half has no surface
        cover = np.array([[(0, 0, 10), (100, 0, 10), (0, 100, 10)]], np.float64)
        points = np.array([[10, 10, 20], [90, 90, 20]], np.float64)
        self.assertEqual(model.in_front(points, cover).tolist(), [True, False])


if __name__ == '__main__':
    unittest.main()
