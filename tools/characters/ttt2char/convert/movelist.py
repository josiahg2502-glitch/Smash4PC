"""A Tekken 7 fighter's command list -> a TTT2 command list (MVLT: the practice mode's list, `<code>.mvl`).

Both games keep a command list as one MVLT file: header {"MVLT", u16 version, u16, u32 entries, u32 demos,
u32 string block, u32 entries offset, u32 demos offset, u32 inputs offset}, then the strings (UTF-8, NUL-ended),
the entries, the demos and their inputs.
    Tekken 7 (as TKMovesets2 saves it, little-endian, version 1000/2): entry 372 bytes: 16 x u32 string offsets
    from the MVLT start ([0] Japanese name, [1] English name, [2] Japanese reading, [3] English name again,
    [4..15] the command in jp, en, pt, es, fr, it, de, mx, ru, ar, zh, ko), u32 @0x40, u16 first demo @0x44
    (0xFFFF none), u16 demo count, u16 kind @0x48 (8 title line, 10 sample combo, 14/15 Tekken 7's key-move and
    punishment drills), u16 demo kind, ..., u32 icons @0x14C (byte 1: the hit that screws); demo 24 bytes =
    TTT2's 20 + u16 "needs Rage" + u16; input 4 bytes {direction, buttons, frames, glyphs lit}.
    TTT2 (big-endian, version 2/0): entry 92 bytes: 12 x u32 string offsets from the string block ([0] jp name,
    [1] en name, [2] jp reading, [3..11] the command in jp, en, fr, it, de, es, ru, ko, zh), 4 x u32 0, u16 first
    demo, u16 demo count, u16 kind, u16 demo kind, 3 x u32 0, u16 icons, u16 icons (high byte: the hit that
    bounds), u32 0; demo 20 bytes {7 x u16 setup (opponent action, distance, angle, ...), u16 inputs, u32 first
    input}; input 4 bytes {buttons, direction, frames, glyphs lit}.
The command text uses the same private-use glyphs in both games (U+E000.. directions, U+E014 + mask buttons); demos
are pad inputs played through the fighter's own moveset, so they need no move ids.

Left out: Tekken 7's drills, entries whose demo needs Rage (Rage Arts and Drives), and title lines left empty.
Screws are bounds in the converted moveset; Tekken 7's screw hit number becomes TTT2's bound icon.
"""
import struct

T7_COMMAND_FOR_TTT2 = [4, 5, 8, 9, 10, 7, 12, 15, 14]   # TTT2's jp en fr it de es ru ko zh <- Tekken 7's index
KIND_TITLE, KIND_KEY_MOVE, KIND_DRILL = 8, 14, 15
ARROW = ''                                         # the arrow between the parts of a combo
OPPONENT_ACTION = {9: 10}                                # Tekken 7's "opponent attacks" (parries) -> TTT2's


def _cstr(d, o):
    return d[o:d.index(b'\0', o)].decode('utf-8')


def read_tekken7(m):
    """-> (entries, demos, inputs) of a Tekken 7 MVLT."""
    magic, version, _, n, ndemos, _, eo, do, io = struct.unpack_from('<4sHHIIIIII', m, 0)
    if magic != b'MVLT' or version != 1000:
        raise ValueError('not a Tekken 7 command list (MVLT 1000)')
    entries = []
    for i in range(n):
        b = eo + 372 * i
        demo, count, kind, demo_kind = struct.unpack_from('<HHHH', m, b + 0x44)
        entries.append(dict(strings=[_cstr(m, o) for o in struct.unpack_from('<16I', m, b)], demo=demo,
                            count=count, kind=kind, demo_kind=demo_kind,
                            icons=struct.unpack_from('<I', m, b + 0x14C)[0]))
    demos = []
    for i in range(ndemos):
        r = struct.unpack_from('<8HIHH', m, do + 24 * i)
        demos.append(dict(setup=list(r[:7]), inputs=r[7], first=r[8], rage=r[9]))
    inputs = [m[io + 4 * k:io + 4 * k + 4] for k in range((len(m) - io) // 4)]
    return entries, demos, inputs


def convert(m, log=print):
    """Tekken 7 MVLT bytes -> TTT2 MVLT bytes."""
    entries, demos, inputs = read_tekken7(m)
    out, out_demos, out_inputs = [], [], []
    dropped = {'drills': 0, 'needs Rage': 0, 'empty title lines': 0}
    for x in entries:
        s = x['strings']
        if x['kind'] in (KIND_KEY_MOVE, KIND_DRILL):
            dropped['drills'] += 1
            continue
        own = [demos[x['demo'] + k] for k in range(x['count'])] if x['demo'] != 0xFFFF else []
        if any(d['rage'] for d in own):
            dropped['needs Rage'] += 1
            continue
        commands = [s[k].replace(' ' + ARROW + ' ', ARROW) for k in T7_COMMAND_FOR_TTT2]
        if x['kind'] == KIND_TITLE:                      # TTT2 frames a title line and puts it in the name too
            def framed(t):
                return '    ■ ' + t + ' ■'
            names = [framed(commands[0]), framed(commands[1]), framed(commands[0])]
            commands = ['', ''] + [framed(c) for c in commands[2:]]
        else:
            names = s[:3]
        first = len(out_demos) if own else 0xFFFF
        for d in own:
            setup = list(d['setup'])
            setup[0] = OPPONENT_ACTION.get(setup[0], setup[0])
            out_demos.append((setup, d['inputs'], len(out_inputs)))
            for k in range(d['inputs']):
                r = inputs[d['first'] + k]
                out_inputs.append(bytes([r[1], r[0], r[2], r[3]]))
        screw = (x['icons'] >> 8) & 0xFF
        out.append(dict(strings=names + commands, demo=first, count=len(own), kind=x['kind'],
                        demo_kind=x['demo_kind'], icons=(0, screw << 8)))
    kept = []
    for j, x in enumerate(out):
        if x['kind'] == KIND_TITLE and (j + 1 >= len(out) or out[j + 1]['kind'] == KIND_TITLE):
            dropped['empty title lines'] += 1
            continue
        kept.append(x)
    log('command list: %d entries, %d demos (left out: %s)' % (
        len(kept), len(out_demos), ', '.join('%d %s' % (n, k) for k, n in dropped.items() if n) or 'none'))
    return write(kept, out_demos, out_inputs)


def write(entries, demos, inputs):
    strings, offsets = bytearray(), []
    for x in entries:
        o = []
        for s in x['strings']:
            o.append(len(strings))
            strings += s.encode('utf-8') + b'\0'
        offsets.append(o)
    while len(strings) % 4:
        strings.append(0)
    so = 0x20
    eo = so + len(strings)
    do = eo + 92 * len(entries)
    io = do + 20 * len(demos)
    out = bytearray(struct.pack('>4sHHIIIIII', b'MVLT', 2, 0, len(entries), len(demos), so, eo, do, io))
    out += strings
    for x, o in zip(entries, offsets):
        out += struct.pack('>12I4I', *o, 0, 0, 0, 0)
        out += struct.pack('>HHHH3IHHI', x['demo'], x['count'], x['kind'], x['demo_kind'], 0, 0, 0, *x['icons'], 0)
    for setup, n, first in demos:
        out += struct.pack('>8HI', *setup, n, first)
    for r in inputs:
        out += r
    return bytes(out)
