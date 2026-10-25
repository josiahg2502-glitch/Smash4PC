"""Unit tests of the background dumper's decisions (tekken7/autodump.py), with Steam's answers made up.

    python3 -m unittest discover tools/characters/tests
"""
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.tekken7 import autodump                 # noqa: E402


class Ownership(unittest.TestCase):
    def unowned(self, fighters, apps):
        """autodump.unowned(fighters) with Steam saying the DLC `apps` are owned."""
        answer = {app: app in apps for app in autodump.DLC.values()}
        with mock.patch.object(autodump, 'steam_running', return_value=True), \
                mock.patch.object(autodump, 'find_tekken7'), mock.patch.object(autodump, 'build_helper'), \
                mock.patch.object(autodump, 'owned_dlc', return_value=answer):
            return autodump.unowned(fighters)

    def test_unowned_dlc_fighters_are_skipped(self):
        self.assertEqual(self.unowned([26, 36, 50, 56], {autodump.DLC[50]}),          # Negan's DLC only
                         {36: 'its DLC is not owned', 56: 'its DLC is not owned'})

    def test_base_fighters_need_no_steam(self):
        with mock.patch.object(autodump, 'steam_running', return_value=False), \
                mock.patch.object(autodump, 'owned_dlc') as asked:
            self.assertEqual(autodump.unowned([26, 22, 8]), {})               # Kazumi, Lucky Chloe, Heihachi
            asked.assert_not_called()

    def test_dlc_fighters_need_steam(self):
        with mock.patch.object(autodump, 'steam_running', return_value=False):
            with self.assertRaises(autodump.DumpError):
                autodump.unowned([50])


if __name__ == '__main__':
    unittest.main()
