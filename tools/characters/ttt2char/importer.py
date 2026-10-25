"""The importer's steps: check the inputs, convert, write the pack (import_character.py is the command line)."""
import collections
import json
import os
import struct
import time

from . import InputError, pack, shared
from .characters import CHARACTERS, definition
from .convert import art, chains, effects as fx, idmaps, itemdb, model, movelist, moveset, particles, tagsystem, \
    tiger, voice, weapons
from .tekken7 import tkmvst
from .tekken7.autodump import FILES
from .tekken7.pak import EncryptedError, Paks, pak_names
from .ttt2 import bone, motbin, title_folder
from .ttt2.archive import Archives, key_of
from .ttt2.rpx import Rpx

HEIHACHI = 8          # Tekken 7's roster id of Heihachi Mishima, the fighter both games share (for the id maps)

TKMOVESETS_HELP = (
    'Save the movesets with TKMovesets2 (github.com/Kiloutre/TKMovesets) while Tekken 7 runs: Practice mode with '
    '%s as player 1 and Heihachi as player 2, then "Extract (all)" (any compression). The two .tkmvst files it '
    'writes to its extracted_chars folder are what --movesets needs.')


def tekken7_paks(folder):
    """The Paks of the player's Tekken 7 PC installation (the folder holding TekkenGame/)."""
    paks = os.path.join(folder or '', 'TekkenGame', 'Content', 'Paks')
    if not folder or not pak_names(paks):
        raise InputError('--tekken7 must be your own Tekken 7 PC installation, the folder holding TekkenGame/ '
                         '(Steam: Library > Tekken 7 > Manage > Browse local files); found no pak files in %s'
                         % paks)
    return paks


def ttt2_folder(folder):
    """The installed Tekken Tag Tournament 2's title folder (code/, content/, meta/): `folder`, or its game/."""
    if folder and os.path.isfile(folder):
        raise InputError('--game must be the folder Tekken Tag Tournament 2 is installed in, not the file %s'
                         % os.path.basename(folder))
    title = title_folder(folder)
    if title is None:
        raise InputError('%s is no installed Tekken Tag Tournament 2 (no content/hdd in it or its game/): --game must '
                         'be the folder the game is installed in' % (folder or 'the --game folder'))
    return title


saved_movesets = tkmvst.saved_movesets          # (there: launcher.py reads them without numpy)


def find_movesets(folder, d):
    """The fighter's and Heihachi's .tkmvst files in `folder`."""
    if not folder or not os.path.isdir(folder):
        raise InputError('--movesets must be the folder with the .tkmvst files. ' + TKMOVESETS_HELP % d.NAME)
    found = saved_movesets(folder)
    missing = [who for who, i in ((d.NAME, d.TEKKEN7_ID), ('Heihachi', HEIHACHI)) if i not in found]
    if missing:
        raise InputError('%s has no Tekken 7 moveset of %s. ' % (folder, ' and '.join(missing))
                         + TKMOVESETS_HELP % d.NAME)
    return found[d.TEKKEN7_ID], found[HEIHACHI]


def run(name, tekken7, movesets, game, out=None, log=print, make_shared=True, files=FILES):
    """Imports fighter `name`; returns the pack's folder. make_shared: also remake what the packs share (shared/; an
    import of several fighters makes it once, after the last). files: the folder of the files Tekken 7 keeps
    encrypted, as the game reads them (tekken7/autodump.py saves them)."""
    try:
        return _run(name, tekken7, movesets, game, out, log, make_shared, files)
    except EncryptedError as error:
        raise InputError('Tekken 7 keeps %s encrypted and it has not been saved from the running game yet: run '
                         'dump_tekken7.py %s (or import_character.py, which runs it) with Steam running'
                         % (error.args[0], name))


def make_shared(tekken7, game, out=None, log=print, files=FILES):
    """Remakes what the packs in `out` share (shared/) for the packs there now, from both games' files: after imports
    with make_shared=False, or after packs were removed. -> shared.build's [(slot, definition, pack folder)]."""
    paks_dir = tekken7_paks(tekken7)
    game = ttt2_folder(game)
    out = pack.output_folder(out)
    step = _steps(log)
    step('reading Tekken 7 (%s) and Tekken Tag Tournament 2 (%s)' % (tekken7, game))
    try:
        return _shared(_paks(paks_dir, files), _archives(game), _rpx(game), out, log, step)
    except EncryptedError as error:
        raise InputError('Tekken 7 keeps %s encrypted and it has not been saved from the running game yet: run '
                         'dump_tekken7.py for that fighter (or import_character.py, which runs it) with Steam running'
                         % error.args[0])


def _steps(log):
    """step(text): logs the text after the seconds from now."""
    start = time.time()
    return lambda text: log('%5.1f s  %s' % (time.time() - start, text))


def _paks(paks_dir, files):
    try:
        return Paks(paks_dir, saved=files)
    except (OSError, ValueError, struct.error) as error:
        raise InputError('cannot read the pak files in %s (%s): let Steam verify Tekken 7\'s files' % (paks_dir, error))


def _archives(game):
    try:
        return Archives(game)
    except (OSError, ValueError, struct.error) as error:
        raise InputError('cannot read the archives in %s (%s): install Tekken Tag Tournament 2 again' % (game, error))


def _rpx(game):
    try:
        return Rpx.of_game(game)
    except (OSError, ValueError) as error:
        raise InputError('cannot read the game\'s executable in %s (%s): install Tekken Tag Tournament 2 again'
                         % (game, error))


def _shared(paks, archives, rpx, out, log, step):
    step('what the added fighters share: their pictures in the title\'s screens, colours and name calls')
    known = {}
    for name in CHARACTERS:
        other = definition(name)
        known[other.CODE] = other
    together = shared.build(paks, archives, rpx, out, known, log=lambda s: log('         ' + s))
    if together:
        step('wrote %s for %s' % (os.path.join(out, pack.SHARED), ', '.join('%s (place %d)' % (f[1].NAME, f[0])
                                                                           for f in together)))
    else:
        step('no added fighter left: %s is gone' % os.path.join(out, pack.SHARED))
    return together


def _run(name, tekken7, movesets, game, out, log, make_shared, files):
    d = definition(name)
    paks_dir = tekken7_paks(tekken7)
    own, heihachi = find_movesets(movesets, d)
    game = ttt2_folder(game)
    out = pack.output_folder(out)
    step = _steps(log)

    step('reading Tekken 7 (%s) and Tekken Tag Tournament 2 (%s)' % (tekken7, game))
    paks = _paks(paks_dir, files)
    archives = _archives(game)
    if not archives.has(key_of(d.DONOR + '.xml')):
        raise InputError('%s has no %s files: is it the Wii U game\'s folder?' % (game, d.DONOR))

    slot = pack.choose_slot(out, d.CODE)
    if slot is None:
        raise InputError('the game has room for %d added fighters and %s holds them already: delete one\'s folder '
                         'first' % (pack.MAX_FIGHTERS, out))
    pack.target_of(out, d)          # (where the pack goes can take it: checked before the minute converting takes)
    place = pack.place(slot)
    rpx = _rpx(game)
    if any(p['code'] == d.CODE and not p['current'] for p in pack.installed(out)):
        step('%s\'s pack in %s was made by an older importer: it is replaced whole (place %d kept)'
             % (d.NAME, out, slot))

    step('model: %s (place %d)' % (d.NAME, slot))
    nud, nut, bon, palettes = model.convert(paks, archives, d, slot, log=lambda s: log('         ' + s.strip()))
    donor_xml = archives.read_name(d.DONOR + '.xml')
    effect_files, _, _ = fx.copy_files(archives, d.DONOR, itemdb.effect_names(donor_xml), {},
                                       log=lambda s: log('         ' + s))
    part3, opt_anims, mop = None, {}, None
    if getattr(d, 'TIGER', None):
        step('tiger: its animations and its bones\' spring entries')
        skel = bone.read(bon)
        part3, opt_anims = tiger.animations(paks, d, skel, log=lambda s: log('         ' + s.strip()))
        mop = tiger.spring_file(archives.read_name(d.DONOR + '.mop'),
                                archives.read_name(tiger.SPRING_TEMPLATE[0] + '.mop'),
                                sum(1 for n in skel['opt'] if tiger.OPT_FIRST <= n < weapons.OPT_FIRST))
    swinging = [n for n in bone.read(bon)['opt'] if chains.OPT_FIRST <= n < tiger.OPT_FIRST]
    if swinging:
        template = archives.read_name(chains.SPRING_TEMPLATE[0] + '.mop')
        donor_mop = archives.read_name(d.DONOR + '.mop') if archives.has(key_of(d.DONOR + '.mop')) else None
        mop = chains.spring_file(mop or donor_mop or chains.empty_spring_file(template), template, swinging)
    still = [n for n in bone.read(bon)['opt'] if n >= weapons.OPT_FIRST]
    if still:                                     # weapons: still extra bones (Devil Jin's wing entry, flag 8)
        template = archives.read_name(tiger.SPRING_TEMPLATE[0] + '.mop')
        donor_mop = archives.read_name(d.DONOR + '.mop') if archives.has(key_of(d.DONOR + '.mop')) else None
        mop = chains.spring_file(mop or donor_mop or chains.empty_spring_file(template), template, still,
                                 template=tiger.SPRING_TEMPLATE[1], links=False)

    step('pictures: %s\'s Tekken 7 pictures for the loading and VS screens and the costume chooser' % d.NAME)
    pictures = art.pack_files(paks, archives, rpx, d, slot, log=lambda s: log('         ' + s))

    step('voice: %s\'s Tekken 7 voice in %s\'s sound banks' % (d.NAME, d.DONOR))
    try:
        clips = voice.Clips(paks, d.TEKKEN7_CODE, d.VOICE_PREFIX)
        vlist = voice.voice_list(rpx, d.DONOR)
        banks, lines, wins, voice_extra = voice.build(
            clips, {w: archives.raw(key_of('%s_%s.nus3bank' % (w, d.DONOR))) for w in ('se_Chr', 'se_Dwi', 'rm_Dst',
                                                                                     'rm_Dwi')}, d.CODE, vlist, slot)
    except KeyError as error:
        raise InputError('%s has no %s voice files (%s): is it the Wii U game\'s folder?' % (game, d.DONOR, error))
    merged, chr_tones = voice.merge(banks.pop('se_Chr_%s.nus3bank' % d.CODE), banks.pop('se_Dwi_%s.nus3bank' % d.CODE))
    banks['se_Chr_%s.nus3bank' % d.CODE] = merged
    lines.append('se_Chr_%s: with se_Dwi_%s\'s tones after its %d (one bank in a match), %d KB'
                 % (d.CODE, d.CODE, chr_tones, len(merged) // 1024))
    for line in lines:
        log('         ' + line)

    step('moveset: the id maps from Heihachi in both games')
    k = tkmvst.read(own)
    h7 = tkmvst.read(heihachi)
    evidence = collections.defaultdict(idmaps.Evidence)
    for who, row in d.ALIGN:
        idmaps.align(k if who == 'self' else h7, motbin.read(archives.entry('data006', row)), evidence)
    req, prop = idmaps.build_maps(evidence)
    maps = (req, prop, idmaps.reaction_remap(evidence), idmaps.hitbox_remap(evidence))
    ttt2_movesets = [motbin.read(archives.entry('data006', row)) for row in range(58)]
    usage = idmaps.ttt2_usage(ttt2_movesets)
    tag_states = tagsystem.TagStates(ttt2_movesets)     # movement states: tag out (walking back, ...)
    del ttt2_movesets

    step('moveset: converting')
    c = moveset.Converter(k, h7, motbin.read(archives.entry('data006', d.DONOR_ROW)),
                          archives.entry('data005', d.DONOR_ROW), motbin.read(archives.entry('data006', 13)),
                          archives.entry('data004', 0), maps, usage, place['row'], d)
    c.win_tones = wins
    c.shouts = sum(1 for mask, _ in vlist if mask & voice.KIAI) + len(voice_extra)
    c.opt_anims, c.part3 = opt_anims, part3
    c.tag_states = tag_states
    c.hit_locations = dict(getattr(d, 'HIT_LOCATIONS', {}))     # Tekken 7 hit locations TTT2 numbers otherwise
    c.hit_locations.update(weapons.points_of(bone.read(bon)['names']))         # ... and its weapons' points
    c.run()
    copies = c.split_effects(particles.emitting_frames(paks, d.TEKKEN7_CODE, c.effect_ids),
                             set(c.projectile_effects) | set(itemdb.effect_ids(donor_xml)))
    moves, motions = c.write()
    for line in c.summary():
        log('         ' + line)

    step('effects: the Tekken 7 effects %s\'s moves spawn' % d.NAME)
    own_files, own_effects = particles.convert_fighter(
        paks, archives, d.TEKKEN7_CODE, c.effect_ids | {particles.HIT_EFFECT},
        model.GIDX_BASE + model.SLOT_GIDX * slot + particles.FIRST_ID, c.projectile_effects,
        log=lambda s: log('         ' + s), copies=copies)
    xml = itemdb.body_only(donor_xml, {i: [(name, None) for name in names] for i, names in own_effects.items()})
    effect_files = fx.still_named(effect_files, itemdb.effect_files_named(xml))    # the donor's not replaced
    effect_files.update(own_files)

    mvl = None
    if k.movelist[:4] == b'MVLT':
        step('command list')
        mvl = movelist.convert(k.movelist, log=lambda s: log('         ' + s))

    files = {'.xml': xml, '.nud': nud, '.nut': nut, '.bon': bon, '.moveset': moves, '.motion': motions}
    files.update(banks)
    files.update(pictures)
    files.update(effect_files)
    if mvl is not None:
        files['.mvl'] = mvl
    if mop is not None:
        files['.mop'] = mop
    if palettes:
        files['colours.json'] = json.dumps({str(nn): v for nn, v in sorted(palettes.items())}).encode('utf-8')
    target = pack.write(out, d, files, slot, chr_tones=chr_tones, voice_extra=voice_extra,
                        weapons=weapons.of_skeleton(bone.read(bon), getattr(d, 'WEAPONS', ())))
    step('wrote %s' % target)

    if make_shared:
        _shared(paks, archives, rpx, out, log, step)
    return target
