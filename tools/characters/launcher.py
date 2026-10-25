#!/usr/bin/env python3
"""What the launcher (launcher/app) asks about adding fighters, answered as one JSON object on stdout:

    python3 tools/characters/launcher.py status [--game FOLDER] [--movesets FOLDER]
    python3 tools/characters/launcher.py owned

status: what is there, quickly (nothing is started): this Python and whether it has numpy and Pillow (the import
needs them; this does not), the tools running Tekken 7 hidden needs, Steam (its folder, and "flatpak" or "snap" for a
sandboxed one), Tekken 7 (found through Steam as the importer finds it, or why it is not usable), the characters
folder (as the game finds it) and every fighter the importer knows: its name, whether it is a DLC fighter (the DLC's
Steam app id), whether its pack is there ("installed"), whether that pack is an older importer's ("outdated": the
game does not load it; adding the fighter again replaces it) or a newer one's ("newer": the game does not load it
either, and this importer does not replace it; the game needs updating) and whether its moveset is saved. With --game:
whether that folder is an installed game the importer can read.

owned: which DLC fighters the player's Steam account owns, as Steam tells the game ({"owned": {name: true|false}}):
the helper runs hidden in Tekken 7's Proton prefix while Proton starts it and it asks (not the game). Steam must be
running; when it is not (or Tekken 7 cannot be used) the answer is {"error": "<why>"} at once. Steam is never started.
SIGTERM (the launcher's Cancel) ends the helper's session as its end does, and nothing is printed (exit code 143).

The exit code is 0 whenever the object is printed; adding and removing fighters is import_character.py's (--progress).
Needs only Python 3; works from a read-only copy (nothing is written next to it).
"""
import argparse
import json
import os
import platform
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ttt2char import pack, signals                             # noqa: E402  (these need no numpy)
from ttt2char.characters import CHARACTERS, definition        # noqa: E402
from ttt2char.tekken7 import autodump                          # noqa: E402
from ttt2char.tekken7.pak import pak_names                     # noqa: E402
from ttt2char.tekken7.tkmvst import saved_movesets             # noqa: E402
from ttt2char.ttt2 import title_folder                         # noqa: E402

MINGW = 'x86_64-w64-mingw32-gcc'


def importable(module):
    try:
        __import__(module)
        return True
    except Exception:                       # (ImportError, or whatever a broken installation raises)
        return False


def tekken7():
    """{found, path, error}: Tekken 7 as the importer finds it (autodump.find_tekken7), or why it is not usable."""
    try:
        path, error = autodump.find_tekken7().game, None
    except autodump.DumpError as e:
        error = str(e)
        try:
            path = autodump.tekken7_library()[2]
        except autodump.DumpError:
            path = None
    found = path is not None and os.path.isdir(path)
    if path is not None and not found:
        error = 'Steam lists Tekken 7 in %s, but it is not there: install it again from Steam' % path
    elif found and error is None and not pak_names(os.path.join(path, 'TekkenGame', 'Content', 'Paks')):
        error = 'Tekken 7 has no pak files in %s: let Steam verify its files' % path
    return {'found': found, 'path': path, 'error': error}


def steam():
    """{installed, running, path, sandbox}: the Steam Tekken 7 is installed through (else the first one installed);
    sandbox 'flatpak' or 'snap' for a Steam from Flathub or the Snap Store (Tekken 7 is not run through it yet:
    tekken7's error says so), else None."""
    path, sandbox = autodump.tekken7_steam()
    return {'installed': path is not None, 'running': autodump.steam_running(), 'path': path, 'sandbox': sandbox}


def status(game=None, movesets=None):
    folder = os.path.abspath(pack.default_folder())
    packs = {}                      # {code: its packs' state: 'current' (one is loaded), else 'newer' or 'outdated'}
    rank = ('outdated', 'newer', 'current')
    for p in pack.installed(folder):
        state = 'current' if p['current'] else 'newer' if p['newer'] else 'outdated'
        if rank.index(state) >= rank.index(packs.get(p['code'], 'outdated')):
            packs[p['code']] = state
    movesets = os.path.abspath(os.path.expanduser(movesets or autodump.MOVESETS))
    saved = saved_movesets(movesets)
    helper = 'prebuilt' if autodump.ready_helper(autodump.CACHE) else 'mingw' if shutil.which(MINGW) else None
    fighters = []
    for name in CHARACTERS:
        d = definition(name)
        fighters.append({'name': name, 'display': d.NAME.split(' (')[0], 'dlc': autodump.DLC.get(d.TEKKEN7_ID),
                         'installed': d.CODE in packs, 'outdated': packs.get(d.CODE) == 'outdated',
                         'newer': packs.get(d.CODE) == 'newer',
                         'moveset_saved': d.TEKKEN7_ID in saved})
    out = {'python': {'version': platform.python_version(), 'numpy': importable('numpy'),
                      'pillow': importable('PIL.Image')},
           'tools': {'gamescope': shutil.which('gamescope'), 'pactl': shutil.which('pactl'), 'helper': helper},
           'steam': steam(),
           'tekken7': tekken7(),
           'packs_folder': folder,
           'movesets': {'folder': movesets, 'heihachi': autodump.HEIHACHI in saved},     # (every import needs his)
           'fighters': fighters}
    if game is not None:
        title = title_folder(os.path.expanduser(game))
        out['game'] = {'path': title, 'error': None if title else
                       '%s is no installed Tekken Tag Tournament 2 (no content/hdd in it or its game/)' % game}
    return out


def owned():
    dlc = {definition(name).TEKKEN7_ID: name for name in CHARACTERS if definition(name).TEKKEN7_ID in autodump.DLC}
    try:
        unowned = autodump.unowned(list(dlc))               # (Steam not running: an error at once)
    except autodump.DumpError as error:
        return {'error': str(error)}
    except Exception as error:              # (building or running the helper: the launcher shows why)
        return {'error': 'the helper failed: %s: %s' % (type(error).__name__, error)}
    return {'owned': {name: fid not in unowned for fid, name in dlc.items()}}


def main():
    parser = argparse.ArgumentParser(description='Answer the launcher\'s questions about adding fighters, as JSON.')
    commands = parser.add_subparsers(dest='command', required=True)
    asked = commands.add_parser('status', help='what is there (fast; nothing is started)')
    asked.add_argument('--game', metavar='FOLDER', help='an installed game folder to check')
    asked.add_argument('--movesets', metavar='FOLDER',
                       help='the saved movesets\' folder (default: %s)' % '~/.local/share/ttt2/tekken7/movesets')
    commands.add_parser('owned', help='which DLC fighters the Steam account owns (Steam must be running)')
    args = parser.parse_args()
    signals.stop_on_signals()               # (owned: the launcher's Cancel ends the hidden helper too)
    answer = status(args.game, args.movesets) if args.command == 'status' else owned()
    print(json.dumps(answer), flush=True)


if __name__ == '__main__':
    main()
