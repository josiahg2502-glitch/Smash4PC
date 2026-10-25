"""Unit tests of the pack format (pack.PACK_FORMAT): what the importer writes, which packs are current (read as the
port reads them), the places the port gives them, an older importer's pack replaced whole in its place (never what a
link leads elsewhere, never a newer importer's pack), what a killed write left, shared/ made for current packs only,
--remove and --no-shared, and launcher.py status "outdated" and "newer". Made-up folders only; no game files needed.

    python3 -m pytest -q tools/characters/tests/test_packs.py
"""
import contextlib
import importlib.util
import io
import os
import shutil
import stat
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, HERE)

import launcher                                       # noqa: E402
from ttt2char import InputError, pack, shared, signals    # noqa: E402
from ttt2char.tekken7 import autodump                 # noqa: E402

spec = importlib.util.spec_from_file_location('import_character', os.path.join(HERE, 'import_character.py'))
import_character = importlib.util.module_from_spec(spec)
spec.loader.exec_module(import_character)

KAZUMI = mock.Mock(CODE='kzm', NAME='Kazumi Mishima', DONOR='jun', SEX='female')
CHLOE = mock.Mock(CODE='dnc', NAME='Lucky Chloe', DONOR='jun', SEX='female')


def write(path, text=''):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


def old_kazumi(folder):
    """Kazumi's pack as the importer of 2026-10-19 made it: no format, no slot, its own keys/ (texture pages made for
    it alone), the se_Dwi bank no longer made."""
    target = os.path.join(folder, 'kzm')
    write(os.path.join(target, 'character.ini'), '# Kazumi Mishima\ncode = kzm\ndonor = jun\nsex = female\n')
    for name in ('kzm.nud', 'se_Dwi_kzm.nus3bank', 'keys/013bf6f5.add', 'keys/6ccdfc60.bin', 'art/portrait_l.nut'):
        write(os.path.join(target, name), 'old')
    return target


def files_in(folder):
    return sorted(os.path.relpath(os.path.join(top, name), folder) for top, _, names in os.walk(folder)
                  for name in names)


def current_pack(folder, d, slot):
    return pack.write(folder, d, {'.nud': b'nud'}, slot)


class Format(unittest.TestCase):
    def test_written_in_every_character_ini(self):
        text = pack.character_ini(KAZUMI, 3, chr_tones=26)
        self.assertIn('format = %d\n' % pack.PACK_FORMAT, text)
        with tempfile.TemporaryDirectory() as d:
            current_pack(d, KAZUMI, 3)
            self.assertEqual(pack.read_ini(os.path.join(d, 'kzm', 'character.ini'))['format'], str(pack.PACK_FORMAT))
            [p] = pack.installed(d)
            self.assertEqual((p['format'], p['current'], p['slot']), (pack.PACK_FORMAT, True, 3))

    def test_older_and_newer_packs_are_not_current(self):
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)                                           # (no format, its own keys/: format 0)
            write(os.path.join(d, 'dnc', 'character.ini'), 'code = dnc\ndonor = jun\nslot = 1\n')     # (no format)
            write(os.path.join(d, 'elz', 'character.ini'), 'format = %d\ncode = elz\ndonor = jun\nslot = 2\n'
                  % (pack.PACK_FORMAT + 1))
            write(os.path.join(d, 'ita', 'character.ini'), 'format = x\ncode = ita\ndonor = jun\nslot = 3\n')
            self.assertEqual([(p['code'], p['format'], p['current'], p['newer']) for p in pack.installed(d)],
                             [('dnc', 1, True, False), ('elz', pack.PACK_FORMAT + 1, False, True),
                              ('ita', 0, False, False), ('kzm', 0, False, False)])

    def test_a_pack_without_a_format_line_and_its_own_keys_is_format_1(self):
        # (the importer's packs made before it wrote the line: they have no keys/ and work as format 1 ones)
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, 'dnc', 'character.ini'), '# Lucky Chloe\ncode = dnc\ndonor = jun\nslot = 1\n')
            write(os.path.join(d, 'dnc', 'dnc.nud'))
            write(os.path.join(d, 'mut', 'character.ini'), 'format = 0\ncode = mut\ndonor = jun\nslot = 2\n')
            self.assertEqual([(p['code'], p['format'], p['current']) for p in pack.installed(d)],
                             [('dnc', 1, True), ('mut', 0, False)])     # (a format given is the format)

    def test_numbers_as_the_port_reads_them(self):
        # roster.cpp ini_number: ASCII digits only, below 2**31; anything else is none (format 0, no slot)
        for value, form in (('1', 1), ('01', 1), (' 1 ', 1), ('1\r', 1), ('+1', 0), ('1.0', 0), ('1abc', 0),
                            ('1 2', 0), ('', 0), ('\u00b9', 0), ('\uff11', 0), ('\u0661', 0), ('\u00a01', 0),
                            ('4294967297', 0), ('2147483648', 0), ('1 # a comment', 1)):
            with tempfile.TemporaryDirectory() as d:
                write(os.path.join(d, 'kzm', 'character.ini'),
                      'format = %s\ncode = kzm\ndonor = jun\nslot = %s\n' % (value, value))
                [p] = pack.installed(d)
                self.assertEqual((p['format'], p['slot']), (form, form if form else None), repr(value))
        self.assertEqual((pack.number('2147483647'), pack.number('2147483648')), (2147483647, None))

    def test_lines_end_at_newlines_only(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, 'kzm', 'character.ini'), 'code = kzm\rdonor = jun\nformat = 1\x0bslot = 3\n')
            [p] = pack.installed(d)                                 # (one line "code = kzm\rdonor = jun": a code
            self.assertEqual((p['valid'], p['format']), (False, 0))  # that is no code; "1\x0bslot = 3": no number)

    def test_hidden_folders_are_no_packs(self):
        with tempfile.TemporaryDirectory() as d:
            for name in ('.kzm.part', '.kzm.old'):                  # (a killed write's, each with a character.ini)
                write(os.path.join(d, name, 'character.ini'), 'format = 1\ncode = kzm\ndonor = jun\nslot = 0\n')
            self.assertEqual(pack.installed(d), [])


class Places(unittest.TestCase):
    """pack.placed / places_of / choose_slot against the port's prepare_roster (roster.cpp)."""

    def test_two_packs_of_one_code(self):
        with tempfile.TemporaryDirectory() as d:
            current_pack(d, KAZUMI, 0)
            write(os.path.join(d, 'kzm-copy', 'character.ini'), 'code = kzm\ndonor = jun\nslot = 5\n')
            current_pack(d, CHLOE, 1)
            packs = pack.installed(d)
            self.assertEqual([(k, os.path.basename(p['path'])) for k, p in pack.placed(packs)],
                             [(0, 'kzm'), (1, 'dnc'), (5, 'kzm-copy')])
            self.assertEqual([(k, p['code']) for k, p in pack.loaded(packs)], [(0, 'kzm'), (1, 'dnc')])
            self.assertEqual(pack.places_of(packs), {'kzm': 0, 'dnc': 1})
            self.assertEqual(pack.choose_slot(d, 'elz'), 2)             # (the copy keeps place 5, as in the port)
            self.assertEqual([(k, f.CODE) for k, f, _ in shared.fighters_of(d, {'kzm': KAZUMI, 'dnc': CHLOE})],
                             [(0, 'kzm'), (1, 'dnc')])

    def test_packs_the_port_does_not_take_have_no_place(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, 'aaa', 'character.ini'), 'format = 1\ncode = aaa\nslot = 0\n')    # (no donor)
            write(os.path.join(d, 'bbb', 'character.ini'), 'format = 1\ncode = KZM\ndonor = jun\nslot = 0\n')
            write(os.path.join(d, 'kzm', 'character.ini'), 'format = 1\ncode = kzm\ndonor = jun\n')
            self.assertEqual(pack.places_of(pack.installed(d)), {'kzm': 0})

    def test_slots_past_the_last_place_are_none(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, 'kzm', 'character.ini'), 'format = 1\ncode = kzm\ndonor = jun\nslot = 130\n')
            current_pack(d, CHLOE, 0)
            self.assertEqual(pack.places_of(pack.installed(d)), {'dnc': 0, 'kzm': 1})


class Reimport(unittest.TestCase):
    def test_an_older_pack_is_replaced_whole_in_its_place(self):
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)
            current_pack(d, CHLOE, 1)
            slot = pack.choose_slot(d, 'kzm')
            self.assertEqual(slot, 0)                               # (it names none: the first free, as the port)
            target = pack.write(d, KAZUMI, {'.nud': b'new', 'art/portrait_l.nut': b'new', 'colours.json': b'{}'},
                                slot)
            self.assertEqual(files_in(target), ['art/portrait_l.nut', 'character.ini', 'colours.json', 'kzm.nud'])
            with open(os.path.join(target, 'kzm.nud')) as f:
                self.assertEqual(f.read(), 'new')
            self.assertEqual(sorted(os.listdir(d)), ['dnc', 'kzm'])  # (no working folder left)
            self.assertEqual({p['code']: (p['slot'], p['current']) for p in pack.installed(d)},
                             {'dnc': (1, True), 'kzm': (0, True)})

    def test_the_slot_it_names_is_kept(self):
        with tempfile.TemporaryDirectory() as d:
            target = old_kazumi(d)
            write(os.path.join(target, 'character.ini'), 'code = kzm\ndonor = jun\nslot = 5\n')
            current_pack(d, CHLOE, 0)
            self.assertEqual(pack.choose_slot(d, 'kzm'), 5)

    def test_an_older_packs_place_is_not_given_to_another_fighter(self):
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)                                           # (place 0, unloaded)
            current_pack(d, CHLOE, 1)
            self.assertEqual(pack.choose_slot(d, 'ita'), 2)

    def test_stopped_while_written_leaves_the_older_pack_as_it_was(self):
        with tempfile.TemporaryDirectory() as d:
            target = old_kazumi(d)
            before = files_in(target)
            real, count = open, [0]

            def stopping(path, *args, **kwargs):
                if '.kzm.part' in path:
                    count[0] += 1
                    if count[0] == 2:
                        raise SystemExit(143)                       # (the launcher's Cancel)
                return real(path, *args, **kwargs)

            with mock.patch('builtins.open', stopping), self.assertRaises(SystemExit):
                pack.write(d, KAZUMI, {'.nud': b'new', '.nut': b'new'}, 0)
            self.assertEqual(files_in(target), before)
            self.assertEqual(os.listdir(d), ['kzm'])

    def test_a_failed_swap_puts_the_earlier_pack_back(self):
        with tempfile.TemporaryDirectory() as d:
            target = old_kazumi(d)
            before = files_in(target)
            real = os.replace

            def replace(a, b):
                if a.endswith('.kzm.part'):
                    raise OSError('no room')
                real(a, b)

            with mock.patch.object(pack.os, 'replace', replace), self.assertRaises(OSError):
                pack.write(d, KAZUMI, {'.nud': b'new'}, 0)
            self.assertEqual(files_in(target), before)
            self.assertEqual(os.listdir(d), ['kzm'])

    def test_a_killed_writes_folders_go(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, '.kzm.part', 'kzm.nud'), 'half')
            write(os.path.join(d, '.kzm.old', 'character.ini'), 'code = kzm\n')
            pack.write(d, KAZUMI, {'.nud': b'new'}, 0)
            self.assertEqual(os.listdir(d), ['kzm'])
            write(os.path.join(d, '.kzm.part', 'kzm.nud'), 'half')
            pack.remove(os.path.join(d, 'kzm'))
            self.assertEqual(os.listdir(d), [])

    def test_a_link_aimed_elsewhere_is_never_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            mine = os.path.join(d, 'mine', 'packs')
            current_pack(mine, CHLOE, 1)
            write(os.path.join(mine, 'notes.txt'), 'mine')
            packs = os.path.join(d, 'packs')
            os.makedirs(packs)
            os.symlink(mine, os.path.join(packs, 'kzm'))
            with self.assertRaisesRegex(InputError, 'is a link to .* which is not Kazumi Mishima\'s pack'):
                pack.write(packs, KAZUMI, {'.nud': b'new'}, 0)
            self.assertEqual(sorted(os.listdir(mine)), ['dnc', 'notes.txt'])
            self.assertEqual(sorted(os.listdir(os.path.dirname(mine))), ['packs'])     # (nothing beside it either)
            os.remove(os.path.join(packs, 'kzm'))
            empty = os.path.join(d, 'empty')
            os.makedirs(empty)
            os.symlink(empty, os.path.join(packs, 'kzm'))           # (an empty folder: the pack goes there)
            pack.write(packs, KAZUMI, {'.nud': b'new'}, 0)
            self.assertEqual(files_in(empty), ['character.ini', 'kzm.nud'])

    def test_a_newer_importers_pack_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, 'kzm', 'character.ini'), 'format = %d\ncode = kzm\ndonor = jun\nslot = 0\n'
                  % (pack.PACK_FORMAT + 1))
            with self.assertRaisesRegex(InputError, 'made by a newer importer'):
                pack.write(d, KAZUMI, {'.nud': b'new'}, 0)
            self.assertEqual(files_in(os.path.join(d, 'kzm')), ['character.ini'])

    def test_a_read_only_folder_in_the_earlier_pack(self):
        with tempfile.TemporaryDirectory() as d:
            target = old_kazumi(d)
            os.chmod(os.path.join(target, 'art'), 0o555)            # (copied from a read-only place)
            pack.write(d, KAZUMI, {'.nud': b'new'}, 0)
            self.assertEqual(sorted(os.listdir(d)), ['kzm'])        # (the earlier pack is gone whole)
            pack.write(d, KAZUMI, {'.nud': b'newer'}, 0)
            self.assertEqual(sorted(os.listdir(d)), ['kzm'])

    def test_a_leftover_that_cannot_be_deleted_is_said_clearly(self):
        with tempfile.TemporaryDirectory() as d:
            current_pack(d, KAZUMI, 0)
            write(os.path.join(d, '.kzm.old', 'art', 'a.nut'))      # (as another user's, which cannot go)
            real = shutil.rmtree

            def refusing(path, *args, **kwargs):
                if path.endswith('.kzm.old'):
                    raise PermissionError(13, 'Permission denied', path)
                return real(path, *args, **kwargs)

            with mock.patch.object(pack.shutil, 'rmtree', refusing):
                with self.assertRaisesRegex(InputError, 'cannot delete .*\\.kzm\\.old, left beside Kazumi Mishima'):
                    pack.write(d, KAZUMI, {'.nud': b'new'}, 0)
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    pack.remove(os.path.join(d, 'kzm'))             # (the pack goes all the same)
                self.assertIn('cannot delete %s' % os.path.join(d, '.kzm.old'), err.getvalue())
            self.assertEqual(pack.installed(d), [])
            self.assertFalse(os.path.exists(os.path.join(d, '.kzm.old', 'character.ini')))

    def test_killed_between_the_renames_the_earlier_pack_comes_back(self):
        with tempfile.TemporaryDirectory() as d:
            current_pack(d, CHLOE, 1)
            os.makedirs(os.path.join(d, '.kzm.old'))
            write(os.path.join(d, '.kzm.old', 'character.ini'), 'format = 1\ncode = kzm\ndonor = jun\nslot = 0\n')
            write(os.path.join(d, '.kzm.part', 'character.ini'), 'format = 1\ncode = kzm\ndonor = jun\nslot = 0\n')
            write(os.path.join(d, '.elz.part', 'elz.nud'))           # (a new fighter's, stopped: never added)
            self.assertEqual(pack.choose_slot(d, 'elz'), 2)           # (Kazumi's place stays hers)
            self.assertEqual(sorted(os.listdir(d)), ['.elz.part', '.kzm.part', 'dnc', 'kzm'])
            pack.write(d, KAZUMI, {'.nud': b'new'}, 0)
            self.assertEqual(sorted(os.listdir(d)), ['.elz.part', 'dnc', 'kzm'])

    def test_a_half_deleted_leftover_is_not_taken_back(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, '.kzm.old', 'kzm.nud'))           # (no character.ini: deleted first)
            pack.recover(d)
            self.assertEqual(os.listdir(d), ['.kzm.old'])

    def test_a_link_to_a_pack_stays(self):
        with tempfile.TemporaryDirectory() as d:
            elsewhere = os.path.join(d, 'mine')
            old_kazumi(elsewhere)
            packs = os.path.join(d, 'packs')
            os.makedirs(packs)
            os.symlink(os.path.join(elsewhere, 'kzm'), os.path.join(packs, 'kzm'))
            pack.write(packs, KAZUMI, {'.nud': b'new'}, 0)
            self.assertTrue(os.path.islink(os.path.join(packs, 'kzm')))
            self.assertEqual(files_in(os.path.join(elsewhere, 'kzm')), ['character.ini', 'kzm.nud'])
            self.assertEqual(sorted(os.listdir(elsewhere)), ['kzm'])


class Shared(unittest.TestCase):
    def test_made_for_current_packs_only_in_their_places(self):
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)                                           # (place 0, outdated)
            current_pack(d, CHLOE, 1)
            fighters = shared.fighters_of(d, {'kzm': KAZUMI, 'dnc': CHLOE})
            self.assertEqual([(slot, f.CODE) for slot, f, _ in fighters], [(1, 'dnc')])

    def test_none_when_every_pack_is_outdated(self):
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)
            self.assertEqual(shared.fighters_of(d, {'kzm': KAZUMI}), [])

    def build(self, folder, fighters):
        with mock.patch.object(shared, 'fighters_of', return_value=fighters), \
                mock.patch.object(shared.art, 'shared_files', return_value={'keys/00000002.bin': b'new'}), \
                mock.patch.object(shared, 'palettes', return_value={}), \
                mock.patch.object(shared, 'name_calls', return_value={}):
            shared.build(None, None, None, folder, {})

    def test_a_link_put_there_is_replaced_and_what_it_leads_to_kept(self):
        with tempfile.TemporaryDirectory() as d:
            elsewhere = os.path.join(d, 'elsewhere')
            write(os.path.join(elsewhere, 'keys', '00000001.bin'), 'old')
            packs = os.path.join(d, 'packs')
            os.makedirs(packs)
            os.symlink(elsewhere, os.path.join(packs, pack.SHARED))
            for _ in range(3):
                self.build(packs, [(0, KAZUMI, None)])
                self.assertEqual(sorted(os.listdir(packs)), [pack.SHARED])
                self.assertFalse(os.path.islink(os.path.join(packs, pack.SHARED)))
            self.assertEqual(files_in(elsewhere), ['keys/00000001.bin'])
            os.symlink(elsewhere, os.path.join(packs, pack.SHARED + '.old'))     # (left by an older build)
            self.build(packs, [])
            self.assertEqual(os.listdir(packs), [])
            self.assertEqual(files_in(elsewhere), ['keys/00000001.bin'])


class Status(unittest.TestCase):
    def test_outdated_packs(self):
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.dict(os.environ, HOME=d, TTT2_CHARACTERS=os.path.join(d, 'packs')):
            packs = os.path.join(d, 'packs')
            old_kazumi(packs)
            current_pack(packs, CHLOE, 1)
            write(os.path.join(packs, 'elz', 'character.ini'), 'format = %d\ncode = elz\ndonor = jun\nslot = 2\n'
                  % (pack.PACK_FORMAT + 1))
            write(os.path.join(packs, 'ita', 'character.ini'), 'code = ita\ndonor = lar\nslot = 3\n')  # (no format)
            write(os.path.join(packs, 'mut', 'character.ini'), 'format = \u00b9\ncode = mut\ndonor = jun\n'
                  'slot = \u00b2\n')                                  # (no number: no crash)
            fighters = {f['name']: f for f in launcher.status(None, os.path.join(d, 'movesets'))['fighters']}
            state = {name: (f['installed'], f['outdated'], f['newer']) for name, f in fighters.items()}
            self.assertEqual(state['kazumi'], (True, True, False))
            self.assertEqual(state['chloe'], (True, False, False))
            self.assertEqual(state['eliza'], (True, False, True))
            self.assertEqual(state['claudio'], (True, False, False))
            self.assertEqual(state['josie'], (True, True, False))
            self.assertEqual(state['akuma'], (False, False, False))


class Remove(unittest.TestCase):
    def main(self, args):
        out = io.StringIO()
        code = 0
        with mock.patch.object(sys, 'argv', ['import_character.py'] + args), contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(io.StringIO()), mock.patch.object(signals, 'stop_on_signals'):
            try:
                import_character.main()
            except SystemExit as e:
                code = 1 if isinstance(e.code, str) else e.code or 0
        return code, [line for line in out.getvalue().splitlines() if line.startswith('@')]

    def test_only_outdated_packs_left_need_no_tekken7(self):
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)
            current_pack(d, CHLOE, 1)
            write(os.path.join(d, pack.SHARED, 'keys', '00000001.bin'))
            with mock.patch.object(autodump, 'tekken7_library', side_effect=autodump.DumpError('no Steam')):
                code, lines = self.main(['--remove', 'chloe', '--game', 'g', '--out', d, '--progress'])
            self.assertEqual((code, lines), (0, ['@removed chloe', '@step shared', '@done']))
            self.assertEqual(os.listdir(d), ['kzm'])                # (shared/ held Lucky Chloe only)

    def test_an_outdated_pack_is_removed_too(self):
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)
            code, lines = self.main(['--remove', 'kazumi', '--game', 'g', '--out', d, '--progress'])
            self.assertEqual((code, lines), (0, ['@removed kazumi', '@step shared', '@done']))
            self.assertEqual(os.listdir(d), [])

    def test_an_outdated_pack_removed_leaves_shared_as_it_is(self):
        # (shared/ never held it: no Tekken 7 is needed to remake it for the others)
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)
            current_pack(d, CHLOE, 1)
            write(os.path.join(d, pack.SHARED, 'keys', '00000001.bin'), 'chloe')
            with mock.patch.object(autodump, 'tekken7_library', side_effect=autodump.DumpError('no Steam')):
                code, lines = self.main(['--remove', 'kazumi', '--game', 'g', '--out', d, '--progress'])
            self.assertEqual((code, lines), (0, ['@removed kazumi', '@done']))
            self.assertEqual(sorted(os.listdir(d)), ['dnc', pack.SHARED])
            self.assertEqual(files_in(os.path.join(d, pack.SHARED)), ['keys/00000001.bin'])

    def test_a_current_pack_removed_needs_both_games_for_the_others(self):
        with tempfile.TemporaryDirectory() as d:
            current_pack(d, KAZUMI, 0)
            current_pack(d, CHLOE, 1)
            with mock.patch.object(autodump, 'tekken7_library', side_effect=autodump.DumpError('no Steam')):
                code, lines = self.main(['--remove', 'kazumi', '--game', 'g', '--out', d, '--progress'])
            self.assertEqual(code, 1)
            self.assertTrue(lines[0].startswith('@error no Steam (or give --tekken7); the fighters left need both'))
            self.assertEqual(sorted(os.listdir(d)), ['dnc', 'kzm'])  # (nothing was removed)


class NoShared(unittest.TestCase):
    """--no-shared: shared/ is made all the same for a fighter it has no pictures of."""

    def add(self, folder, fighters):
        # (Nothing of the player's: their folders are never named, and Tekken 7 never runs, whatever else is missed.)
        from ttt2char import importer
        made = []
        scratch = os.path.join(folder, '.scratch')
        args = mock.Mock(tekken7='t7', movesets=os.path.join(scratch, 'movesets'), files=os.path.join(scratch, 'files'),
                         game='g', out=folder, no_shared=True)
        with mock.patch.object(import_character, 'needs_packages'), \
                mock.patch.object(autodump, 'dump', side_effect=AssertionError('Tekken 7 would run')), \
                mock.patch.object(autodump, 'unowned', return_value={}), \
                mock.patch('ttt2char.tekken7.pak.Paks'), \
                mock.patch.object(importer, 'tekken7_paks'), \
                mock.patch.object(importer, 'saved_movesets', return_value={26: 'k', 8: 'h', 22: 'c'}), \
                mock.patch.object(autodump, 'unsaved_files', return_value=[]), \
                mock.patch.object(importer, 'run', side_effect=lambda f, *a, **k: current_pack(
                    folder, {'kazumi': KAZUMI, 'chloe': CHLOE}[f], {'kazumi': 0, 'chloe': 1}[f])), \
                mock.patch.object(importer, 'make_shared', side_effect=lambda *a, **k: made.append(1)), \
                contextlib.redirect_stdout(io.StringIO()):
            import_character.add(args, list(fighters), import_character.Progress(False))
        return bool(made)

    def test_a_current_pack_again_leaves_shared(self):
        with tempfile.TemporaryDirectory() as d:
            current_pack(d, KAZUMI, 0)
            self.assertFalse(self.add(d, ['kazumi']))

    def test_an_outdated_pack_replaced_makes_shared(self):
        with tempfile.TemporaryDirectory() as d:
            old_kazumi(d)
            self.assertTrue(self.add(d, ['kazumi']))

    def test_a_new_fighter_makes_shared(self):
        with tempfile.TemporaryDirectory() as d:
            current_pack(d, KAZUMI, 0)
            self.assertTrue(self.add(d, ['kazumi', 'chloe']))


if __name__ == '__main__':
    unittest.main()
