"""Unit tests of what the launcher asks the importer (launcher.py status / owned, import_character.py --progress and
--remove), on made-up folders: a made-up Steam library, packs and movesets; no game files needed. The hidden session
(its end on the launcher's Cancel, a Tekken 7 the player starts) runs with made-up programs: nothing real is started.

    python3 -m unittest discover tools/characters/tests
"""
import contextlib
import importlib.util
import io
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, HERE)

import launcher                                       # noqa: E402
from ttt2char import importer, pack, shared, signals, ttt2   # noqa: E402
from ttt2char.tekken7 import autodump                 # noqa: E402

spec = importlib.util.spec_from_file_location('import_character', os.path.join(HERE, 'import_character.py'))
import_character = importlib.util.module_from_spec(spec)
spec.loader.exec_module(import_character)


def write(path, text=''):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


def made_pack(folder, code, slot, form=pack.PACK_FORMAT):
    """A pack (character.ini and a model file) at place `slot` (None: it names none) of format `form` (None: no
    format line, as the importer's before it wrote one: format 1, as it has no keys/)."""
    write(os.path.join(folder, code, 'character.ini'),
          ('format = %d\n' % form if form is not None else '') + 'code = %s\ndonor = jun\nsex = female\n' % code
          + ('slot = %d\n' % slot if slot is not None else ''))
    write(os.path.join(folder, code, code + '.nud'))


def made_moveset(folder, fighter):
    """A .tkmvst file's header (all saved_movesets reads) of Tekken 7 roster id `fighter`."""
    head = bytearray(0xF0)
    head[:4] = b'TKM2'
    struct.pack_into('<I', head, 0x80, fighter)
    head[0x90:0x98] = b'Tekken 7'
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'T7_%d.tkmvst' % fighter), 'wb') as f:
        f.write(bytes(head))


def environment(home, **more):
    """os.environ for a subprocess with nothing of the player's: HOME and the XDG folders in `home`."""
    env = {k: v for k, v in os.environ.items() if not k.startswith(('TTT2_', 'XDG_'))}
    env.update(HOME=home, XDG_CONFIG_HOME=os.path.join(home, 'config'), XDG_DATA_HOME=os.path.join(home, 'data'),
               XDG_CACHE_HOME=os.path.join(home, 'cache'), **more)
    return env


def run(script, args, env):
    return subprocess.run([sys.executable, os.path.join(HERE, script)] + args, env=env, capture_output=True, text=True,
                          timeout=60)


class Status(unittest.TestCase):
    def test_nothing_installed_and_no_numpy(self):
        with tempfile.TemporaryDirectory() as home:
            write(os.path.join(home, 'fake', 'numpy.py'), 'raise ImportError("no numpy here")\n')
            r = run('launcher.py', ['status', '--game', os.path.join(home, 'none')],
                    environment(home, PYTHONPATH=os.path.join(home, 'fake')))
            self.assertEqual(r.returncode, 0, r.stderr)
            s = json.loads(r.stdout)
            self.assertFalse(s['python']['numpy'])
            self.assertFalse(s['steam']['installed'])
            self.assertFalse(s['tekken7']['found'])
            self.assertIn('Steam is not installed', s['tekken7']['error'])
            self.assertEqual(s['packs_folder'], os.path.join(home, 'data', 'ttt2', 'characters'))
            self.assertIsNone(s['game']['path'])
            fighters = {f['name']: f for f in s['fighters']}
            self.assertEqual(list(fighters), list(importer.CHARACTERS))
            self.assertEqual(fighters['kazumi'], {'name': 'kazumi', 'display': 'Kazumi Mishima', 'dlc': None,
                                                  'installed': False, 'outdated': False, 'newer': False,
                                                  'moveset_saved': False})
            self.assertEqual(fighters['eliza']['dlc'], 592370)

    def test_tekken7_as_the_importer_finds_it(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, HOME=home):
            library = os.path.join(home, 'library')
            write(os.path.join(home, '.local', 'share', 'Steam', 'steamapps', 'libraryfolders.vdf'),
                  '"libraryfolders"\n{\n "1"\n {\n  "path"  "%s"\n }\n}\n' % library)
            write(os.path.join(library, 'steamapps', 'appmanifest_389730.acf'), '"installdir"  "TEKKEN 7"\n')
            game = os.path.join(library, 'steamapps', 'common', 'TEKKEN 7')
            t7 = launcher.tekken7()                         # (listed by Steam, its folder gone)
            self.assertEqual((t7['found'], t7['path']), (False, game))
            self.assertIn('it is not there', t7['error'])
            write(os.path.join(game, 'TekkenGame', 'Content', 'Paks', 'a.pak'))
            t7 = launcher.tekken7()
            self.assertEqual((t7['found'], t7['path']), (True, game))
            self.assertIn('has not run with Proton yet', t7['error'])
            prefix = os.path.join(library, 'steamapps', 'compatdata', '389730')
            proton = os.path.join(home, 'proton')
            write(os.path.join(proton, 'proton'))
            write(os.path.join(prefix, 'config_info'), '9.0\n%s/files/share/fonts/\n' % proton)
            os.makedirs(os.path.join(prefix, 'pfx'))
            self.assertEqual(launcher.tekken7(), {'found': True, 'path': game, 'error': None})

    def test_packs_and_movesets(self):
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.dict(os.environ, HOME=d, TTT2_CHARACTERS=os.path.join(d, 'packs')):
            made_pack(os.path.join(d, 'packs'), 'kzm', 0)
            for fighter in (26, 8):                                     # Kazumi's and Heihachi's movesets
                made_moveset(os.path.join(d, 'movesets'), fighter)
            install = os.path.join(d, 'install')
            write(os.path.join(install, 'game', 'content', 'hdd', 'data000.ofs'))
            s = launcher.status(install, os.path.join(d, 'movesets'))
            fighters = {f['name']: f for f in s['fighters']}
            self.assertEqual((fighters['kazumi']['installed'], fighters['kazumi']['moveset_saved']), (True, True))
            self.assertFalse(fighters['kazumi']['outdated'])
            self.assertEqual((fighters['chloe']['installed'], fighters['chloe']['moveset_saved']), (False, False))
            self.assertTrue(s['movesets']['heihachi'])
            self.assertEqual(s['packs_folder'], os.path.join(d, 'packs'))
            self.assertEqual(s['game'], {'path': os.path.join(install, 'game'), 'error': None})


class Folders(unittest.TestCase):
    def test_packs_folder_as_the_game_finds_it(self):
        with mock.patch.dict(os.environ, HOME='/h', XDG_DATA_HOME='/x', TTT2_CHARACTERS='/t'):
            self.assertEqual(pack.default_folder(), '/t')
            del os.environ['TTT2_CHARACTERS']
            self.assertEqual(pack.default_folder(), '/x/ttt2/characters')
            del os.environ['XDG_DATA_HOME']
            self.assertEqual(pack.default_folder(), '/h/.local/share/ttt2/characters')

    def test_install_folder_or_title_folder(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(ttt2.title_folder(d))
            write(os.path.join(d, 'game', 'content', 'hdd', 'data000.ofs'))
            self.assertEqual(ttt2.title_folder(d), os.path.join(d, 'game'))
            self.assertEqual(importer.ttt2_folder(os.path.join(d, 'game')), os.path.join(d, 'game'))


class Helper(unittest.TestCase):
    def test_prebuilt_helper_needs_no_mingw(self):
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, 't7helper.exe')
            write(exe)
            with mock.patch.dict(os.environ, TTT2_T7HELPER=exe), mock.patch.object(autodump.shutil, 'which',
                                                                                  return_value=None):
                self.assertEqual(autodump.build_helper(os.path.join(d, 'cache')), exe)
            with mock.patch.dict(os.environ, TTT2_T7HELPER=os.path.join(d, 'gone.exe')), \
                    mock.patch.object(autodump.shutil, 'which', return_value=None):
                with self.assertRaisesRegex(autodump.DumpError, 'MinGW'):
                    autodump.build_helper(os.path.join(d, 'cache'))

    def test_one_session_at_a_time(self):
        with tempfile.TemporaryDirectory() as d:
            first = autodump._lock(os.path.join(d, 'session.lock'))
            with self.assertRaisesRegex(autodump.DumpError, 'busy'):
                autodump._lock(os.path.join(d, 'session.lock'), timeout=0)
            first.close()
            autodump._lock(os.path.join(d, 'session.lock'), timeout=0).close()


class Owned(unittest.TestCase):
    def test_steam_not_running(self):
        with mock.patch.object(autodump, 'steam_running', return_value=False), \
                mock.patch.object(autodump, 'owned_dlc') as asked:
            self.assertIn('Steam is not running', launcher.owned()['error'])
            asked.assert_not_called()

    def test_dlc_fighters_only(self):
        answer = {app: app == autodump.DLC[50] for app in autodump.DLC.values()}          # Negan's DLC only
        with mock.patch.object(autodump, 'steam_running', return_value=True), \
                mock.patch.object(autodump, 'find_tekken7'), mock.patch.object(autodump, 'build_helper'), \
                mock.patch.object(autodump, 'owned_dlc', return_value=answer):
            owned = launcher.owned()['owned']
        self.assertTrue(owned['negan'])
        self.assertFalse(owned['eliza'])
        self.assertNotIn('kazumi', owned)
        self.assertEqual(len(owned), sum(1 for n in importer.CHARACTERS
                                         if importer.definition(n).TEKKEN7_ID in autodump.DLC))


class Progress(unittest.TestCase):
    def main(self, args):
        """import_character.main(args) -> (exit code, its @ lines)."""
        out = io.StringIO()
        code = 0
        with mock.patch.object(sys, 'argv', ['import_character.py'] + args), contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(io.StringIO()), mock.patch.object(signals, 'stop_on_signals'):
            try:
                import_character.main()
            except SystemExit as e:
                code = 1 if isinstance(e.code, str) else e.code or 0
        return code, [line for line in out.getvalue().splitlines() if line.startswith('@')]

    def test_import_lines(self):
        def imported(name, tekken7, movesets, game, out, make_shared, files):
            self.assertFalse(make_shared)                    # (made once, after the last)
            made_pack(out, importer.definition(name).CODE, 3)
            return os.path.join(out, importer.definition(name).CODE)

        with tempfile.TemporaryDirectory() as d, \
                mock.patch.object(autodump, 'tekken7_library', return_value=('s', 'l', 't7')), \
                mock.patch('ttt2char.tekken7.pak.Paks'), mock.patch.object(importer, 'tekken7_paks'), \
                mock.patch.object(importer, 'saved_movesets', return_value={26: 'k', 22: 'c', 8: 'h'}), \
                mock.patch.object(autodump, 'unsaved_files', return_value=[]), \
                mock.patch.object(importer, 'run', side_effect=imported), \
                mock.patch.object(importer, 'make_shared') as shared:
            code, lines = self.main(['kazumi', 'chloe', '--game', 'g', '--out', d, '--progress'])
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['@step fighter kazumi 1 2', '@added kazumi', '@step fighter chloe 2 2', '@added chloe',
                                 '@step shared', '@done'])
        shared.assert_called_once()

    def test_unowned_dlc_skipped(self):
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.object(autodump, 'tekken7_library', return_value=('s', 'l', 't7')), \
                mock.patch.object(autodump, 'unowned', return_value={36: 'its DLC is not owned'}):
            code, lines = self.main(['eliza', '--game', 'g', '--out', d, '--progress'])
        self.assertEqual(code, 1)
        self.assertEqual(lines[1:], ['@skipped eliza its DLC is not owned',
                                     '@error none of the fighters asked for is owned'])
        self.assertTrue(lines[0].startswith('@step dump '))

    def test_remove_the_last(self):
        with tempfile.TemporaryDirectory() as d:
            made_pack(d, 'kzm', 0)
            write(os.path.join(d, pack.SHARED, 'keys', '00000001.bin'))
            with mock.patch.object(autodump, 'tekken7_library', side_effect=autodump.DumpError('no Steam')):
                code, lines = self.main(['--remove', 'kazumi', 'chloe', '--game', 'g', '--out', d, '--progress'])
            self.assertEqual(code, 0)
            self.assertEqual(lines, ['@skipped chloe it is not added', '@removed kazumi', '@step shared', '@done'])
            self.assertEqual(os.listdir(d), [])                 # (no Tekken 7 needed: nothing is left to share)

    def test_remove_keeps_the_others_places(self):
        with tempfile.TemporaryDirectory() as d:
            for code, slot in (('kzm', 0), ('dnc', 1), ('elz', 2)):
                made_pack(d, code, slot)
            with mock.patch.object(autodump, 'tekken7_library', return_value=('s', 'l', 't7')), \
                    mock.patch.object(importer, 'tekken7_paks'), mock.patch.object(importer, 'ttt2_folder'), \
                    mock.patch.object(importer, 'make_shared') as shared:
                code, lines = self.main(['--remove', 'chloe', '--game', 'g', '--out', d, '--progress'])
            self.assertEqual((code, lines), (0, ['@removed chloe', '@step shared', '@done']))
            shared.assert_called_once()
            self.assertEqual(pack.places_of(pack.installed(d)), {'kzm': 0, 'elz': 2})
            self.assertEqual(pack.choose_slot(d, 'mrx'), 1)          # the next fighter takes the free place

    def test_remove_needs_tekken7_for_the_others(self):
        with tempfile.TemporaryDirectory() as d:
            made_pack(d, 'kzm', 0)
            made_pack(d, 'dnc', 1)
            with mock.patch.object(autodump, 'tekken7_library', side_effect=autodump.DumpError('Steam is not installed')):
                code, lines = self.main(['--remove', 'chloe', '--game', 'g', '--out', d, '--progress'])
            self.assertEqual(code, 1)
            self.assertTrue(lines[-1].startswith('@error Steam is not installed'), lines)
            self.assertIn('nothing was removed', lines[-1])
            self.assertEqual(sorted(os.listdir(d)), ['dnc', 'kzm'])  # (nothing was removed)



# ---------------------------------------------------------------- the hidden session, with made-up programs

FAKE_GAMESCOPE = """#!/bin/sh
# gamescope: runs what follows --, as the session's first program
while [ $# -gt 0 ] && [ "$1" != -- ]; do shift; done
shift
exec "$@"
"""
FAKE_PACTL = """#!/bin/sh
echo "$*" >> "$FAKE_LOGS/pactl"
[ "$1" = load-module ] && echo 42
exit 0
"""
FAKE_PROTON = """#!/bin/sh
# Proton: "run <program> <arguments>"; Tekken 7 sleeps, the helper is fake_helper.py
case "$2" in
  *t7helper.exe) shift 2; exec "$FAKE_PYTHON" "$FAKE_HELPER" "$@" ;;
  *) exec "$FAKE_BIN/TekkenGame-Win64-Shipping" 600 ;;
esac
"""
FAKE_WINESERVER = """#!/bin/sh
echo "$*" >> "$FAKE_LOGS/wineserver"
"""
FAKE_HELPER = """import os, socket, sys, time
_, port, secret = sys.argv[1:4]                 # serve PORT SECRET
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', int(port)))
server.listen(1)
f = server.accept()[0].makefile('rwb', buffering=0)
if f.readline().strip().decode() != secret:
    sys.exit(1)
for line in f:
    command = line.decode().strip()
    with open(os.path.join(os.environ['FAKE_LOGS'], 'helper'), 'a') as log:
        log.write(command.split()[0] + '\\n')
    if command.startswith('dlc'):
        while os.environ.get('FAKE_GO') and not os.path.exists(os.environ['FAKE_GO']):
            time.sleep(0.1)
        f.write(b'end\\n')
    elif command == 'attach':
        f.write(b'ok\\n')
    elif command == 'ready':
        f.write(b'0\\n')                         # (never ready: the dump waits there)
    elif command == 'quit':
        break
    else:
        f.write(b'error unknown\\n')
"""


def empty_pak(path):
    """A pak file with no entries (what Paks reads: the index and the tail)."""
    mount = b'../../../\0'
    index = struct.pack('<i', len(mount)) + mount + struct.pack('<i', 0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(index + struct.pack('<IIqq', 0x5A6F12E1, 3, 0, len(index)) + bytes(20))


def tagged(home):
    """{pid: comm} of the processes a session started for this test (its HOME and SESSION_TAG in their environment)."""
    out = {}
    for pid, (comm, env) in autodump._processes().items():
        if autodump.SESSION_TAG in env and env.get('HOME') == home:
            out[pid] = comm
    return out


def alive(pid):
    return (autodump._stat(pid) or ['Z'])[0] != 'Z'


def helper_in(processes):
    return any('fake_helper.py' in ' '.join(autodump._cmdline(pid)) for pid in processes)


class Sessions(unittest.TestCase):
    """The hidden session's start and end through the importer's own programs, with gamescope, pactl, Proton, Wine's
    server, Tekken 7 and the helper made up (shell scripts, `sleep`, a helper speaking its protocol); Steam is a
    process named steam. SIGTERM is the launcher's Cancel. (A real Tekken 7 running meanwhile counts as the
    player's: these then fail.)"""

    def setUp(self):
        self.home = os.path.realpath(tempfile.mkdtemp(prefix='ttt2-sessions-'))
        self.addCleanup(subprocess.run, ['rm', '-rf', self.home])
        h = self.home
        self.bin, self.logs = os.path.join(h, 'bin'), os.path.join(h, 'logs')
        for d in (self.bin, self.logs, os.path.join(h, 'run')):
            os.makedirs(d)
        sleep = shutil.which('sleep')
        for name in ('TekkenGame-Win64-Shipping', 'steam'):
            shutil.copy(sleep, os.path.join(self.bin, name))
        proton = os.path.join(h, 'proton')
        for path, text in ((os.path.join(self.bin, 'gamescope'), FAKE_GAMESCOPE),
                           (os.path.join(self.bin, 'pactl'), FAKE_PACTL),
                           (os.path.join(proton, 'proton'), FAKE_PROTON),
                           (os.path.join(proton, 'files', 'bin', 'wineserver'), FAKE_WINESERVER),
                           (os.path.join(h, 'fake_helper.py'), FAKE_HELPER), (os.path.join(h, 't7helper.exe'), '')):
            write(path, text)
            os.chmod(path, 0o755)
        steam = os.path.join(h, '.local', 'share', 'Steam')
        write(os.path.join(steam, 'steamapps', 'appmanifest_389730.acf'), '"installdir"  "TEKKEN 7"\n')
        self.game = os.path.join(steam, 'steamapps', 'common', 'TEKKEN 7')
        empty_pak(os.path.join(self.game, 'TekkenGame', 'Content', 'Paks', 'pakchunk0-WindowsNoEditor.pak'))
        self.prefix = os.path.join(steam, 'steamapps', 'compatdata', '389730')
        os.makedirs(os.path.join(self.prefix, 'pfx', 'drive_c'))
        write(os.path.join(self.prefix, 'config_info'), '9.0\n%s/files/share/fonts/\n' % proton)
        self.env = environment(h, PATH=self.bin + os.pathsep + os.environ.get('PATH', ''),
                               XDG_RUNTIME_DIR=os.path.join(h, 'run'), TTT2_T7HELPER=os.path.join(h, 't7helper.exe'),
                               FAKE_BIN=self.bin, FAKE_LOGS=self.logs, FAKE_PYTHON=sys.executable,
                               FAKE_HELPER=os.path.join(h, 'fake_helper.py'))
        self.steam = subprocess.Popen([os.path.join(self.bin, 'steam'), '600'])       # (Steam is running)
        self.addCleanup(self.stop, self.steam)
        self.addCleanup(self.end_all)

    @staticmethod
    def stop(process):
        process.kill()
        process.wait()

    def end_all(self):
        """What a failed test left running."""
        autodump._kill(tagged(self.home))

    def log(self, name):
        try:
            with open(os.path.join(self.logs, name)) as f:
                return f.read().split('\n')
        except FileNotFoundError:
            return []

    def wait_for(self, what, condition, timeout=30):
        end = time.time() + timeout
        while not condition():
            if time.time() > end:
                self.fail('%s did not happen within %d s' % (what, timeout))
            time.sleep(0.05)

    def start(self, script, args, **env):
        return subprocess.Popen([sys.executable, os.path.join(HERE, script)] + args, env=dict(self.env, **env),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def finish(self, process, timeout=30):
        out, err = process.communicate(timeout=timeout)
        return process.returncode, out, err

    def player(self):
        """The player's Tekken 7, as Steam starts it: the app id and the prefix in its environment, no session's."""
        env = environment(self.home, SteamAppId='389730', STEAM_COMPAT_DATA_PATH=self.prefix)
        game = subprocess.Popen([os.path.join(self.bin, 'TekkenGame-Win64-Shipping'), '600'], env=env)
        self.addCleanup(self.stop, game)
        return game

    def test_sigterm_during_the_dump_ends_the_hidden_session(self):
        importing = self.start('import_character.py', [
            'kazumi', '--game', os.path.join(self.home, 'game'), '--out', os.path.join(self.home, 'packs'),
            '--movesets', os.path.join(self.home, 'movesets'), '--files', os.path.join(self.home, 'files'),
            '--progress'])
        self.wait_for('the dump waiting for Tekken 7', lambda: 'ready' in self.log('helper'))
        session = tagged(self.home)
        self.assertIn('TekkenGame-Win6', session.values())
        importing.send_signal(signal.SIGTERM)
        code, out, err = self.finish(importing)
        self.assertEqual(code, 128 + signal.SIGTERM, err)
        self.assertIn('@step dump Saving what Tekken 7 has only while it runs', out)
        self.assertNotIn('@error', out)
        self.assertEqual(tagged(self.home), {})
        self.assertFalse(any(alive(pid) for pid in session))
        self.assertIn('-k', self.log('wineserver'))                  # (nothing of the player's in the prefix)
        self.assertIn('unload-module 42', self.log('pactl'))         # (the silent output goes too)
        self.assertFalse(os.path.exists(os.path.join(self.home, 'packs', 'kzm')))

    def test_sigterm_during_the_dlc_check(self):
        go = os.path.join(self.home, 'go')
        asking = self.start('launcher.py', ['owned'], FAKE_GO=go)
        self.wait_for('the DLC question', lambda: 'dlc' in self.log('helper'))
        session = tagged(self.home)
        self.assertTrue(helper_in(session))
        asking.send_signal(signal.SIGTERM)
        code, out, err = self.finish(asking)
        self.assertEqual((code, out), (128 + signal.SIGTERM, ''), err)
        self.assertEqual(tagged(self.home), {})
        self.assertFalse(any(alive(pid) for pid in session))

    def test_the_players_tekken7_is_left_alone(self):
        game = self.player()
        self.wait_for('the player\'s Tekken 7', lambda: autodump._players(self.prefix, autodump._processes()))
        code, out, err = self.finish(self.start('launcher.py', ['owned']))
        self.assertEqual((code, json.loads(out)), (0, {'error': 'Tekken 7 is running: close it first'}), err)
        self.assertIsNone(game.poll())
        self.assertEqual((self.log('helper'), self.log('wineserver')), ([], []))       # (nothing was started)

    def test_a_tekken7_the_player_starts_meanwhile_goes_on(self):
        go = os.path.join(self.home, 'go')
        asking = self.start('launcher.py', ['owned'], FAKE_GO=go)
        self.wait_for('the DLC question', lambda: 'dlc' in self.log('helper'))
        session = tagged(self.home)
        game = self.player()                            # (Steam starts it while the helper runs)
        write(go)
        code, out, err = self.finish(asking)
        self.assertEqual(code, 0, err)
        self.assertNotIn(False, [v is False for v in json.loads(out)['owned'].values()])
        self.assertIsNone(game.poll())                  # theirs goes on ...
        self.assertNotIn('-k', self.log('wineserver'))  # ... Wine's server of the prefix is not stopped ...
        self.assertEqual(tagged(self.home), {})         # ... and the session's own processes are gone
        self.assertFalse(any(alive(pid) for pid in session))

    def test_a_killed_importers_session_is_ended_by_the_next(self):
        asking = self.start('launcher.py', ['owned'], FAKE_GO=os.path.join(self.home, 'never'))
        self.wait_for('the DLC question', lambda: 'dlc' in self.log('helper'))
        asking.kill()                                   # (SIGKILL: no cleanup)
        self.finish(asking)
        left = tagged(self.home)
        self.assertTrue(helper_in(left))
        code, out, err = self.finish(self.start('launcher.py', ['owned']))
        self.assertEqual(code, 0, err)
        self.assertIn('owned', json.loads(out))
        self.assertFalse(any(alive(pid) for pid in left))
        self.assertEqual(tagged(self.home), {})


class Players(unittest.TestCase):
    """Which processes are the player's Tekken 7 (autodump._players), from made-up process tables."""

    PREFIX = '/lib/steamapps/compatdata/389730'

    def players(self, *processes):
        table = {100000 + k: (comm, env) for k, (comm, env) in enumerate(processes)}
        return [table[pid][0] for pid in autodump._players(self.PREFIX, table)]

    def test_steams_start_of_the_game(self):
        self.assertEqual(self.players(('reaper', {'SteamAppId': '389730'}),
                                      ('python3', {'STEAM_COMPAT_DATA_PATH': self.PREFIX + '/'}),
                                      ('steam.exe', {'WINEPREFIX': self.PREFIX + '/pfx/'}),
                                      ('TekkenGame-Win6', {})),
                         ['reaper', 'python3', 'steam.exe', 'TekkenGame-Win6'])

    def test_not_the_sessions_nor_other_games(self):
        session = {autodump.SESSION_TAG: '1.2.3', 'SteamAppId': '389730', 'STEAM_COMPAT_DATA_PATH': self.PREFIX}
        self.assertEqual(self.players(('TekkenGame-Win6', session), ('wineserver', session),
                                      ('game.exe', {'SteamAppId': '570', 'WINEPREFIX': '/lib/compatdata/570/pfx'}),
                                      ('steam', {})), [])

    def test_a_session_of_a_killed_program(self):
        self.assertFalse(autodump._owner_alive('999999999.1.x'))
        self.assertTrue(autodump._owner_alive(autodump._new_tag()))
        self.assertFalse(autodump._owner_alive('%d.0.x' % os.getpid()))          # (its pid, but another start)


class Signals(unittest.TestCase):
    def setUp(self):
        old = {s: signal.getsignal(s) for s in signals.SIGNALS}
        self.addCleanup(lambda: [signal.signal(s, h) for s, h in old.items()])
        self.addCleanup(signals._state.update, dict(signals._state))
        signals.stop_on_signals()

    @staticmethod
    def wait():
        for _ in range(200):                            # (the handler runs between two steps of Python)
            time.sleep(0.005)

    def test_sigterm_ends_by_an_exception_once(self):
        cleaned = []
        with self.assertRaises(SystemExit) as stop:
            try:
                os.kill(os.getpid(), signal.SIGTERM)
                self.wait()
                self.fail('no exception')
            finally:
                os.kill(os.getpid(), signal.SIGTERM)    # (a second one, during the cleanup: ignored)
                self.wait()
                cleaned.append(True)
        self.assertEqual((stop.exception.code, cleaned), (128 + signal.SIGTERM, [True]))
        self.assertEqual(signals.stopping(), signal.SIGTERM)

    def test_deferred_comes_at_the_end_of_the_block(self):
        done = []
        with self.assertRaises(SystemExit):
            with signals.deferred():
                os.kill(os.getpid(), signal.SIGHUP)
                self.wait()
                done.append(True)
        self.assertEqual(done, [True])

    def test_sigterm_in_the_dump_closes_the_session(self):
        closed = []

        class Session:
            def __init__(self, *args):
                pass

            def wait_game(self):
                os.kill(os.getpid(), signal.SIGTERM)
                Signals.wait()

            def close(self):
                closed.append(True)

        with mock.patch.object(autodump, 'steam_running', return_value=True), \
                mock.patch.object(autodump, 'find_tekken7'), mock.patch.object(autodump, 'tekken7_running',
                                                                               return_value=False), \
                mock.patch.object(autodump, 'build_helper'), mock.patch.object(autodump, 'owned_dlc', return_value={}), \
                mock.patch.object(autodump, 'Session', Session), tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                autodump.dump({26: ('knm', 'Kazumi')}, d, log=lambda s: None)
        self.assertEqual(closed, [True])


# ---------------------------------------------------------------- where things are

class SandboxedSteam(unittest.TestCase):
    def tekken7(self, home, steam):
        write(os.path.join(home, steam, 'steamapps', 'appmanifest_389730.acf'), '"installdir"  "TEKKEN 7"\n')
        game = os.path.join(home, steam, 'steamapps', 'common', 'TEKKEN 7')
        write(os.path.join(game, 'TekkenGame', 'Content', 'Paks', 'a.pak'))
        return game

    def test_flatpak(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, HOME=home), \
                mock.patch.object(autodump, 'steam_running', return_value=True):
            game = self.tekken7(home, '.var/app/com.valvesoftware.Steam/.local/share/Steam')
            s = launcher.status()
            self.assertEqual(s['steam'], {'installed': True, 'running': True, 'sandbox': 'flatpak',
                                          'path': os.path.join(home, '.var/app/com.valvesoftware.Steam/.local/share/'
                                                                     'Steam')})
            self.assertEqual((s['tekken7']['found'], s['tekken7']['path']), (True, game))
            self.assertIn('Flatpak, which is not supported yet', s['tekken7']['error'])
            self.assertEqual(launcher.owned(), {'error': s['tekken7']['error']})
            self.assertEqual(autodump.tekken7_library()[2], game)     # (its files can be read)

    def test_flatpak_library_in_its_home(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, HOME=home):
            sandbox = os.path.join(home, '.var', 'app', 'com.valvesoftware.Steam')
            write(os.path.join(sandbox, '.local', 'share', 'Steam', 'steamapps', 'libraryfolders.vdf'),
                  '"libraryfolders"\n{\n "1"\n {\n  "path"  "%s"\n }\n}\n' % os.path.join(home, 'Games'))
            game = self.tekken7(sandbox, 'Games')                    # (the sandbox's ~/Games)
            self.assertEqual(autodump.tekken7_library()[2], game)

    def test_snap(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, HOME=home):
            game = self.tekken7(home, 'snap/steam/common/.local/share/Steam')
            t7 = launcher.tekken7()
            self.assertEqual((t7['found'], t7['path']), (True, game))
            self.assertIn('Snap, which is not supported yet', t7['error'])
            self.assertEqual(launcher.steam()['sandbox'], 'snap')

    def test_the_distributions_steam_first(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, HOME=home):
            self.tekken7(home, '.var/app/com.valvesoftware.Steam/.local/share/Steam')
            os.makedirs(os.path.join(home, '.local', 'share', 'Steam'))
            game = self.tekken7(home, '.local/share/Steam')
            self.assertEqual(launcher.tekken7()['path'], game)
            self.assertIsNone(launcher.steam()['sandbox'])


class Places(unittest.TestCase):
    def test_cache_as_the_game_and_the_launcher_find_it(self):
        code = 'import sys; sys.path.insert(0, %r); from ttt2char.tekken7 import autodump; print(autodump.CACHE)' % HERE
        for xdg, cache in (('/x/cache', '/x/cache/ttt2/tekken7'), ('', '/h/.cache/ttt2/tekken7')):
            env = dict(os.environ, HOME='/h', XDG_CACHE_HOME=xdg)
            out = subprocess.run([sys.executable, '-c', code], env=env, capture_output=True, text=True).stdout
            self.assertEqual(out.strip(), cache)

    def test_one_lock_per_prefix_whatever_the_cache(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(os.environ, XDG_RUNTIME_DIR=d):
            path = autodump._lock_path('/lib/steamapps/compatdata/389730')
            self.assertEqual(os.path.dirname(path), d)
            self.assertEqual(path, autodump._lock_path('/lib/steamapps/compatdata/389730/'))
            self.assertNotEqual(path, autodump._lock_path('/other/steamapps/compatdata/389730'))

    def test_brackets_in_a_library_path(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, HOME=home):
            library = os.path.join(home, 'Games [SSD]', 'SteamLibrary')
            write(os.path.join(home, '.local', 'share', 'Steam', 'steamapps', 'libraryfolders.vdf'),
                  '"libraryfolders"\n{\n "1"\n {\n  "path"  "%s"\n }\n}\n' % library)
            write(os.path.join(library, 'steamapps', 'appmanifest_389730.acf'), '"installdir"  "TEKKEN 7"\n')
            game = os.path.join(library, 'steamapps', 'common', 'TEKKEN 7')
            write(os.path.join(game, 'TekkenGame', 'Content', 'Paks', 'a.pak'))
            proton = os.path.join(home, 'proton [GE]')
            write(os.path.join(proton, 'proton'))
            write(os.path.join(library, 'steamapps', 'compatdata', '389730', 'config_info'),
                  '9.0\n%s/files/share/fonts/\n' % proton)
            os.makedirs(os.path.join(library, 'steamapps', 'compatdata', '389730', 'pfx'))
            self.assertEqual(launcher.tekken7(), {'found': True, 'path': game, 'error': None})
            self.assertEqual(importer.tekken7_paks(game), os.path.join(game, 'TekkenGame', 'Content', 'Paks'))
            made_moveset(os.path.join(home, 'movesets [1]'), 26)
            self.assertEqual(list(importer.saved_movesets(os.path.join(home, 'movesets [1]'))), [26])

    def test_no_developer_command_in_what_players_see(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, 'file'))
            for call in (lambda: importer._archives(d), lambda: importer._rpx(d), lambda: importer.ttt2_folder(d),
                         lambda: importer.ttt2_folder(os.path.join(d, 'file'))):
                with self.assertRaises(importer.InputError) as error:
                    call()
                # (the folder's own path may hold 'ttt2', as a checkout or TMPDIR of that name does)
                self.assertNotIn('ttt2', str(error.exception).replace(d, ''))


# ---------------------------------------------------------------- removing, and a pack written halfway

class Remove(unittest.TestCase):
    def test_only_tekken7s_files_are_needed(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, HOME=home):
            write(os.path.join(home, '.local', 'share', 'Steam', 'steamapps', 'appmanifest_389730.acf'), '')
            game = os.path.join(home, '.local', 'share', 'Steam', 'steamapps', 'common', 'TEKKEN 7')
            write(os.path.join(game, 'TekkenGame', 'Content', 'Paks', 'a.pak'))
            packs = os.path.join(home, 'packs')
            made_pack(packs, 'kzm', 0)
            made_pack(packs, 'dnc', 1)
            with self.assertRaisesRegex(autodump.DumpError, 'has not run with Proton yet'):
                autodump.find_tekken7()                     # (no Proton prefix: Tekken 7 never ran)
            with mock.patch.object(importer, 'ttt2_folder'), mock.patch.object(importer, 'make_shared') as shared:
                code, lines = Progress.main(self, ['--remove', 'chloe', '--game', 'g', '--out', packs, '--progress'])
            self.assertEqual((code, lines), (0, ['@removed chloe', '@step shared', '@done']))
            self.assertEqual(shared.call_args[0][0], game)
            self.assertEqual(os.listdir(packs), ['kzm'])

    def test_stopped_before_what_they_share_is_made_again(self):
        with tempfile.TemporaryDirectory() as d:
            for code, slot in (('kzm', 0), ('dnc', 1), ('elz', 2)):
                made_pack(d, code, slot)
            write(os.path.join(d, pack.SHARED, 'keys', '00000001.bin'))
            with mock.patch.object(autodump, 'tekken7_library', return_value=('s', 'l', 't7')), \
                    mock.patch.object(importer, 'tekken7_paks'), mock.patch.object(importer, 'ttt2_folder'), \
                    mock.patch.object(importer, 'make_shared', side_effect=SystemExit(143)):     # (Cancel)
                code, lines = Progress.main(self, ['--remove', 'chloe', '--game', 'g', '--out', d, '--progress'])
            self.assertEqual((code, lines), (143, ['@removed chloe', '@step shared']))
            self.assertEqual(sorted(os.listdir(d)), ['elz', 'kzm'])          # (shared/ would show Lucky Chloe)

    def test_no_numpy_needed_to_remove_the_last(self):
        with tempfile.TemporaryDirectory() as home:
            write(os.path.join(home, 'fake', 'numpy.py'), 'raise ImportError("no numpy here")\n')
            packs = os.path.join(home, 'packs')
            made_pack(packs, 'kzm', 0)
            made_pack(packs, 'dnc', 1)
            env = environment(home, PYTHONPATH=os.path.join(home, 'fake'))
            r = run('import_character.py', ['--remove', 'chloe', '--game', 'g', '--out', packs, '--progress'], env)
            self.assertEqual(r.returncode, 1)
            self.assertIn('@error needs the Python packages numpy', r.stdout)
            self.assertEqual(sorted(os.listdir(packs)), ['dnc', 'kzm'])        # (nothing was removed)
            r = run('import_character.py', ['--remove', 'chloe', 'kazumi', '--game', 'g', '--out', packs,
                                             '--progress'], env)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual([line for line in r.stdout.splitlines() if line.startswith('@')],
                             ['@removed chloe', '@removed kazumi', '@step shared', '@done'])
            self.assertEqual(os.listdir(packs), [])


class PackWrite(unittest.TestCase):
    D = mock.Mock(CODE='kzm', NAME='Kazumi Mishima', DONOR='jun', SEX='female')
    FILES = {'.nud': b'nud', '.nut': b'nut', 'art/a.bin': b'art'}

    def write_stopped(self, folder, at):
        """pack.write, stopped (SystemExit, as a signal) writing the file number `at`."""
        real, count = open, [0]

        def stopping(path, *args, **kwargs):
            if '%s.kzm.part%s' % (os.sep, os.sep) in path:          # (the pack being made)
                count[0] += 1
                if count[0] == at:
                    raise SystemExit(143)
            return real(path, *args, **kwargs)

        with mock.patch('builtins.open', stopping), self.assertRaises(SystemExit):
            pack.write(folder, self.D, self.FILES, 0)

    def test_a_new_pack_stopped_is_not_there(self):
        with tempfile.TemporaryDirectory() as d:
            self.write_stopped(d, 3)
            self.assertEqual(os.listdir(d), [])
            self.assertEqual(pack.installed(d), [])

    def test_a_pack_written_again_and_stopped_stays_as_it_was(self):
        with tempfile.TemporaryDirectory() as d:
            target = pack.write(d, self.D, {'.nud': b'old', '.mop': b'mop'}, 4)
            self.write_stopped(d, 4)                        # (character.ini: the last)
            self.assertEqual(sorted(os.listdir(target)), ['character.ini', 'kzm.mop', 'kzm.nud'])
            with open(os.path.join(target, 'kzm.nud'), 'rb') as f:
                self.assertEqual(f.read(), b'old')
            self.assertEqual([p['slot'] for p in pack.installed(d)], [4])

    def test_character_ini_is_written_last_and_the_pack_put_in_place_whole(self):
        with tempfile.TemporaryDirectory() as d:
            real_open, real_replace, opened, placed = open, os.replace, [], []

            def recording(path, *args, **kwargs):
                opened.append(os.path.basename(path))
                return real_open(path, *args, **kwargs)

            with mock.patch('builtins.open', recording), \
                    mock.patch.object(pack.os, 'replace', lambda a, b: placed.append((a, b)) or real_replace(a, b)):
                pack.write(d, self.D, self.FILES, 0)
            self.assertEqual(opened[-1], 'character.ini')
            self.assertEqual(sorted(opened), ['a.bin', 'character.ini', 'kzm.nud', 'kzm.nut'])
            self.assertEqual(placed, [(os.path.join(d, '.kzm.part'), os.path.join(d, 'kzm'))])     # (one rename)
            self.assertEqual(os.listdir(d), ['kzm'])

    def test_a_signal_while_the_files_are_put_in_place_waits(self):
        old = signal.getsignal(signal.SIGTERM)
        self.addCleanup(signal.signal, signal.SIGTERM, old)
        self.addCleanup(signals._state.update, dict(signals._state))
        signals.stop_on_signals()
        with tempfile.TemporaryDirectory() as d:
            real = os.replace

            pack.write(d, self.D, {'.nud': b'old', 'keys/00000001.add': b'old'}, 0)

            def replace(a, b):
                if b.endswith('.kzm.old'):                 # (the earlier pack goes: a signal before the new comes)
                    os.kill(os.getpid(), signal.SIGTERM)
                    Signals.wait()
                real(a, b)

            with mock.patch.object(pack.os, 'replace', replace), self.assertRaises(SystemExit):
                pack.write(d, self.D, self.FILES, 0)
            self.assertEqual([p['code'] for p in pack.installed(d)], ['kzm'])     # (whole: the signal came after)
            self.assertEqual(sorted(os.listdir(os.path.join(d, 'kzm'))), ['art', 'character.ini', 'kzm.nud', 'kzm.nut'])
            self.assertEqual(os.listdir(d), ['kzm'])



class Shared(unittest.TestCase):
    """shared/ made again: the new one replaces the old one whole, a signal or not."""

    def build(self, folder):
        with mock.patch.object(shared, 'fighters_of', return_value=[(0, None, None)]), \
                mock.patch.object(shared.art, 'shared_files', return_value={'keys/00000002.bin': b'new'}), \
                mock.patch.object(shared, 'palettes', return_value={}), \
                mock.patch.object(shared, 'name_calls', return_value={}):
            shared.build(None, None, None, folder, {})

    def made_before(self, folder):
        write(os.path.join(folder, 'shared', 'keys', '00000001.bin'), 'old')
        write(os.path.join(folder, 'shared.part', 'keys', '00000003.bin'))         # (a killed run's)

    def test_made_again(self):
        with tempfile.TemporaryDirectory() as d:
            self.made_before(d)
            self.build(d)
            self.assertEqual(os.listdir(d), ['shared'])
            self.assertEqual(os.listdir(os.path.join(d, 'shared', 'keys')), ['00000002.bin'])

    def test_a_signal_while_it_is_put_in_place_waits(self):
        old = signal.getsignal(signal.SIGTERM)
        self.addCleanup(signal.signal, signal.SIGTERM, old)
        self.addCleanup(signals._state.update, dict(signals._state))
        signals.stop_on_signals()
        with tempfile.TemporaryDirectory() as d:
            self.made_before(d)
            real = os.replace

            def replace(a, b):
                if b.endswith('shared.old'):
                    os.kill(os.getpid(), signal.SIGTERM)
                    Signals.wait()
                real(a, b)

            with mock.patch.object(shared.os, 'replace', replace), self.assertRaises(SystemExit):
                self.build(d)
            self.assertEqual(os.listdir(d), ['shared'])
            self.assertEqual(os.listdir(os.path.join(d, 'shared', 'keys')), ['00000002.bin'])

    def test_stopped_while_it_is_written(self):
        # (the old shared/ stays, and no half-written shared.part is left for the launcher's uninstall to miss)
        old = {s: signal.getsignal(s) for s in signals.SIGNALS}
        self.addCleanup(lambda: [signal.signal(s, h) for s, h in old.items()])
        self.addCleanup(signals._state.update, dict(signals._state))
        signals.stop_on_signals()
        with tempfile.TemporaryDirectory() as d:
            self.made_before(d)
            real = os.makedirs

            def makedirs(path, exist_ok=False):
                real(path, exist_ok=exist_ok)
                os.kill(os.getpid(), signal.SIGTERM)
                Signals.wait()

            with mock.patch.object(shared.os, 'makedirs', makedirs), self.assertRaises(SystemExit):
                self.build(d)
            self.assertEqual(os.listdir(d), ['shared'])
            self.assertEqual(os.listdir(os.path.join(d, 'shared', 'keys')), ['00000001.bin'])


if __name__ == '__main__':
    unittest.main()
