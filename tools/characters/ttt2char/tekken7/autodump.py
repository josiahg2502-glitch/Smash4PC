"""Tekken 7 movesets dumped in the background: the importer's .tkmvst files without the player doing anything.

Tekken 7's data files are encrypted; a fighter's moveset is readable only once the game has loaded it, so the game
has to run, and it runs where nobody sees or hears it, and with nothing pressed:
* Proton runs it inside gamescope's headless backend (no window, no output) with its sound sent to a silent sink of
  its own (Tekken 7 does not start without an audio device);
* t7helper.exe (tools/characters/t7helper, built with MinGW, or the launcher's prebuilt one that $TTT2_T7HELPER
  names) runs next to it in its Wine prefix: it asks Steam, as the game does, which DLC the player owns (only owned
  fighters are dumped), and it calls the game's own loader for each fighter while the game sits on its start-up
  screens: the function that loads a fighter's files by its code (the game's background tasks read and decrypt
  them), then the game's two functions that turn the moveset and the command list into the form it uses; the helper
  knows those functions and checks their first bytes before calling any (another build of the game is refused, not
  crashed), and takes no address from outside;
* the dumper reads the result from the game's memory (memdump.py) and writes .tkmvst files.
Some fighters' files the game keeps encrypted in its paks (Geese Howard's and Noctis's costumes, Noctis's voice bank):
the helper has the game read them with its own file reader (the one its loading uses), which hands them over decoded,
and they are saved under their pak paths in a folder of their own (FILES) that the importer reads them from.
No menu, key or screen is involved, so the player's key bindings, settings, language or pop-ups do not matter.
Nothing is written into the game's files, its saves or its prefix (but the helper's copy at C:\\t7helper.exe).
A session ends what it started and nothing else (Session.close; also on the SIGTERM of the launcher's Cancel, see
signals.py): a Tekken 7 the player starts from Steam meanwhile goes on, and what a killed importer left running is
ended by the next session. Steam from Flatpak or Snap is found, but Tekken 7 is not run through it (not supported
yet).

Written for this project (no third-party code).
"""
import atexit
import fcntl
import hashlib
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import tempfile
import time

from .. import signals
from .pak import plain

APP_ID = 389730
GAME_COMM = 'TekkenGame-Win6'                 # the game's Linux process name (15 characters)
SINK = 'ttt2_tekken7_silent'
SINK_NAME = 'Tekken 7 (silent, for Tekken Tag Tournament 2)'      # (in the sound settings meanwhile)
STEAMWORKS = 'Engine/Binaries/ThirdParty/Steamworks/Steamv132/Win64/steam_api64.dll'
HEIHACHI = 8                                    # every import needs his moveset (the id maps)
BATCH = 4                                       # fighters loaded at once
LOAD_TIMEOUT = 90                               # seconds a batch may take
# Tekken 7's DLC fighters: their roster ids and the Steam app ids of the DLC that has them (Steam's public lists)
DLC = {36: 592370, 43: 592373, 44: 592374, 45: 878460, 46: 878461, 47: 878462, 48: 878463, 49: 878464, 50: 878465,
       51: 1114561, 52: 1162600, 53: 1162601, 54: 1236500, 55: 1289451, 56: 1289453}
RECORD_MOVELIST = 0x68                          # a fighter record's command list: written last, once all is loaded
COMMON_ANIMATIONS = 0x38
# the movesets saved from the game, and the encrypted files as the game reads them (pak paths below it)
MOVESETS = os.path.join(os.path.expanduser('~/.local/share'), 'ttt2', 'tekken7', 'movesets')
FILES = os.path.join(os.path.expanduser('~/.local/share'), 'ttt2', 'tekken7', 'files')
# the helper's build and its run's logs: $XDG_CACHE_HOME/ttt2 as the game's and the launcher's caches
CACHE = os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'ttt2', 'tekken7')
FILE_BATCH = 64                                 # files the game reads at once (the helper takes up to 256) ...
FILE_BATCH_BYTES = 96 << 20                     # ... and their size together, in the game's memory meanwhile


class DumpError(Exception):
    pass


# ---------------------------------------------------------------- finding the game

class Tekken7:
    def __init__(self, steam, game, prefix, proton):
        self.steam, self.game, self.prefix, self.proton = steam, game, prefix, proton


def _text(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


# Steam's folder: the distribution's Steam (or Valve's installer's) first, then Flathub's and the Snap Store's, which
# keep it in a home folder of their own
STEAMS = (('~/.local/share/Steam', None), ('~/.steam/steam', None),
          ('~/.var/app/com.valvesoftware.Steam/.local/share/Steam', 'flatpak'),
          ('~/.var/app/com.valvesoftware.Steam/data/Steam', 'flatpak'),
          ('~/snap/steam/common/.local/share/Steam', 'snap'))
FLATPAK_HOME = '~/.var/app/com.valvesoftware.Steam'
SANDBOX_NAMES = {'flatpak': 'Flatpak', 'snap': 'Snap'}


def _host_path(path, sandbox):
    """A library folder a sandboxed Steam names, as this side sees it: Flatpak's Steam sees its own home folder
    (FLATPAK_HOME) at the player's home, the folders it was given where they are (the Snap Store's sees the
    folders as they are)."""
    home = os.path.expanduser('~')
    if sandbox == 'flatpak' and (path == home or path.startswith(home + os.sep)):
        inside = os.path.expanduser(FLATPAK_HOME) + path[len(home):]
        if os.path.isdir(inside):
            return inside
    return path


def _libraries(steam, sandbox=None):
    out = [steam]
    vdf = os.path.join(steam, 'steamapps', 'libraryfolders.vdf')
    if os.path.isfile(vdf):
        out += [_host_path(p, sandbox) for p in re.findall(r'"path"\s+"([^"]+)"', _text(vdf))]
    return list(dict.fromkeys(out))


def steams():
    """[(Steam's folder, None | 'flatpak' | 'snap')] of the Steams installed, in STEAMS' order (one entry per
    folder: ~/.steam/steam usually links to ~/.local/share/Steam)."""
    out, seen = [], set()
    for path, sandbox in STEAMS:
        path = os.path.expanduser(path)
        if os.path.isdir(path) and os.path.realpath(path) not in seen:
            seen.add(os.path.realpath(path))
            out.append((path, sandbox))
    return out


def _tekken7_library():
    """(Steam's folder, its sandbox (None, 'flatpak', 'snap'), the library Tekken 7 is installed in, its folder)."""
    found = steams()
    if not found:
        raise DumpError('Steam is not installed (no ~/.local/share/Steam, nor Steam from Flatpak or Snap)')
    for steam, sandbox in found:
        for lib in _libraries(steam, sandbox):
            acf = os.path.join(lib, 'steamapps', 'appmanifest_%d.acf' % APP_ID)
            if not os.path.isfile(acf):
                continue
            m = re.search(r'"installdir"\s+"([^"]+)"', _text(acf))
            return steam, sandbox, lib, os.path.join(lib, 'steamapps', 'common', m.group(1) if m else 'TEKKEN 7')
    raise DumpError('Tekken 7 is not installed in any Steam library')


def tekken7_library():
    """(Steam's folder, the Steam library Tekken 7 is installed in, its installation folder), through Steam's files:
    all reading Tekken 7's files needs (find_tekken7: running it)."""
    steam, _, lib, game = _tekken7_library()
    return steam, lib, game


def tekken7_steam():
    """(Steam's folder, its sandbox) of the Steam Tekken 7 is installed through, else of the first Steam installed;
    (None, None) when there is none."""
    try:
        return _tekken7_library()[:2]
    except DumpError:
        found = steams()
        return found[0] if found else (None, None)


def find_tekken7():
    """Tekken 7's install, its Proton prefix and the Proton it uses, through Steam's files."""
    steam, sandbox, lib, game = _tekken7_library()
    if sandbox:
        # (its Proton and its prefix are made for the sandbox, and the sandboxed Steam would have to answer a game
        # started outside it: not tried yet)
        raise DumpError('Tekken 7 is installed through Steam from %s, which is not supported yet: running Tekken 7 '
                        'hidden needs Steam from your distribution\'s packages' % SANDBOX_NAMES[sandbox])
    prefix = os.path.join(lib, 'steamapps', 'compatdata', str(APP_ID))
    info = os.path.join(prefix, 'config_info')
    if not os.path.isdir(os.path.join(prefix, 'pfx')) or not os.path.isfile(info):
        raise DumpError('Tekken 7 has not run with Proton yet: start it once from Steam, then try again')
    lines = _text(info).splitlines()
    proton = next((l.split('/files/')[0] for l in lines if '/files/' in l), None)
    if not proton or not os.path.isfile(os.path.join(proton, 'proton')):
        raise DumpError('the Proton Tekken 7 last ran with is gone; start the game once from Steam')
    return Tekken7(steam, game, prefix, proton)


def _running(comm):
    for pid in os.listdir('/proc'):
        if pid.isdigit():
            try:
                with open('/proc/%s/comm' % pid) as f:
                    if f.read().strip() == comm:
                        return int(pid)
            except OSError:
                pass
    return None


def steam_running():
    return _running('steam') is not None


HELPER_SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 't7helper', 't7helper.c')


def ready_helper(cache):
    """A t7helper.exe that needs no building: the prebuilt one $TTT2_T7HELPER names (the launcher's kit has one), or
    the one built into `cache` when it is newer than its source; else None."""
    prebuilt = os.environ.get('TTT2_T7HELPER')
    if prebuilt and os.path.isfile(prebuilt):
        return prebuilt
    exe = os.path.join(cache, 't7helper.exe')
    src = os.path.normpath(HELPER_SOURCE)
    if os.path.isfile(exe) and os.path.isfile(src) and os.path.getmtime(exe) >= os.path.getmtime(src):
        return exe
    return None


def build_helper(cache):
    """t7helper.exe: a ready one (ready_helper), else built with MinGW into `cache`."""
    ready = ready_helper(cache)
    if ready:
        return ready
    exe = os.path.join(cache, 't7helper.exe')
    src = os.path.normpath(HELPER_SOURCE)
    gcc = shutil.which('x86_64-w64-mingw32-gcc')
    if gcc is None:
        raise DumpError('building the helper needs MinGW (x86_64-w64-mingw32-gcc; Arch: mingw-w64-gcc, '
                        'Debian/Ubuntu: gcc-mingw-w64-x86-64)')
    os.makedirs(cache, exist_ok=True)
    subprocess.run([gcc, '-O2', '-mwindows', '-o', exe, src, '-lws2_32'], check=True)
    return exe


# ---------------------------------------------------------------- the helper

class Helper:
    """A connection to t7helper.exe (serve <port> <secret>)."""

    def __init__(self, port, secret, timeout=90):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
                break
            except OSError:
                if time.time() > end:
                    raise DumpError('the helper did not start')
                time.sleep(0.5)
        self.s.settimeout(180)
        self.buf = b''
        self.send(secret)

    def _fill(self):
        c = self.s.recv(1 << 20)
        if not c:
            raise DumpError('the helper stopped')
        self.buf += c

    def _line(self):
        while b'\n' not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b'\n', 1)
        return line.decode('latin1').rstrip('\r')

    def _exact(self, n):
        while len(self.buf) < n:
            self._fill()
        d, self.buf = self.buf[:n], self.buf[n:]
        return d

    def send(self, line):
        self.s.sendall((line + '\n').encode())

    def _answer(self, line):
        self.send(line)
        answer = self._line()
        if answer.startswith('error'):
            raise DumpError(answer[6:])
        return answer

    def dlc(self, dll):
        self.send('dlc ' + dll)
        out = []
        while True:
            line = self._line()
            if line == 'end':
                return out
            if line.startswith('error'):
                raise DumpError(line[6:])
            a, owned, name = line.split(' ', 2)
            out.append((int(a), owned == '1', name))

    def attach(self):
        return self._answer('attach')

    def ready(self):
        return self._answer('ready') == '1'

    def common(self):
        return int(self._answer('common'), 16)

    def load(self, fighter):
        """-> (record, the game's code of the fighter)."""
        record, code = self._answer('load %d' % fighter).split()
        return int(record, 16), code

    def fixup(self, record, common):
        moveset, movelist = self._answer('fixup %x %x' % (record, common)).split()
        return int(moveset, 16), int(movelist, 16)

    def read(self, address, size):
        self.send('read %x %d' % (address, size))
        line = self._line()
        if not line.startswith('data '):
            raise DumpError('helper: ' + line)
        return self._exact(int(line.split()[1]))

    def u64(self, address):
        return int.from_bytes(self.read(address, 8).ljust(8, b'\0'), 'little')

    def files(self, paths):
        """The game's files (pak paths, 'TekkenGame/Content/...') as the game reads them: [bytes, or None where the
        game could not read one], in order."""
        for p in paths:
            self._answer('want ../../../' + p)
        self.send('fetch')
        out = []
        while True:
            line = self._line()
            if line == 'end':
                return out
            if line.startswith('error'):
                raise DumpError(line[6:])
            _, ok, n = line.split()
            d = self._exact(int(n))
            out.append(d if ok == '1' else None)

    def quit(self):
        try:
            self.send('quit')
            self.s.close()
        except OSError:
            pass


# ---------------------------------------------------------------- the hidden session

INSIDE = r'''#!/bin/sh
# Run by gamescope (headless); the session lasts as long as this script.
if [ "$WITH_GAME" = 1 ]; then
  ( cd "$T7DIR" && PULSE_SINK="$SINK" "$PROTON" run "$T7DIR/TEKKEN 7.exe" > "$RUN/game.log" 2>&1 ) &
  i=0
  while [ $i -lt 360 ]; do
    for f in /proc/[0-9]*/comm; do
      [ "$(cat "$f" 2>/dev/null)" = "TekkenGame-Win6" ] || continue
      p=${f#/proc/}; p=${p%/comm}
      # (this session's game, not one the player started)
      tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qxF "TTT2_T7_SESSION=$TTT2_T7_SESSION" &&
        echo $p > "$RUN/game.pid"
    done
    [ -s "$RUN/game.pid" ] && break
    i=$((i + 1)); sleep 0.5
  done
  [ -s "$RUN/game.pid" ] || exit 1
  # (without the game's app id: Steam would count the helper as the game)
  env -u SteamAppId -u SteamGameId -u STEAM_COMPAT_APP_ID "$PROTON" run 'C:\t7helper.exe' serve "$PORT" "$SECRET" > "$RUN/helper.log" 2>&1 &
  while kill -0 "$(cat "$RUN/game.pid")" 2>/dev/null; do sleep 1; done
  exit 0
fi
exec "$PROTON" run 'C:\t7helper.exe' serve "$PORT" "$SECRET" > "$RUN/helper.log" 2>&1
'''


RUNNING = 'Tekken 7 is running: close it first'
BUSY = 'Tekken 7 is busy with another import (or check of your DLC)'
SESSION_TAG = 'TTT2_T7_SESSION'     # in the environment of all a session starts (Proton and Wine pass it on)


def _lock_path(prefix):
    """The prefix's lock file: the same for every importer run, whatever its cache folder (in XDG_RUNTIME_DIR, the
    user's own folder for such files)."""
    folder = os.environ.get('XDG_RUNTIME_DIR')
    if not folder or not os.path.isdir(folder):
        folder = tempfile.gettempdir()
    name = hashlib.sha1(os.path.realpath(prefix).encode('utf-8', 'surrogateescape')).hexdigest()[:16]
    return os.path.join(folder, 'ttt2-tekken7-%d-%s.lock' % (os.getuid(), name))


def _lock(path, timeout=300):
    """The prefix's lock, waited for: one session at a time (a session's end stops what it started in the prefix,
    and the launcher's check of the DLC must not end an import's session). Never a link's target (it may be in
    /tmp); not passed on to what the session starts, so it is free again as soon as this program has ended."""
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    f = os.fdopen(fd, 'w')
    end = time.time() + timeout
    while True:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return f
        except BlockingIOError:
            if time.time() > end:
                f.close()
                raise DumpError('%s for %d s: try again when it is done' % (BUSY, timeout))
            time.sleep(0.5)


def _free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


# Whose processes are whose: what a session starts has SESSION_TAG in its environment (Wine starts its processes
# detached, so they are no children of the session's gamescope); the player's own Tekken 7 is started by Steam (its
# launch of the app id, Proton's processes in the prefix, the game).

def _processes():
    """{pid: (comm, {name: value} of its environment as it started)} of the processes this user may read."""
    out = {}
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            with open('/proc/%s/comm' % pid) as f:
                comm = f.read().strip()
            with open('/proc/%s/environ' % pid, 'rb') as f:
                raw = f.read()
        except OSError:
            continue
        env = {}
        for item in raw.split(b'\0'):
            name, eq, value = item.partition(b'=')
            if eq:
                env[name.decode('utf-8', 'replace')] = value.decode('utf-8', 'replace')
        out[int(pid)] = (comm, env)
    return out


def _stat(pid):
    """The fields of /proc/<pid>/stat after the command's name (state, parent, ...), or None when it is gone."""
    try:
        with open('/proc/%d/stat' % pid) as f:
            return f.read().rsplit(')', 1)[1].split()
    except (OSError, IndexError):
        return None


def _ancestors(pid):
    out = set()
    while pid > 1 and pid not in out:
        out.add(pid)
        fields = _stat(pid)
        pid = int(fields[1]) if fields else 0
    return out


def _cmdline(pid):
    try:
        with open('/proc/%d/cmdline' % pid, 'rb') as f:
            return f.read().decode('utf-8', 'replace').split('\0')
    except OSError:
        return []


def _same_folder(path, folder):
    return bool(path) and os.path.realpath(path.rstrip('/') or '/') == os.path.realpath(folder)


def _players(prefix, processes):
    """The pids of the processes that are the player's Tekken 7 (or anything else using its prefix): no session's
    (SESSION_TAG), not this program or what started it. Steam starts the game with the app id and the prefix in the
    environment of all of it (its reaper, Proton, Wine's processes), and Proton names the prefix to Wine."""
    mine = _ancestors(os.getpid())
    app = str(APP_ID)
    out = []
    for pid, (comm, env) in processes.items():
        if pid in mine or SESSION_TAG in env:
            continue
        if comm == GAME_COMM or app in (env.get('SteamAppId'), env.get('SteamGameId'), env.get('STEAM_COMPAT_APP_ID')) \
                or _same_folder(env.get('STEAM_COMPAT_DATA_PATH'), prefix) \
                or _same_folder(env.get('WINEPREFIX'), os.path.join(prefix, 'pfx')) \
                or (comm == 'reaper' and 'AppId=%s' % app in _cmdline(pid)):
            out.append(pid)
    return out


def tekken7_running(t7):
    """Whether the player's own Tekken 7 runs (or is being started): a session would share its prefix."""
    return bool(_players(t7.prefix, _processes()))


def _tagged(prefix, processes):
    """{tag: [pids]} of the processes of the sessions in the prefix (a session names it to all it starts)."""
    out = {}
    for pid, (_, env) in processes.items():
        if SESSION_TAG in env and _same_folder(env.get('STEAM_COMPAT_DATA_PATH'), prefix):
            out.setdefault(env[SESSION_TAG], []).append(pid)
    return out


def _new_tag():
    """This program's pid and start time (a pid can come again), and a random part."""
    return '%d.%s.%s' % (os.getpid(), (_stat(os.getpid()) or ['?'] * 20)[19], secrets.token_hex(6))


def _owner_alive(tag):
    """Whether the program that started the session `tag` still runs."""
    pid, _, started = tag.partition('.')
    fields = _stat(int(pid)) if pid.isdigit() else None
    return fields is not None and fields[19] == started.partition('.')[0]


def _kill(pids, sig=signal.SIGKILL):
    for pid in pids:
        try:
            os.kill(pid, sig)
        except OSError:
            pass


def _wait_gone(pids, timeout):
    """Waits until the processes are gone (or zombies); -> whether they are."""
    end = time.time() + timeout
    while True:
        left = [pid for pid in pids if (_stat(pid) or ['Z'])[0] != 'Z']
        if not left or time.time() > end:
            return not left
        time.sleep(0.1)


def _wineserver_kill(t7):
    """Stops everything in the prefix (`wineserver -k`): only when nothing of the player's uses it."""
    wineserver = os.path.join(t7.proton, 'files', 'bin', 'wineserver')
    try:
        subprocess.run([wineserver, '-k'], env=dict(os.environ, WINEPREFIX=os.path.join(t7.prefix, 'pfx')),
                       capture_output=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        pass


def _end(t7, tags, gamescope=None):
    """Ends the processes of the sessions `tags`: everything in the prefix with `wineserver -k` when nothing else
    uses it (the player's Tekken 7, or a session of a program still running), gamescope, then whatever of theirs is
    left (a Wine process the server's end left behind, gamescope's reaper and Xwayland, and all of them when the
    prefix was not theirs alone: Steam's start of Tekken 7 waits for their Wine to end)."""
    processes = _processes()
    if not _players(t7.prefix, processes) and all(t in tags or not _owner_alive(t)
                                                   for t in _tagged(t7.prefix, processes)):
        _wineserver_kill(t7)
    if gamescope is not None and gamescope.poll() is None:
        gamescope.terminate()
        try:
            gamescope.wait(5)
        except subprocess.TimeoutExpired:
            gamescope.kill()
            try:
                gamescope.wait(5)
            except subprocess.TimeoutExpired:
                pass
    left = [pid for t, pids in _tagged(t7.prefix, _processes()).items() if t in tags for pid in pids]
    _kill(left)
    _wait_gone(left, 5)


def _pactl(*args, timeout=10):
    """pactl's output ('' when it fails or hangs: the sound server's trouble is no session's)."""
    try:
        return subprocess.run(['pactl'] + list(args), capture_output=True, text=True, timeout=timeout).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ''


def _sink_modules():
    """The pactl module numbers of the silent outputs sessions loaded."""
    return [line.split('\t')[0] for line in _pactl('list', 'short', 'modules').splitlines()
            if '\tmodule-null-sink\t' in line and 'sink_name=%s' % SINK in line]


_open = []                              # the sessions not closed yet


@atexit.register
def close_all():
    """Closes the sessions still open: at exit, after an exception that came between a session's start and its
    caller's `try` (a signal: signals.py)."""
    for s in list(_open):
        s.close()


class Session:
    """Tekken 7's prefix inside gamescope's headless backend: the helper alone, or the game and the helper.

    close() ends it (also at exit, for an exception that came between its start and the caller's `try`), and only
    what it started: a Tekken 7 the player starts from Steam meanwhile goes on. A session whose program was killed
    (SIGKILL: no close) is ended by the next session."""

    def __init__(self, t7, helper_exe, run, with_game):
        for tool in ('gamescope', 'pactl') if with_game else ('gamescope',):
            if shutil.which(tool) is None:
                raise DumpError('Tekken 7 runs hidden in gamescope, with its sound sent to a silent output (pactl, of '
                                'PipeWire or PulseAudio): install %s with your distribution\'s packages' % tool)
        self.t7, self.run, self.closed = t7, run, False
        self.lock = self.gamescope = self.helper = self.sink_module = None
        self.tag = _new_tag()
        _open.append(self)
        try:
            self._start(helper_exe, with_game)
        except BaseException:
            self.close()
            raise

    def _start(self, helper_exe, with_game):
        t7, run = self.t7, self.run
        os.makedirs(run, exist_ok=True)
        self.lock = _lock(_lock_path(t7.prefix))
        # no other session runs in the prefix now: what one left (its program killed) is ended first
        left = list(_tagged(t7.prefix, _processes()))
        if any(_owner_alive(tag) for tag in left):           # (an importer whose lock file is another)
            raise DumpError('%s: try again when it is done' % BUSY)
        if left:
            _end(t7, left)
        for module in _sink_modules():
            _pactl('unload-module', module)
        if _players(t7.prefix, _processes()):
            raise DumpError(RUNNING)
        if os.path.exists(os.path.join(run, 'game.pid')):
            os.remove(os.path.join(run, 'game.pid'))
        shutil.copyfile(helper_exe, os.path.join(t7.prefix, 'pfx', 'drive_c', 't7helper.exe'))
        if with_game:
            # (the properties in ticks: the description's spaces would end them, which shows as "Tekken")
            self.sink_module = _pactl('load-module', 'module-null-sink', 'sink_name=' + SINK,
                                      'sink_properties=\'device.description="%s"\'' % SINK_NAME).strip() or None
        inside = os.path.join(run, 'inside.sh')
        with open(inside, 'w') as f:
            f.write(INSIDE)
        os.chmod(inside, 0o755)
        self.port, self.secret = _free_port(), secrets.token_hex(16)
        env = dict(os.environ, STEAM_COMPAT_CLIENT_INSTALL_PATH=t7.steam, STEAM_COMPAT_DATA_PATH=t7.prefix,
                   SteamAppId=str(APP_ID), SteamGameId=str(APP_ID), STEAM_COMPAT_APP_ID=str(APP_ID),
                   PROTON=os.path.join(t7.proton, 'proton'), T7DIR=t7.game, RUN=run, SINK=SINK, PORT=str(self.port),
                   SECRET=self.secret, WITH_GAME='1' if with_game else '0')
        env[SESSION_TAG] = self.tag
        with open(os.path.join(run, 'gamescope.log'), 'w') as log:
            self.gamescope = subprocess.Popen(
                ['gamescope', '--backend', 'headless', '-W', '1280', '-H', '720', '-w', '1280', '-h', '720', '-r', '60',
                 '--', inside], env=env, stdout=log, stderr=subprocess.STDOUT)

    def wait_game(self, timeout=240):
        end = time.time() + timeout
        while time.time() < end:
            p = os.path.join(self.run, 'game.pid')
            if os.path.isfile(p) and _text(p).strip():
                return int(_text(p))
            if self.gamescope.poll() is not None:
                raise DumpError('the hidden session ended (see %s)' % self.run)
            time.sleep(0.5)
        raise DumpError('Tekken 7 did not start within %d s' % timeout)

    def connect(self):
        self.helper = Helper(self.port, self.secret)
        return self.helper

    def close(self):
        """Ends the session (a signal meanwhile waits for its end: signals.deferred)."""
        if self.closed:
            return
        self.closed = True
        with signals.deferred():
            try:
                if self.helper:
                    self.helper.quit()
                if self.lock is not None:       # (else another session may be running, and this one started nothing)
                    _end(self.t7, [self.tag], self.gamescope)
                if self.sink_module:
                    _pactl('unload-module', self.sink_module, timeout=5)
            finally:
                if self in _open:
                    _open.remove(self)
                if self.lock:
                    self.lock.close()


# ---------------------------------------------------------------- the encrypted files

def encrypted_files(paks, code):
    """The pak entries of a fighter that the game keeps encrypted: its Tekken 7 code a word of the path (Item/Meshes/
    mrz/, CI_mry_gla_...) or ending a sound bank's name (AkBankVoiceMRZ.bnk), not inside another word (Shaheen's arb
    in polycarbonate, Claudio's ita in guitar: other fighters' encrypted files, which his import does not read)."""
    code = code.lower()
    word = re.compile(r'(?<![a-z])%s(?![a-z])' % re.escape(code))
    return [e for k, e in sorted(paks.index.items())
            if (word.search(k) or k.endswith(code + '.bnk')) and paks.encrypted(e)]


def unsaved_files(paks, code):
    """The fighter's encrypted entries not in paks.saved yet."""
    out = []
    for e in encrypted_files(paks, code):
        p = paks.saved_path(e)
        if not p or not os.path.isfile(p) or os.path.getsize(p) != e.usize:
            out.append(e)
    return out


def _save_files(h, paks, entries):
    """Has the game read the entries (in batches) and saves them below paks.saved."""
    k = 0
    while k < len(entries):
        batch, size = [], 0
        while k < len(entries) and len(batch) < FILE_BATCH and (not batch or size + entries[k].usize <= FILE_BATCH_BYTES):
            batch.append(entries[k])
            size += entries[k].usize
            k += 1
        for e, d in zip(batch, h.files([e.path for e in batch])):
            if d is None or len(d) != e.usize or not plain(e.path, d):
                raise DumpError('Tekken 7 did not read %s' % e.path)
            path = paks.saved_path(e)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path + '.part', 'wb') as f:
                f.write(d)
            os.replace(path + '.part', path)


# ---------------------------------------------------------------- dumping

def owned_dlc(t7, helper_exe, run):
    """{DLC app id: owned} as the game asks Steam."""
    if tekken7_running(t7):                 # (Session checks again, once no other session runs)
        raise DumpError(RUNNING)
    s = Session(t7, helper_exe, run, False)
    try:
        h = s.connect()
        return {a: owned for a, owned, _ in h.dlc('Z:' + os.path.join(t7.game, STEAMWORKS))}
    finally:
        s.close()


def unowned(fighters, cache=None):
    """{roster id: reason} of the fighters (roster ids) whose DLC the player's Steam account does not own, as Steam
    tells the game (Steam must be running). Base-game fighters are always owned."""
    dlc = [fid for fid in fighters if fid in DLC]
    if not dlc:
        return {}
    if not steam_running():
        raise DumpError('Steam is not running: start it, then try again (it tells which fighters you own)')
    cache = cache or CACHE
    owned = owned_dlc(find_tekken7(), build_helper(cache), os.path.join(cache, 'run'))
    return {fid: 'its DLC is not owned' for fid in dlc if not owned.get(DLC[fid], False)}


def _until(what, condition, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if condition():
            return
        time.sleep(0.5)
    raise DumpError(what)


def dump(fighters, out, log=print, cache=None, files=None, paks=None):
    """Dumps the fighters ({roster id: (Tekken 7 code, name)}) into `out` as T7_<name>.tkmvst; Heihachi too (when
    there are any). files: {roster id: (name, [pak entries])}: also saves those encrypted files (from unsaved_files)
    of the fighters, as the game reads them, below paks.saved. -> ({id: path} dumped, {id: reason} skipped)."""
    from . import memdump                   # (numpy: the rest of this module works without it)
    if not steam_running():
        raise DumpError('Steam is not running: start it, then try again (Tekken 7 needs it)')
    t7 = find_tekken7()
    if tekken7_running(t7):
        raise DumpError(RUNNING)
    cache = cache or CACHE
    run = os.path.join(cache, 'run')
    helper_exe = build_helper(cache)
    log('Tekken 7: %s (Proton: %s)' % (t7.game, os.path.basename(t7.proton)))
    owned = owned_dlc(t7, helper_exe, run)
    names = dict(fighters)
    if names:
        names.setdefault(HEIHACHI, ('hei', 'Heihachi'))
    files = {fid: v for fid, v in (files or {}).items() if v[1]}
    skipped = {}
    for fid in list(names) + list(files):
        if fid in DLC and not owned.get(DLC[fid], False):
            skipped[fid] = 'its DLC is not owned'
    wanted = [fid for fid in names if fid not in skipped]
    files = {fid: v for fid, v in files.items() if fid not in skipped}
    dumped = {}
    if not wanted and not files:
        return dumped, skipped
    log('Starting Tekken 7 in the background (no window, no sound) ...')
    s = Session(t7, helper_exe, run, True)
    try:
        s.wait_game()
        h = s.connect()
        end = time.time() + 60
        while True:                                      # (the game's code may not be mapped at once)
            try:
                h.attach()
                break
            except DumpError:
                if time.time() > end:
                    raise
                time.sleep(1)
        _until('Tekken 7 did not get ready', h.ready, 120)
        for fid, (name, entries) in files.items():
            log('  %s: %d encrypted files (%d MB), as the game reads them'
                % (name, len(entries), sum(e.usize for e in entries) >> 20))
            _save_files(h, paks, entries)
        if wanted:
            common = h.common()
            _until('the shared animations did not load', lambda: h.u64(common + COMMON_ANIMATIONS), LOAD_TIMEOUT)
        for k in range(0, len(wanted), BATCH):
            batch = {}
            for fid in wanted[k:k + BATCH]:
                try:
                    batch[fid] = h.load(fid)
                except DumpError as e:
                    skipped[fid] = str(e)
            end = time.time() + LOAD_TIMEOUT
            pending = dict(batch)
            while pending and time.time() < end:
                for fid, (record, code) in list(pending.items()):
                    if not h.u64(record + RECORD_MOVELIST):
                        continue
                    del pending[fid]
                    moveset, movelist = h.fixup(record, common)
                    blocks = memdump.read_moveset(h.read, moveset, movelist)
                    path = os.path.join(out, 'T7_%s.tkmvst' % names[fid][1].replace(' ', '_'))
                    os.makedirs(out, exist_ok=True)       # (only now: a stopped first dump leaves no folder)
                    memdump.write_tkmvst(path, blocks, fid, names[fid][1])
                    dumped[fid] = path
                    log('  %s' % names[fid][1])
                time.sleep(0.5)
            for fid in pending:
                skipped[fid] = 'it did not load'
    finally:
        s.close()
    return dumped, skipped
