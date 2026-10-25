"""Movesets of Tekken 7 (as TKMovesets2 stores them: 64-bit little-endian) and TTT2 (the Wii U motbin: 32-bit
big-endian) in one form: 19 tables of rows (dicts with the same field names in both games), links between tables as
row indices (None: no link), move and animation names as offsets into a string block.

A field is (name, offset, struct code without byte order, link): 'Q' is a Tekken 7 64-bit pointer slot; link names
the table the field indexes ('names': a string offset, 'anim': an animation reference, 'move': a move id, which goes
through the alias table from 0x8000 on; None: a plain value).
"""
import struct

TABLE_ORDER = ['reactions', 'requirements', 'hit_conditions', 'projectiles', 'pushbacks', 'pushback_extras',
               'cancels', 'group_cancels', 'cancel_extras', 'extra_move_properties', 'move_begin_props',
               'move_end_props', 'moves', 'voiceclips', 'input_sequences', 'inputs', 'parry_related',
               'camera_data', 'throw_cameras']

def _reactions(p, w):
    """p: the pointer struct code, w: its width."""
    f = [('pb_front', 0, p, 'pushbacks'), ('pb_back', w, p, 'pushbacks'), ('pb_left', 2 * w, p, 'pushbacks'),
         ('pb_right', 3 * w, p, 'pushbacks'), ('pb_front_ch', 4 * w, p, 'pushbacks'), ('pb_downed', 5 * w, p, 'pushbacks'),
         ('pb_block', 6 * w, p, 'pushbacks')]
    o = 7 * w
    for k, n in enumerate(['dir_front', 'dir_back', 'dir_left', 'dir_right', 'dir_front_ch', 'dir_downed']):
        f.append((n, o + 2 * k, 'H', None))
    o += 12
    f += [('r_int1', o, 'I', None), ('r_int2', o + 4, 'I', None)]
    o += 8
    for k, n in enumerate(['vertical_pushback', 'standing', 'default', 'crouch', 'ch', 'crouch_ch', 'left',
                           'crouch_left', 'right', 'crouch_right', 'back', 'crouch_back', 'block', 'crouch_block',
                           'wallslump', 'downed']):
        f.append((n, o + 2 * k, 'H', 'move' if k else None))
    return f

REACTION_MOVE_FIELDS = ['standing', 'default', 'crouch', 'ch', 'crouch_ch', 'left', 'crouch_left', 'right',
                        'crouch_right', 'back', 'crouch_back', 'block', 'crouch_block', 'wallslump', 'downed']

T7 = {
    'reactions': (0x70, _reactions('Q', 8) + [('r_6c', 0x6C, 'I', None)]),
    'requirements': (0x8, [('condition', 0, 'I', None), ('param', 4, 'I', None)]),
    'hit_conditions': (0x18, [('req', 0, 'Q', 'requirements'), ('damage', 8, 'I', None), ('h_0c', 0xC, 'I', None),
                              ('reactions', 0x10, 'Q', 'reactions')]),
    'projectiles': (0xA8, [('raw', 0, '168s', None), ('hit_condition', 0x60, 'Q', 'hit_conditions'),
                           ('cancel', 0x68, 'Q', 'cancels')]),
    'pushbacks': (0x10, [('duration', 0, 'H', None), ('displacement', 2, 'H', None), ('loops', 4, 'I', None),
                         ('extradata', 8, 'Q', 'pushback_extras')]),
    'pushback_extras': (0x2, [('value', 0, 'H', None)]),
    'cancels': (0x28, [('direction', 0, 'I', None), ('button', 4, 'I', None), ('req', 8, 'Q', 'requirements'),
                       ('extradata', 0x10, 'Q', 'cancel_extras'), ('win_start', 0x18, 'I', None),
                       ('win_end', 0x1C, 'I', None), ('start_frame', 0x20, 'I', None), ('move_id', 0x24, 'H', 'move'),
                       ('option', 0x26, 'H', None)]),
    'cancel_extras': (0x4, [('value', 0, 'I', None)]),
    'extra_move_properties': (0xC, [('frame', 0, 'I', None), ('id', 4, 'I', None), ('value', 8, 'I', None)]),
    'move_begin_props': (0x10, [('req', 0, 'Q', 'requirements'), ('id', 8, 'I', None), ('value', 0xC, 'I', None)]),
    'moves': (0xB0, [('name', 0, 'Q', 'names'), ('anim_name', 8, 'Q', 'names'), ('anim', 0x10, 'Q', 'anim'),
                     ('vuln', 0x18, 'I', None), ('hitlevel', 0x1C, 'I', None), ('cancel', 0x20, 'Q', 'cancels'),
                     ('cancel2', 0x28, 'Q', 'cancels'), ('c2_int', 0x30, 'I', None), ('m_34', 0x34, 'I', None),
                     ('cancel3', 0x38, 'Q', 'cancels'), ('c3_int', 0x40, 'I', None), ('m_44', 0x44, 'I', None),
                     ('cancel4', 0x48, 'Q', 'cancels'), ('c4_int', 0x50, 'I', None), ('transition', 0x54, 'H', 'move'),
                     ('m_56', 0x56, 'H', None), ('move_id_lo', 0x58, 'H', None), ('move_id_hi', 0x5A, 'H', None),
                     ('m_5c', 0x5C, 'H', None), ('m_5e', 0x5E, 'H', None),
                     ('hit_condition', 0x60, 'Q', 'hit_conditions'), ('anim_len', 0x68, 'I', None),
                     ('airborne_start', 0x6C, 'I', None), ('airborne_end', 0x70, 'I', None),
                     ('ground_fall', 0x74, 'I', None), ('voiceclip', 0x78, 'Q', 'voiceclips'),
                     ('extraprops', 0x80, 'Q', 'extra_move_properties'), ('begin_props', 0x88, 'Q', 'move_begin_props'),
                     ('end_props', 0x90, 'Q', 'move_end_props'), ('bitfield', 0x98, 'I', None),
                     ('hitbox', 0x9C, '4s', None), ('first_active', 0xA0, 'I', None), ('last_active', 0xA4, 'I', None),
                     ('m_a8', 0xA8, 'H', None), ('distance', 0xAA, 'H', None), ('m_ac', 0xAC, 'I', None)]),
    'voiceclips': (0x4, [('value', 0, 'I', None)]),
    'input_sequences': (0x10, [('window', 0, 'H', None), ('amount', 2, 'H', None), ('s_04', 4, 'I', None),
                               ('inputs', 8, 'Q', 'inputs')]),
    'inputs': (0x8, [('direction', 0, 'I', None), ('button', 4, 'I', None)]),
    'parry_related': (0x4, [('value', 0, 'I', None)]),
    'camera_data': (0xC, [('c_0', 0, 'I', None), ('c_4', 4, 'H', None), ('left', 6, 'H', None), ('right', 8, 'H', None),
                          ('c_a', 0xA, 'H', None)]),
    'throw_cameras': (0x10, [('side', 0, 'Q', None), ('camera_data', 8, 'Q', 'camera_data')]),
}
T7['group_cancels'] = T7['cancels']
T7['move_end_props'] = T7['move_begin_props']

TTT2 = {
    'reactions': (0x50, _reactions('I', 4)),
    'requirements': (0x8, [('condition', 0, 'I', None), ('param', 4, 'I', None)]),
    'hit_conditions': (0xC, [('req', 0, 'I', 'requirements'), ('damage', 4, 'I', None), ('reactions', 8, 'I', 'reactions')]),
    'projectiles': (0x88, [('raw', 0, '136s', None)]),
    'pushbacks': (0xC, [('duration', 0, 'H', None), ('displacement', 2, 'H', None), ('loops', 4, 'I', None),
                        ('extradata', 8, 'I', 'pushback_extras')]),
    'pushback_extras': (0x2, [('value', 0, 'H', None)]),
    'cancels': (0x20, [('direction', 0, 'I', None), ('button', 4, 'I', None), ('req', 8, 'I', 'requirements'),
                       ('extradata', 0xC, 'I', 'cancel_extras'), ('win_start', 0x10, 'I', None),
                       ('win_end', 0x14, 'I', None), ('start_frame', 0x18, 'I', None), ('move_id', 0x1C, 'H', 'move'),
                       ('option', 0x1E, 'H', None)]),
    'cancel_extras': (0x4, [('value', 0, 'I', None)]),
    'extra_move_properties': (0xC, [('frame', 0, 'I', None), ('id', 4, 'I', None), ('value', 8, 'I', None)]),
    'move_begin_props': (0xC, [('req', 0, 'I', 'requirements'), ('id', 4, 'I', None), ('value', 8, 'I', None)]),
    'moves': (0x70, [('name', 0, 'I', 'names'), ('anim_name', 4, 'I', 'names'), ('anim', 8, 'I', 'anim'),
                     ('vuln', 0xC, 'I', None), ('hitlevel', 0x10, 'I', None), ('cancel', 0x14, 'I', 'cancels'),
                     ('cancel2', 0x18, 'I', 'cancels'), ('c2_int', 0x1C, 'I', None), ('cancel3', 0x20, 'I', 'cancels'),
                     ('c3_int', 0x24, 'I', None), ('cancel4', 0x28, 'I', 'cancels'), ('c4_int', 0x2C, 'I', None),
                     ('transition', 0x30, 'H', 'move'), ('m_56', 0x32, 'H', None), ('move_id_hi', 0x34, 'H', None),
                     ('move_id_lo', 0x36, 'H', None), ('hit_condition', 0x38, 'I', 'hit_conditions'),
                     ('anim_len', 0x3C, 'I', None), ('airborne_start', 0x40, 'I', None), ('airborne_end', 0x44, 'I', None),
                     ('ground_fall', 0x48, 'I', None), ('voiceclip', 0x4C, 'I', 'voiceclips'),
                     ('extraprops', 0x50, 'I', 'extra_move_properties'), ('begin_props', 0x54, 'I', 'move_begin_props'),
                     ('end_props', 0x58, 'I', 'move_end_props'), ('bitfield', 0x5C, 'I', None),
                     ('hitbox', 0x60, '4s', None), ('first_active', 0x64, 'I', None), ('last_active', 0x68, 'I', None),
                     ('m_a8', 0x6C, 'H', None), ('distance', 0x6E, 'H', None)]),
    'voiceclips': (0x4, [('value', 0, 'I', None)]),
    'input_sequences': (0x8, [('s_00', 0, 'B', None), ('window', 1, 'B', None), ('amount', 2, 'H', None),
                              ('inputs', 4, 'I', 'inputs')]),
    'inputs': (0x8, [('direction', 0, 'I', None), ('button', 4, 'I', None)]),
    'parry_related': (0x4, [('value', 0, 'I', None)]),
    'camera_data': (0xC, [('c_0', 0, 'I', None), ('c_4', 4, 'H', None), ('left', 6, 'H', None), ('right', 8, 'H', None),
                          ('c_a', 0xA, 'H', None)]),
    'throw_cameras': (0x8, [('side', 0, 'I', None), ('camera_data', 4, 'I', 'camera_data')]),
}
TTT2['group_cancels'] = TTT2['cancels']
TTT2['move_end_props'] = TTT2['move_begin_props']

# pointer-like links that are table indices in both files (0xFFFF.. = none)
INDEX_LINKS = {'requirements', 'hit_conditions', 'reactions', 'pushbacks', 'pushback_extras', 'cancels',
               'cancel_extras', 'extra_move_properties', 'move_begin_props', 'move_end_props', 'voiceclips', 'inputs',
               'camera_data'}


class Moveset:
    """tables[name] = list of rows; game = 't7' or 'ttt2'; aliases = the 56 alias slots (move id 0x8000 + k -> move
    aliases[k]); namebuf = the string block."""

    def name(self, off):
        return self.namebuf[off:self.namebuf.index(b'\0', off)].decode('latin1')

    def move_name(self, i):
        return self.name(self.tables['moves'][i]['name'])

    def anim_name(self, i):
        return self.name(self.tables['moves'][i]['anim_name'])

    def resolve_alias(self, mid):
        if mid >= 0x8000:
            k = mid - 0x8000
            return self.aliases[k] if k < len(self.aliases) else None
        return mid

    def iter_list(self, table, start, end_test):
        """A list inside a table: rows from start up to and including the first one end_test accepts."""
        t = self.tables[table]
        i = start
        while i is not None and i < len(t):
            yield i, t[i]
            if end_test(t[i]):
                return
            i += 1


def read_table(d, base, n, layout, endian):
    size, fields = layout
    out = []
    for i in range(n):
        o = base + i * size
        e = {}
        for nm, off, fmt, link in fields:
            v = struct.unpack_from(endian + fmt, d, o + off)[0]
            if link in INDEX_LINKS and ((fmt == 'Q' and v == 0xFFFFFFFFFFFFFFFF) or (fmt == 'I' and v == 0xFFFFFFFF)):
                v = None
            e[nm] = v
        out.append(e)
    return out


def pack_table(rows, layout, endian='>'):
    size, fields = layout
    out = bytearray(size * len(rows))
    for i, e in enumerate(rows):
        for nm, off, fmt, link in fields:
            v = e[nm]
            if v is None:
                v = 0xFFFFFFFF if fmt == 'I' else 0xFFFF
            struct.pack_into(endian + fmt, out, i * size + off, v)
    return bytes(out)
