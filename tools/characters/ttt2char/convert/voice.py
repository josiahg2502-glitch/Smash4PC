"""A Tekken 7 fighter's voice -> the added fighter's four TTT2 sound banks.

Each bank is the donor's (`se_Chr_<donor>`, `se_Dwi_<donor>`, `rm_Dst_<donor>`, `rm_Dwi_<donor>`) with every tone's
audio replaced by
one of the Tekken 7 fighter's clips of the same kind, so the banks keep the layout the title expects of the donor:
  se_Chr: shouts (`atck`/`atk` short, medium, long), damage voices (`dmg`), KO screams (`ko`, `die`), special shouts
          (`org`, `itm`), the announcer calling the fighter's name (`syvo_name`)
  se_Dwi: win quotes;  rm_Dst: intro lines;  rm_Dwi: the long win quote one win pose streams
The Tekken 7 clips are Wwise events of its voice bank `AkBankVoice<CODE>.bnk` named `<code>_atk_s01`, `_dmg_m02`,
`_ko01`, `_org03`, `<prefix>_s00` (intros), `<prefix>_w00` (wins); the announcer's call is `syvo_name_<code>` in
`AkBankBattle.bnk`. Events are found by name (Wwise ids are hashes of the names).

Moves play a shout by its number n (voice clip 2 << 24 | n): the n-th entry of category 5 (mask 0x20) in the fighter's
voice list (the title's table by select id: entries {sound id, mask}; the sound table gives each id's tone), so
se_Chr's tones are filled by that list, not by their names: shout n gets the Tekken 7 fighter's shout n (Tekken 7
numbers them alike: `atk` short, medium, long, then `org`), the damage voices (0x40, numbered from the list's end)
and KO screams (0x80) its clips of those kinds. Shouts past the donor's count become tones appended to se_Chr, which the
port adds to the fighter's voice list (character.ini `voice_extra`).

The select screen's announcer calls are tones of one shared bank, se_Slt_StartChar: a link tone
`sel_syvo_name_<code>` per fighter plays its audio tones `lnk_sel_syvo_name_<code>[_Copy]`. name_call() appends the
added fighter's three (the runtime gives its select id the link tone's sound).
"""
import re
import struct

import numpy as np

from ..tekken7.wwise import Bank as WwiseBank, decode_wem
from ..ttt2 import dsp
from ..ttt2.nus3bank import Bank, wave

VOICE_BANK = 'TekkenGame/Content/WwiseAudio/Windows/AkBankVoice%s.bnk'
BATTLE_BANK = 'TekkenGame/Content/WwiseAudio/Windows/AkBankBattle.bnk'
# the ids the added fighter's banks get in the sound project (BINF): the sound library refuses a bank whose id a
# loaded bank has (error 0x80000021: a tag partner's or the opponent's bank with the first added fighter's id stayed
# silent), so each added fighter's are its own, above TTT2's (0..461): 1000 + 4 * its number + the bank's kind
PROJECT_ID_BASE = 1000
BANK_KINDS = ('se_Chr', 'se_Dwi', 'rm_Dst', 'rm_Dwi')


def project_id(which, slot):
    return PROJECT_ID_BASE + 4 * slot + BANK_KINDS.index(which)
# sample rates tried in turn: the first that keeps a bank no bigger than the donor's (the title's sound memory is
# sized for its own banks)
RATES = (48000, 32000, 24000)
# The title's tables (code/Tekken.rpx): fighters' codes, voice lists and the sound table
CODES = 0x107C57F4              # 63 x char*: each fighter's code, by select id
VOICE_LISTS = 0x107D44EC        # 63 x {entries*, count, fighter}; an entry 16 B {0, sound id, 0x4000 | k, u16 mask, 0}
SOUND_TABLE = 0x107D4AC0        # by sound id {tone, bank id, group, flags}
KIAI, DAMAGE, KO = 0x20, 0x40, 0x80


def tone_kind(name):
    """A donor tone's kind: (category, size) with category atk, dmg, ko, org, name, or None."""
    n = name.lower()
    if 'syvo_name' in n:
        return ('name', None)
    m = re.search(r'_(?:atck|atk|attack)_?([sml])?\d', n)
    if m:
        return ('atk', m.group(1))
    if re.search(r'_(?:dmg_|damage_)?(?:ko|die)_?\d', n):
        return ('ko', None)
    m = re.search(r'_(?:dmg|damage)_?([sml])?\d', n)
    if m:
        return ('dmg', m.group(1))
    if re.search(r'_org_?\d|itm_|_\d\d$', n):
        return ('org', None)
    return (None, None)


def voice_list(rpx, donor):
    """The donor's voice list: [(mask, its se_Chr tone)] in order. rpx: the title's executable (ttt2.rpx.Rpx)."""
    codes = [rpx.read(rpx.u32(CODES + 4 * i), 4).split(b'\0')[0].decode('latin1') for i in range(63)]
    entries, count, _ = struct.unpack('>3I', rpx.read(VOICE_LISTS + 12 * codes.index(donor), 12))
    out = []
    for j in range(count):
        sid = rpx.u32(entries + 16 * j + 4)
        mask, = struct.unpack('>H', rpx.read(entries + 16 * j + 12, 2))
        out.append((mask, rpx.u32(SOUND_TABLE + 16 * sid)))
    return out


class Clips:
    """The Tekken 7 fighter's clips by kind: {(category, size): [(event name, rate, int16 [channels, samples])]}."""

    def __init__(self, paks, code, prefix):
        self.bank = WwiseBank(paks.read(VOICE_BANK % code.upper()))
        self.kinds = {}
        c = code.lower()
        # the fighters' banks spell their events two ways: kzm_atk_s01, kzm_ko01 / elz_atck_s_01, elz_dmg_ko_01
        for cat, spellings in (('atk', ('atk', 'atck')), ('dmg', ('dmg',))):
            for size in 'sml':
                self._collect((cat, size), ['%s_%s_%s%s%02d' % (c, word, size, sep, k) for word in spellings
                                            for sep in ('', '_') for k in range(1, 21)])
        self._collect(('ko', None), ['%s_%s%02d' % (c, word, k) for word in ('ko', 'ko_', 'dmg_ko_')
                                     for k in range(1, 11)])
        self._collect(('org', None), ['%s_org%s%02d' % (c, sep, k) for sep in ('', '_') for k in range(1, 21)])
        self._collect(('intro', None), ['%s_s%02d' % (prefix, k) for k in range(0, 20)])
        self._collect(('win', None), ['%s_w%02d' % (prefix, k) for k in range(0, 20)])
        try:
            battle = WwiseBank(paks.read(BATTLE_BANK))
            name = 'syvo_name_%s' % c
            if battle.has_event(name):
                media = battle.event_media(name)
                if media:
                    rate, a = decode_wem(battle.media[media[0]])
                    self.kinds[('name', None)] = [(name, rate, a)]
        except (KeyError, OSError, ValueError):
            pass

    def _collect(self, kind, names):
        out = []
        for n in names:
            if not self.bank.has_event(n):
                continue
            media = self.bank.event_media(n)
            if media:
                rate, a = decode_wem(self.bank.media[media[0]])
                out.append((n, rate, a))
        if out:
            self.kinds[kind] = out

    def kiai(self):
        """The shouts in Tekken 7's numbering (a move's shout n is the n-th): short, medium, long, then `org`."""
        return [c for size in 'sml' for c in self.kinds.get(('atk', size), [])] + self.kinds.get(('org', None), [])

    def damage(self, sizes):
        """The damage voices of the sizes, in that order of preference."""
        return [c for size in sizes for c in self.kinds.get(('dmg', size), [])]

    def pick(self, kind, counter):
        """The next clip of a kind (round robin), falling back to the nearest size."""
        cat, size = kind
        order = [size] + [s for s in ('m', 's', 'l', None) if s != size] if cat in ('atk', 'dmg') else [size]
        for s in order:
            lst = self.kinds.get((cat, s))
            if lst:
                k = counter.get((cat, s), 0)
                counter[(cat, s)] = k + 1
                return lst[k % len(lst)]
        return None


def _resample(a, rate, to):
    if rate == to:
        return a
    n = int(round(a.shape[1] * to / rate))
    x = np.linspace(0, a.shape[1] - 1, n)
    return np.stack([np.interp(x, np.arange(a.shape[1]), ch).round().astype(np.int16) for ch in a])


def _fit(a, channels):
    """int16 [channels, samples] with the wanted channel count (mixing down or duplicating)."""
    if a.shape[0] == channels:
        return a
    if channels == 1:
        return a.astype(np.int32).mean(0, keepdims=True).round().astype(np.int16)
    return np.repeat(a[:1], channels, 0)


def _audio(original, clip, channels, to, side=None):
    """Audio bytes in the tone's own container (IDSP or WAVE) -> (bytes, rate, channels, samples). side: 'L' or 'R'
    for a tone that plays one channel of a stereo pair."""
    _, rate, a = clip
    if side is not None and a.shape[0] == 2:
        a = a[1:2] if side == 'R' else a[0:1]
    a = _fit(_resample(a, rate, to), channels)
    if original[:4] == b'IDSP':
        return dsp.idsp(list(a), to), to, channels, a.shape[1]
    return wave(list(a), to), to, channels, a.shape[1]


def build(clips, donor_banks, code, vlist, slot):
    """donor_banks: {'se_Chr': bytes, 'se_Dwi': bytes, 'rm_Dst': bytes, 'rm_Dwi': bytes}; vlist: the donor's voice
    list (voice_list); slot: the fighter's number (its banks' project ids) -> ({file name: bytes}, report lines, win quotes {Tekken 7 event name: the se_Dwi tone that
    plays it (moves play win quotes by tone)}, the shouts' tones appended to se_Chr (its voice list's extra entries:
    shouts number len(donor's shouts) on)). se_Chr follows the voice list; the other banks take clips in turn from
    one round of each kind (rm_Dwi gets a win quote se_Dwi has not)."""
    out, report, wins, extra = {}, [], {}, []
    counter = {}
    for which, data in donor_banks.items():
        for rate in RATES:
            start = dict(counter)
            written, used, placed, extra_here = _build_one(which, data, clips, code, rate, start,
                                                           vlist if which == 'se_Chr' else None, slot)
            # (the title's sound memory is sized for its own banks; appended shouts may add to it)
            if len(written) <= len(data) * 1.02 * (1 + len(extra_here) / max(1, len(vlist))):
                break
        counter = start
        out['%s_%s.nus3bank' % (which, code)] = written
        if which == 'se_Dwi':
            wins = placed
        if which == 'se_Chr':
            extra = extra_here
        report.append('%s_%s: %d tones of its own%s, %d Hz, %d KB (the donor\'s %d KB)'
                      % (which, code, len(used), ' (%d shouts appended)' % len(extra_here) if extra_here else '',
                         rate, len(written) // 1024, len(data) // 1024))
    return out, report, wins, extra


def _build_one(which, data, clips, code, to, counter, vlist=None, slot=0):
    """The donor's bank `which` with the clips at sample rate `to` -> (bytes, the tones replaced, {clip name: the
    tone that plays it}, the shouts' tones appended). A link tone's tones (a stereo pair `..R`/`..L`, or variants)
    get one clip together. counter: how many clips of each kind earlier banks took (advanced here). vlist: the
    donor's voice list (se_Chr): its tones first, by the list."""
    bank = Bank(data)
    used, placed, extra = [], {}, []

    def kind_of(name):
        if which == 'se_Chr':
            return tone_kind(name)
        return ('intro', None) if which == 'rm_Dst' else ('win', None)

    def put(i, clip, side=None):
        _, channels, _ = bank.format(i)
        audio, rate, ch, n = _audio(bank.audio[i], clip, channels, to, side)
        bank.replace(i, audio, rate, ch, n)
        used.append('%s<-%s' % (bank.name(i), clip[0]))

    done = set()

    def give(i, clip):
        """Tone i (or a link tone's tones) plays the clip."""
        if bank.has_audio(i):
            put(i, clip)
            done.add(i)
        else:
            for _, k in bank.links(i):
                put(k, clip, bank.name(k)[-1] if bank.name(k)[-1] in 'LR' else None)
                done.add(k)
        done.add(i)
        placed.setdefault(clip[0], i)

    if vlist:
        kiai = clips.kiai()
        shouts = [tone for mask, tone in vlist if mask & KIAI]
        for n, tone in enumerate(shouts):
            if kiai and tone not in done:
                give(tone, kiai[n % len(kiai)])
        template = next((t for t in reversed(shouts) if bank.has_audio(t)), None)
        for n in range(len(shouts), len(kiai) if template is not None else 0):
            old = bank.name(template)
            new = bank.append(bank.renamed(template, old[:-2] + '%02d' % (n % 100)), audio=bank.audio[template])
            give(new, kiai[n])
            extra.append(new)
        # damage voice n is the (count - 1 - n)-th of category 6 in both games (sub_0796F218 counts it from the
        # end): Tekken 7's (short, medium, long) are lined up with the donor's last ones
        hurt, slots = clips.damage('sml'), [tone for mask, tone in vlist if mask & DAMAGE and not mask & KIAI]
        for j, tone in enumerate(slots):
            if hurt and tone not in done:
                give(tone, hurt[(j - len(slots) + len(hurt)) % len(hurt)])
        for mask, tone in vlist:
            if tone in done or mask & (KIAI | DAMAGE):
                continue
            if mask & KO:
                clip = clips.pick(('ko', None), counter)
                if clip is not None:
                    give(tone, clip)

    for i in range(len(bank.tones)):
        name = bank.name(i)
        if i in done:
            continue
        links = bank.links(i) if name and not bank.has_audio(i) else []
        if not links or kind_of(name)[0] is None:
            continue
        clip = clips.pick(kind_of(name), counter)
        if clip is None:
            continue
        for _, k in links:
            side = bank.name(k)[-1] if bank.name(k)[-1] in 'LR' else None
            put(k, clip, side)
            done.add(k)
        placed.setdefault(clip[0], i)
    for i in range(len(bank.tones)):
        name = bank.name(i)
        if i in done or not name or not bank.has_audio(i) or not bank.audio[i] or kind_of(name)[0] is None:
            continue
        clip = clips.pick(kind_of(name), counter)
        if clip is None:
            continue
        put(i, clip)
        placed.setdefault(clip[0], i)
    bank.rename('%s_%s' % (which, code), project_id(which, slot))
    return bank.write(), used, placed, extra


def merge(chr_bank, dwi_bank):
    """One bank with se_Chr's tones and then se_Dwi's (the port gives each added fighter one bank id in a match:
    roster.cpp plays se_Dwi's tone n as n + the se_Chr tone count) -> (bytes, se_Chr's tone count)."""
    out, dwi = Bank(chr_bank), Bank(dwi_bank)
    first = len(out.tones)
    shared = {}                                    # a se_Dwi audio source -> the merged tone that holds it
    for i, tone in enumerate(dwi.tones):
        entry = bytearray(tone)
        if dwi.name(i) and not dwi.has_audio(i):
            for offset, k in dwi.links(i):
                struct.pack_into('<I', entry, offset, first + k)
        if dwi.has_audio(i) and dwi.audio[i] is not None:
            if dwi.source[i] in shared:
                out.append(entry, share=shared[dwi.source[i]])
            else:
                shared[dwi.source[i]] = out.append(entry, audio=dwi.audio[i])
        else:
            out.append(entry)
    return out.write(), first


SELECT_BANK = 'se_Slt_StartChar.nus3bank'


def name_call(clips, data, donor, code):
    """The select screen's announcer bank with the fighter's name call added (a copy of the donor's link tone and
    its audio tones with the fighter's name) -> bytes, or None without a name clip."""
    clip = clips.kinds.get(('name', None))
    if not clip:
        return None
    bank = Bank(data)
    names = {bank.name(i): i for i in range(len(bank.tones))}
    link = names.get('sel_syvo_name_%s' % donor)
    if link is None or not bank.links(link):
        return None
    entry = bank.renamed(link, 'sel_syvo_name_%s' % code)
    for offset, k in bank.links(link):
        rate, channels, _ = bank.format(k)
        audio, rate, ch, n = _audio(bank.audio[k], clip[0], channels, rate)
        new = bank.append(bank.renamed(k, bank.name(k).replace('_' + donor, '_' + code)), audio=audio)
        bank.replace(new, audio, rate, ch, n)
        struct.pack_into('<I', entry, offset, new)
    bank.append(entry)
    return bank.write()
