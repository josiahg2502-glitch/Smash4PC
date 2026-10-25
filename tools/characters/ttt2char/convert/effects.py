"""The added fighter's effect files: copies of TTT2's own, under the fighter's code.

TTT2 finds a fighter's character effects through its item database's `cmn_eff_default` Instance: EffectGroup names
without a path, which the title expands to `<code>/alo/<name>.alo` with the fighter's own code, and EffectParameters
mapping the effect ids its moves fire (move property 0x801f = body part << 8 | id) to those names. An effect file
(ALO) names its textures at its end (after "AL_EFFECT"), found under `<code>/alo/texture/`; every fighter's folder
has its own copies. A full path there is not loaded, so the added fighter gets copies of the effect files it uses
(the donor's own and the definition's EFFECTS) and their textures in its pack's `alo/` folder, which the port serves
as `<code>/alo/...`.

Written for this project from the files themselves (no third-party code).
"""
import os

from ..ttt2.archive import key_of

ALO_END = b'AL_EFFECT\0'


def textures_of(alo):
    """The texture file names an effect file names (after AL_EFFECT)."""
    at = alo.find(ALO_END)
    if at < 0:
        return []
    return [t.decode('latin1') for t in alo[at + len(ALO_END):].split(b'\0') if t.endswith(b'.nut')]


def copy_files(archives, donor, donor_names, effects, log=None):
    """-> (pack files {'alo/<name>.alo' or 'alo/texture/<name>': bytes}, {effect id: [(bare name, bone)]} for
    effects, the donor's names that exist). donor_names: the donor's cmn_eff_default EffectGroup names; effects:
    {id: [(effect file path, bone or None)]}."""
    files, names, kept = {}, {}, []

    def take(src):
        """Copies effect file `src` and its textures; -> its bare name in the pack, or None."""
        if not archives.has(key_of(src)):
            return None
        base = os.path.splitext(os.path.basename(src))[0]
        target = 'alo/%s.alo' % base
        data = archives.read_name(src)
        if target in files and files[target] != data:          # two sources, one name: a longer one
            base = '%s_%s' % (base, src.split('/')[0])
            target = 'alo/%s.alo' % base
        files[target] = data
        folder = os.path.dirname(src)
        for t in textures_of(data):
            path = '%s/texture/%s' % (folder, t)
            if 'alo/texture/' + t not in files and archives.has(key_of(path)):
                files['alo/texture/' + t] = archives.read_name(path)
        return base

    for name in donor_names:                     # the donor's own effects (its generic moves fire them)
        if take('%s/alo/%s.alo' % (donor, name)) == name:
            kept.append(name)
    for effect_id, sources in sorted(effects.items()):
        for src, bone in sources:
            base = take(src)
            if base is None:
                if log:
                    log('effects: the game has no %s (left out)' % src)
                continue
            names.setdefault(effect_id, []).append((base, bone))
    if log:
        log('effects: %d files of the donor\'s, %d ids of its own (%d effect files, %d textures)' % (
            len(kept), len(names), sum(1 for f in files if not f.startswith('alo/texture/')),
            sum(1 for f in files if f.startswith('alo/texture/'))))
    return files, names, kept


def still_named(files, names):
    """The pack files of the effects in `names` (bare names) and the textures they name."""
    out = {}
    for path, data in files.items():
        if path.startswith('alo/texture/'):
            continue
        if os.path.splitext(os.path.basename(path))[0] in names:
            out[path] = data
            for t in textures_of(data):
                if 'alo/texture/' + t in files:
                    out['alo/texture/' + t] = files['alo/texture/' + t]
    return out
