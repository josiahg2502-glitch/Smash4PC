#!/usr/bin/env python3
"""Saves Tekken 7 fighters' movesets (what import_character.py needs from the running game) without you doing
anything: Tekken 7 runs in the background, with no window and no sound, its own loader loads the fighters while it
sits on its start-up screens (nothing is pressed), and it is closed again. The files Tekken 7 keeps encrypted (Geese's
and Noctis's costumes, Noctis's voice) are saved too, as the game reads them (--files). Only fighters your Steam
account owns are dumped; the others are listed as skipped.

    python3 tools/characters/dump_tekken7.py --all
    python3 tools/characters/dump_tekken7.py kazumi negan

Needs Steam running, Tekken 7 started once from Steam (its Proton prefix), gamescope, PulseAudio/PipeWire (pactl),
MinGW (x86_64-w64-mingw32-gcc, to build the small helper the first time) and Python 3.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(os.path.expanduser('~/.local/share'), 'ttt2', 'tekken7', 'movesets')


def main():
    sys.path.insert(0, HERE)
    from ttt2char import InputError, importer, signals
    from ttt2char.tekken7 import autodump
    from ttt2char.tekken7.pak import Paks
    signals.stop_on_signals()               # (SIGTERM closes the hidden Tekken 7 as Ctrl+C does)
    parser = argparse.ArgumentParser(description='Save Tekken 7 movesets in the background, for import_character.py.')
    parser.add_argument('fighter', nargs='*', help='fighters (the importer\'s names); none with --all')
    parser.add_argument('--all', action='store_true', help='every fighter the importer knows')
    parser.add_argument('--out', default=DEFAULT_OUT, metavar='FOLDER', help='where the .tkmvst files go (default: %(default)s)')
    parser.add_argument('--files', default=autodump.FILES, metavar='FOLDER',
                        help='where the files Tekken 7 keeps encrypted go, as the game reads them (default: %(default)s; '
                             'import_character.py reads them there)')
    args = parser.parse_args()
    names = list(importer.CHARACTERS) if args.all else args.fighter
    if not names:
        parser.error('name fighters, or --all')
    try:
        paks = Paks(importer.tekken7_paks(autodump.find_tekken7().game), saved=os.path.expanduser(args.files))
    except (autodump.DumpError, InputError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1
    fighters, files = {}, {}
    for n in names:
        d = importer.definition(n)
        fighters[d.TEKKEN7_ID] = (d.TEKKEN7_CODE, d.NAME)
        entries = autodump.unsaved_files(paks, d.TEKKEN7_CODE)
        if entries:
            files[d.TEKKEN7_ID] = (d.NAME, entries)
    try:
        dumped, skipped = autodump.dump(fighters, os.path.expanduser(args.out), files=files, paks=paks)
    except autodump.DumpError as e:
        print('error: %s' % e, file=sys.stderr)
        return 1
    print('Saved %d moveset(s) to %s.' % (len(dumped), args.out))
    saved = [name for fid, (name, _) in files.items() if fid not in skipped]
    if saved:
        print('Saved the encrypted files of %s to %s.' % (', '.join(saved), args.files))
    for fid, why in skipped.items():
        print('Skipped %s: %s.' % (fighters.get(fid, ('', 'Heihachi'))[1], why))
    return 0


if __name__ == '__main__':
    sys.exit(main())
