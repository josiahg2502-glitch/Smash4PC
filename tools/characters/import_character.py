#!/usr/bin/env python3
"""Adds a fighter from your own copy of Tekken 7 (PC) to Tekken Tag Tournament 2 (this repository's port).

Everything the fighter is made of is read from your files when you run this: your Tekken 7 installation (found
through Steam), its movesets and your installed Tekken Tag Tournament 2. The result is a character pack in
~/.local/share/ttt2/characters/<code>/ (or $XDG_DATA_HOME/ttt2/characters/), which the game loads at start-up. The
pack holds Namco's assets: keep it for yourself, never upload or share it.

    python3 tools/characters/import_character.py kazumi --game ~/Games/TekkenTag2
    python3 tools/characters/import_character.py --all --game ~/Games/TekkenTag2
    python3 tools/characters/import_character.py --remove kazumi --game ~/Games/TekkenTag2

A moveset Tekken 7 only has while it runs: the ones not saved yet are saved first (dump_tekken7.py: Tekken 7 runs in
the background, no window, no sound; Steam must be running), and so are the files Tekken 7 keeps encrypted (Geese's
and Noctis's), as the game reads them. Fighters whose DLC your account does not own are skipped.
Movesets saved from your own Tekken 7 with TKMovesets2 work too (--movesets); DLC fighters are imported only when your
Steam account owns them.

--remove deletes fighters' packs again. The packs left keep their places (their files are made for them) and what
they share is made again for them, from both games' files (not when only packs the game does not load go, which it
never held); the next fighter imported takes the first free place. A pack an older importer made (launcher.py status
"outdated") is not loaded by the game: adding its fighter again replaces it whole, in its place. A newer importer's
pack is not replaced.

--progress adds lines for the launcher (launcher/app), one each, flushed, among the others:
    @step dump <text>              saving what only the running Tekken 7 has (hidden in the background)
    @step fighter <name> <i> <n>   importing fighter i of n
    @step shared                   making what the packs share
    @added <name>                  that fighter's pack is complete
    @removed <name>                (--remove) that fighter's pack is gone
    @skipped <name> <reason>
    @error <text>                  why it failed (the exit code is not 0)
    @done

SIGTERM (the launcher's Cancel) and SIGHUP stop it as Ctrl+C does: Tekken 7 running hidden is closed again (a
Tekken 7 you started yourself is left alone), and the pack being written is not left half written (a fighter added
before keeps its pack as it was); the exit code is then 128 + the signal's number.

Needs Python 3 with numpy and Pillow (--remove only when what the packs left share is made again).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


class Progress:
    """The --progress lines (the launcher reads them), and the end of a run that failed."""

    def __init__(self, on):
        self.on = on
        if on and hasattr(sys.stdout, 'reconfigure'):
            sys.stdout.reconfigure(line_buffering=True)    # (the log lines between them come as they are made)

    def __call__(self, *words):
        if self.on:
            print('@' + ' '.join(str(w) for w in words).replace('\n', ' '), flush=True)

    def fail(self, text):
        self('error', text)
        sys.exit('import_character: %s' % text)


def missing_packages():
    """The Python packages converting needs that this Python lacks, by their pip names."""
    missing = []
    for module in ('numpy', 'PIL'):
        try:
            __import__(module)
        except ImportError:
            missing.append({'PIL': 'Pillow'}.get(module, module))
    return missing


def needs_packages(progress):
    missing = missing_packages()
    if missing:
        progress.fail('needs the Python packages %s (e.g. pip install %s, or your distribution\'s python-numpy / '
                      'python-pillow)' % (', '.join(missing), ' '.join(missing)))


def main():
    sys.path.insert(0, HERE)
    from ttt2char import InputError, signals
    from ttt2char.characters import CHARACTERS, definition
    signals.stop_on_signals()
    parser = argparse.ArgumentParser(
        description='Add a fighter from your own Tekken 7 (PC) to Tekken Tag Tournament 2.',
        epilog='The pack holds Namco\'s assets: it is for your own use; never upload or share it.')
    parser.add_argument('fighter', nargs='*', help='who to add (one or more): kazumi, chloe, eliza, claudio, akuma, '
                        'gigas, josie, katarina, master_raven, shaheen, fahkumram, leroy, lidia, kunimitsu2, negan, '
                        'geese, noctis')
    parser.add_argument('--all', action='store_true', help='every fighter above that your Tekken 7 has')
    parser.add_argument('--tekken7', metavar='FOLDER',
                        help='your Tekken 7 PC installation (the folder holding TekkenGame/; default: found through Steam)')
    parser.add_argument('--movesets', metavar='FOLDER',
                        help='the folder with the .tkmvst movesets saved from your own Tekken 7 (default: %s, filled as '
                             'needed)' % '~/.local/share/ttt2/tekken7/movesets')
    parser.add_argument('--files', metavar='FOLDER',
                        help='the folder with the files Tekken 7 keeps encrypted (Geese\'s, Noctis\'s), as the game reads '
                             'them (default: %s, filled as needed)' % '~/.local/share/ttt2/tekken7/files')
    parser.add_argument('--game', required=True, metavar='FOLDER',
                        help='your installed Tekken Tag Tournament 2 (the folder ttt2 --install made)')
    parser.add_argument('--out', metavar='FOLDER',
                        help='where character packs go (default: $TTT2_CHARACTERS, else '
                             '$XDG_DATA_HOME/ttt2/characters, else ~/.local/share/ttt2/characters)')
    parser.add_argument('--no-shared', action='store_true',
                        help='leave what the packs share (shared/) as it is: for a fighter whose pictures, 2P colours '
                             'and name call have not changed (it is made all the same for a fighter that was not in '
                             'it: new, or its pack an older importer\'s)')
    parser.add_argument('--remove', action='store_true',
                        help='remove the fighters\' packs instead (with --all: every added fighter); the packs left '
                             'keep their places and what they share is made again for them')
    parser.add_argument('--progress', action='store_true',
                        help='also print progress lines for the launcher (@step, @added, @removed, @skipped, @error, '
                             '@done)')
    args = parser.parse_args()
    progress = Progress(args.progress)
    fighters = list(CHARACTERS) if args.all else list(dict.fromkeys(f.lower() for f in args.fighter))
    if not fighters:
        parser.error('name the fighters to %s, or --all' % ('remove' if args.remove else 'add'))
    for fighter in fighters:
        try:
            definition(fighter)
        except InputError as error:
            progress.fail(str(error))
    if not args.remove:
        needs_packages(progress)
    try:
        (remove if args.remove else add)(args, fighters, progress)
    except Exception as error:              # (a bug: the launcher shows what it was; the traceback follows)
        progress('error', 'the importer failed: %s: %s' % (type(error).__name__, error))
        raise
    progress('done')


def add(args, fighters, progress):
    from ttt2char import InputError, importer
    from ttt2char import pack
    from ttt2char.tekken7 import autodump
    from ttt2char.tekken7.pak import Paks
    try:
        # (its files are all the import reads; the dump finds Proton and the prefix itself, if it is needed)
        tekken7 = os.path.expanduser(args.tekken7) if args.tekken7 else autodump.tekken7_library()[2]
    except autodump.DumpError as error:
        progress.fail('%s (or give --tekken7)' % error)
    movesets = os.path.expanduser(args.movesets or autodump.MOVESETS)
    files_folder = os.path.expanduser(args.files) if args.files else autodump.FILES
    game = os.path.expanduser(args.game)
    out = args.out and os.path.expanduser(args.out)
    try:
        folder = pack.output_folder(out)
    except InputError as error:
        progress.fail(str(error))
    # only fighters the player owns, whichever movesets are already saved (DLC fighters: as Steam tells)
    ids = [importer.definition(f).TEKKEN7_ID for f in fighters]
    if any(fid in autodump.DLC for fid in ids):
        progress('step', 'dump', 'Asking Steam which DLC you own (a helper runs hidden in Tekken 7\'s Proton prefix)')
    try:
        skipped = autodump.unowned(ids)
    except autodump.DumpError as error:
        progress.fail(str(error))
    for fighter in list(fighters):
        d = importer.definition(fighter)
        if d.TEKKEN7_ID in skipped:
            print('Skipped %s: %s.' % (d.NAME, skipped[d.TEKKEN7_ID]))
            progress('skipped', fighter, skipped[d.TEKKEN7_ID])
            fighters.remove(fighter)
    if not fighters:
        progress.fail('none of the fighters asked for is owned')
    # the movesets and encrypted files not saved yet: Tekken 7 in the background, once for all of them
    try:
        paks = Paks(importer.tekken7_paks(tekken7), saved=files_folder)
    except InputError as error:
        progress.fail(str(error))
    have = importer.saved_movesets(movesets)
    needed, files = {}, {}
    for fighter in fighters:
        d = importer.definition(fighter)
        if d.TEKKEN7_ID not in have:
            needed[d.TEKKEN7_ID] = (d.TEKKEN7_CODE, d.NAME)
        entries = autodump.unsaved_files(paks, d.TEKKEN7_CODE)
        if entries:
            files[d.TEKKEN7_ID] = (d.NAME, entries)
    if importer.HEIHACHI not in have:                       # (every import needs his)
        needed[importer.HEIHACHI] = ('hei', 'Heihachi')
    if needed or files:
        print('Saving %d Tekken 7 moveset(s) and %d fighter(s)\' encrypted files first (Tekken 7 runs hidden in the '
              'background for a minute or two) ...' % (len(needed), len(files)))
        progress('step', 'dump', 'Saving what Tekken 7 has only while it runs (it runs hidden in the background for a '
                 'minute or two)')
        try:
            _, skipped = autodump.dump(needed, movesets, files=files, paks=paks)
        except autodump.DumpError as error:
            progress.fail(str(error))
        who = {fid: name for fid, (_, name) in needed.items()}
        who.update((fid, name) for fid, (name, _) in files.items())
        for fid, why in skipped.items():
            print('Skipped %s: %s.' % (who.get(fid, 'Heihachi'), why))
        have = importer.saved_movesets(movesets)
        saved = [f for f in fighters if importer.definition(f).TEKKEN7_ID in have and
                 not autodump.unsaved_files(paks, importer.definition(f).TEKKEN7_CODE)]
        for fighter in fighters:
            if fighter not in saved:
                progress('skipped', fighter, skipped.get(importer.definition(fighter).TEKKEN7_ID,
                                                         'it could not be saved from Tekken 7'))
        fighters = saved
        if importer.HEIHACHI not in have:
            progress.fail('Heihachi\'s moveset could not be saved')
        if not fighters:
            progress.fail('none of the fighters asked for could be saved from Tekken 7')
    # the fighters shared/ has no pictures of (it holds the packs the game loads): a new one, or an outdated pack's
    pack.recover(folder)
    shown = {p['code'] for _, p in pack.loaded(pack.installed(folder))}
    unshared = [f for f in fighters if importer.definition(f).CODE not in shown]
    for n, fighter in enumerate(fighters):
        progress('step', 'fighter', fighter, n + 1, len(fighters))
        try:
            # what the packs share is made once, after the last fighter
            target = importer.run(fighter, tekken7, movesets, game, out, make_shared=False, files=files_folder)
        except InputError as error:
            progress.fail(str(error))
        slot = int(pack.read_ini(os.path.join(target, 'character.ini')).get('slot', '0'))
        cell = pack.place(slot)
        rows = ('top', 'middle', 'bottom')
        where = ('a cell at the %s end of the select screen\'s %s row' % ('left' if cell['column'] < 10 else 'right',
                                                                         rows[cell['grid_row']])
                 if cell['column'] is not None else 'no cell (the select screen is full)')
        print('Done: %s has %s.' % (fighter, where))
        progress('added', fighter)
    if args.no_shared and unshared:
        print('What the packs share is made again all the same: %s had no pictures, colours or name call in it.'
              % ', '.join(importer.definition(f).NAME for f in unshared))
    if not args.no_shared or unshared:
        progress('step', 'shared')
        try:
            importer.make_shared(tekken7, game, out, files=files_folder)
        except InputError as error:
            progress.fail(str(error))
    print('Start the game. The packs (in %s) hold Namco\'s assets: keep them for yourself.' % os.path.dirname(target))


def remove(args, fighters, progress):
    from ttt2char import InputError, pack              # (no numpy until what the packs left share is made again)
    from ttt2char.characters import CHARACTERS, definition
    from ttt2char.tekken7 import autodump
    try:
        out = pack.output_folder(args.out and os.path.expanduser(args.out))
    except InputError as error:
        progress.fail(str(error))
    game = os.path.expanduser(args.game)
    files_folder = os.path.expanduser(args.files) if args.files else autodump.FILES
    pack.recover(out)
    packs = {}
    for p in pack.installed(out):
        packs.setdefault(p['code'], []).append(p['path'])
    current = {p['code'] for _, p in pack.loaded(pack.installed(out))}         # (shared/ holds these)
    going = []
    for fighter in fighters:
        d = definition(fighter)
        if d.CODE in packs:
            going.append(fighter)
        elif not args.all:
            print('Skipped %s: it is not added (no pack of %s in %s).' % (d.NAME, d.CODE, out))
            progress('skipped', fighter, 'it is not added')
    if not going:
        print('No pack to remove in %s.' % out)
        return
    known = {definition(f).CODE for f in CHARACTERS}
    gone = {definition(f).CODE for f in going}
    left = [code for code in packs if code in known and code in current and code not in gone]   # (shared/'s)
    # shared/ changes only when a fighter it holds goes (an outdated pack's never was in it), or none is left
    remake = bool(left) and bool(gone & current & known) and not args.no_shared
    tekken7 = importer = None
    if remake:
        # what the packs left share is made again from both games' files (Tekken 7's paks, not its Proton): they must
        # be there, and what converts them, before a pack goes
        needs_packages(progress)
        from ttt2char import importer
        why = None
        try:
            tekken7 = os.path.expanduser(args.tekken7) if args.tekken7 else autodump.tekken7_library()[2]
            importer.tekken7_paks(tekken7)
            importer.ttt2_folder(game)
        except autodump.DumpError as error:
            why = '%s (or give --tekken7)' % error
        except InputError as error:
            why = str(error)
        if why:
            progress.fail('%s; the fighters left need both games\' files for what they share, so nothing was removed'
                          % why)
    changed = False
    try:
        for fighter in going:
            for path in packs[definition(fighter).CODE]:
                changed = True
                try:
                    pack.remove(path)
                except OSError as error:
                    progress.fail('cannot remove %s: %s' % (path, error))
                print('Removed %s (%s).' % (definition(fighter).NAME, path))
            progress('removed', fighter)
        if args.no_shared or (left and not remake):
            return
        progress('step', 'shared')
        try:
            if left:
                importer.make_shared(tekken7, game, out, files=files_folder)
            else:
                pack.remove_shared(out)
                print('No added fighter is left: removed %s.' % os.path.join(out, pack.SHARED))
        except (InputError, OSError) as error:
            progress.fail('%s; the fighters left are without what they share until a fighter is imported again'
                          % error)
    except BaseException:
        # stopped (a failure, the launcher's Cancel) before what the packs share was made again: what it held would
        # show the removed fighters
        if changed and (remake or not left) and not args.no_shared:
            try:
                pack.remove_shared(out)
            except OSError:
                pass
        raise


if __name__ == '__main__':
    main()
