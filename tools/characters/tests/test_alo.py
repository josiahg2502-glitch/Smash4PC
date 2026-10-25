"""Unit tests of the ALO (effect file) reader and writer on made-up effects (no game files needed).

    python3 -m unittest discover tools/characters/tests
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.ttt2 import alo                          # noqa: E402


def _u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def _u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def _sample():
    """Two emitters (a mover linking a sprite), a texture animation, a modifier, a shape and a parameter."""
    doc = alo.new()
    mover = alo.new_emitter()
    sprite = alo.new_emitter()
    sp = sprite['spawner']
    sp['tracks'][alo.ALPHA_TRACK]['keys'] = [[0, 1.0], [7, 0.5], [14, 0.0]]
    sp['tracks'][alo.SIZE_TRACK]['keys'] = [[0, 0.4], [14, 0.15]]
    raw = bytearray(sp['raw'])
    struct.pack_into('>H', raw, alo.LIFE, 15)
    sp['raw'] = bytes(raw)
    frames = []
    for k in range(4):
        f = bytearray(alo.FRAME_SIZE)
        struct.pack_into('>Hxxff', f, 0, 1, 0.25 * k, 0.0)
        frames.append({'raw': bytes(f), 'texture': 'tfm_test00.nut'})
    lay = bytearray(alo.LAYER_SIZE)
    lay[12] = 1
    sp['layers'] = [{'raw': bytes(lay), 'anim': {'raw': bytes(alo.ANIM_SIZE), 'frames': frames,
                                                 'names': [{'raw': bytes(alo.NAME_SIZE), 'name': 'tfm_test00.nut'}]},
                     'uvs': [{'raw': bytes(alo.UV_SIZE), 'name': 'tfm_test00.nut',
                              'tracks': {8: alo._track('pair', [[0, 0.5, 0.5]]), 20: alo._track('pair')}}]},
                    {'raw': bytes(alo.LAYER_SIZE), 'still': struct.pack('>fhh', 1.0, 2, 3), 'uvs': []}]
    mod = bytearray(alo.MODIFIER_SIZE)
    mod[4] = 1
    data = bytearray(32)
    struct.pack_into('>f', data, 12, 9.8)
    sp['modifiers'] = [{'raw': bytes(mod), 'data': bytes(data), 'tracks': {0: alo._track('scalar', [[0, 0.0]])}}]
    pa = sprite['particle']
    raw = bytearray(pa['raw'])
    raw[175] = 2
    pa['raw'] = bytes(raw)
    pa['shape'] = {'raw': bytes(56), 'tracks': {o: alo._track('scalar', [[0, 6.25]]) for o in (0, 12, 24, 36)}}
    param = bytearray(alo.PARAM_SIZE)
    param[12] = 2
    sprite['params'] = [{'raw': bytes(param), 'data': struct.pack('>Ifh2x', 0xFF804730, 1500.0, 30)}]
    alo.add_emitter(doc, mover)
    alo.add_emitter(doc, sprite)
    alo.link(doc, 0, 1)
    doc['textures'] = [{'raw': bytes(alo.REF_SIZE), 'name': 'tfm_test00.nut'}]
    return doc


class Layout(unittest.TestCase):
    def test_header_and_pool(self):
        b = alo.write(_sample())
        self.assertEqual(b[:4], b'OELA')
        self.assertEqual(_u32(b, 4), 10)
        self.assertEqual(_u32(b, 200), 1)
        self.assertEqual(b[124], 2)                     # emitters
        self.assertEqual(b[126], 1)                     # textures
        pool = _u32(b, 104)
        self.assertEqual(b[pool:pool + 10], b'AL_EFFECT\0')
        name = _u32(b, _u32(b, 96) + 4)                 # the texture name: a pool offset
        self.assertEqual(b[pool + name:pool + name + 15], b'tfm_test00.nut\0')

    def test_offsets_are_relative_to_their_blocks(self):
        b = alo.write(_sample())
        node = _u32(b, 56) + alo.NODE_SIZE              # emitter 1
        sp, pa = _u32(b, node + 16), _u32(b, node + 24)
        alpha = sp + alo.ALPHA_TRACK                    # track reference: offset from the spawner, count
        self.assertEqual(_u16(b, alpha + 4), 3)
        keys = sp + _u32(b, alpha)
        self.assertEqual(struct.unpack_from('>Hxxf', b, keys + 8), (7, 0.5))
        shape = pa + _u32(b, pa)                        # shape data: offset from the particle block
        self.assertEqual(_u16(b, shape + 4), 1)         # its first track (keys: offset from the particle block)
        self.assertEqual(struct.unpack_from('>f', b, pa + _u32(b, shape) + 4)[0], 6.25)
        mover = _u32(b, 56)                             # the mover's link names emitter 1 by its file offset
        self.assertEqual(_u16(b, mover + 72), 1)
        self.assertEqual(_u32(b, _u32(b, mover + 4)), node)
        self.assertTrue(all(_u32(b, o) % 16 == 0 for o in (56, 96, 104)))


class RoundTrip(unittest.TestCase):
    def test_read_back(self):
        doc = _sample()
        b = alo.write(doc)
        d2 = alo.read(b)
        self.assertEqual(len(d2['emitters']), 2)
        self.assertEqual(d2['emitters'][0]['links'][0]['emitter'], 1)
        sp = d2['emitters'][1]['spawner']
        self.assertEqual(sp['tracks'][alo.ALPHA_TRACK]['keys'], [[0, 1.0], [7, 0.5], [14, 0.0]])
        self.assertEqual([f['texture'] for f in sp['layers'][0]['anim']['frames']], ['tfm_test00.nut'] * 4)
        self.assertEqual(sp['layers'][1]['still'], struct.pack('>fhh', 1.0, 2, 3))
        self.assertEqual(sp['modifiers'][0]['tracks'][0]['keys'], [[0, 0.0]])
        self.assertEqual(d2['emitters'][1]['particle']['shape']['tracks'][36]['keys'], [[0, 6.25]])
        self.assertEqual(d2['emitters'][1]['params'][0]['data'][:4], b'\xff\x80\x47\x30')
        self.assertEqual(d2['extra'], [])

    def test_identical_bytes(self):
        b = alo.write(_sample())
        self.assertEqual(alo.write(alo.read(b)), b)

    def test_places_and_leftovers_are_kept(self):
        b = bytearray(alo.write(_sample()))
        node = _u32(b, 56)
        b[node + 20:node + 24] = b'\x12\x34\x56\x78'    # a value the loader ignores, kept in the record
        gap = _u32(b, 104) - 4                          # a stray byte between blocks
        self.assertEqual(b[gap], 0)
        b[gap] = 0x5A
        doc = alo.read(bytes(b))
        self.assertEqual(doc['extra'], [(gap, b'\x5a')])
        self.assertEqual(alo.write(doc), bytes(b))

    def test_padding_and_empty_list_offsets(self):
        b = bytearray(alo.write(_sample()))
        node = _u32(b, 56) + alo.NODE_SIZE
        sp = _u32(b, node + 16)
        keys = sp + _u32(b, sp + alo.ALPHA_TRACK)
        b[keys + 2:keys + 4] = b'\x00\x07'              # a key's spare bytes
        self.assertEqual(alo.write(alo.read(bytes(b))), bytes(b))

    def test_fresh_layout(self):
        doc = alo.read(alo.write(_sample()))
        sp = doc['emitters'][1]['spawner']
        sp['tracks'][alo.ALPHA_TRACK]['keys'].append([20, 0.0])      # grows: needs a new place
        sp['tracks'][alo.ALPHA_TRACK].pop('at')
        b = alo.write(doc)
        d2 = alo.read(b)
        self.assertEqual(d2['emitters'][1]['spawner']['tracks'][alo.ALPHA_TRACK]['keys'][-1], [20, 0.0])
        b2 = alo.write(alo.unpin(d2))
        self.assertEqual(alo.read(b2)['emitters'][1]['spawner']['tracks'][alo.SIZE_TRACK]['keys'],
                         [[0, 0.4000000059604645], [14, 0.15000000596046448]])

    def test_pool_stays_last_when_a_block_grows(self):
        b = alo.write(_sample())
        doc = alo.read(b)
        pool = doc['pool']
        t = doc['emitters'][1]['spawner']['tracks'][alo.ALPHA_TRACK]
        alo._set_keys(t, t['keys'] + [[20, 0.0]])      # grows in a pinned file: the keys need a new place
        b2 = alo.write(doc)
        d2 = alo.read(b2)
        self.assertEqual(d2['pool'], pool)              # nothing but the names (the keys are not swallowed)
        self.assertEqual(len(b2), _u32(b2, 104) + len(pool))
        self.assertEqual(d2['emitters'][1]['spawner']['tracks'][alo.ALPHA_TRACK]['keys'][-1], [20, 0.0])
        self.assertEqual(alo.write(d2), b2)
        self.assertGreater(_u32(b2, 104), _u32(b, 104))

    def test_not_an_alo(self):
        with self.assertRaises(ValueError):
            alo.read(b'XXXX' + bytes(800))
        bad = bytearray(alo.write(_sample()))
        struct.pack_into('>I', bad, 4, 11)
        with self.assertRaises(ValueError):
            alo.read(bytes(bad))


class Editing(unittest.TestCase):
    def test_recolour(self):
        doc = alo.read(alo.write(_sample()))
        alo.recolour(doc, scale=(2.0, 1.0, 2.0), tint=(1.0, 0.5, 1.0), emitters={1})
        d2 = alo.read(alo.write(doc))
        self.assertEqual(alo.emitter_colour(d2['emitters'][1]), [2.0, 1.0, 2.0])
        self.assertEqual(alo.emitter_colour(d2['emitters'][0]), [1.0, 1.0, 1.0])
        self.assertEqual(d2['emitters'][1]['spawner']['tracks'][alo.COLOUR_TRACK]['keys'], [[0, 1.0, 0.5, 1.0]])

    def test_retexture_rebuilds_the_pool(self):
        doc = alo.read(alo.write(_sample()))
        alo.retexture(doc, {'tfm_test00.nut': 'tfm_other_longer_name.nut'}, {'tfm_other_longer_name.nut': (512, 128)})
        b = alo.write(doc)
        d2 = alo.read(b)
        self.assertEqual(alo.texture_names(d2), ['tfm_other_longer_name.nut'])
        slot = d2['emitters'][1]['spawner']['layers'][0]['uvs'][0]['raw']
        self.assertEqual(struct.unpack_from('>hh', slot, 64), (512, 128))
        self.assertNotIn(b'tfm_test00', d2['pool'])
        self.assertEqual(alo.write(d2), b)

    def test_clone_and_link(self):
        doc = alo.read(alo.write(_sample()))
        k = alo.add_emitter(doc, alo.clone_emitter(doc, 1))
        alo.link(doc, 0, k)
        d2 = alo.read(alo.write(doc))
        self.assertEqual([lk['emitter'] for lk in d2['emitters'][0]['links']], [1, 2])
        self.assertTrue(all(lk['raw'][12] == 0x14 for lk in d2['emitters'][0]['links']))   # the game's usual record
        self.assertEqual(d2['emitters'][2]['spawner']['tracks'][alo.SIZE_TRACK]['keys'],
                         d2['emitters'][1]['spawner']['tracks'][alo.SIZE_TRACK]['keys'])

    def test_linked_emitters_start_only_by_their_links(self):
        doc = _sample()
        self.assertEqual(alo.roots(doc), [0])           # the sprite is linked: it no longer starts by itself
        self.assertTrue(_u32(doc['emitters'][1]['particle']['raw'], alo.START_FLAGS) & alo.CHILD_ONLY)
        free = alo.add_emitter(doc, alo.clone_emitter(doc, 1, root=True))
        kept = alo.add_emitter(doc, alo.clone_emitter(doc, 1))
        self.assertEqual(alo.roots(alo.read(alo.write(doc))), [0, free])
        self.assertFalse(alo.is_root(doc['emitters'][kept]))
        alo.set_root(doc['emitters'][kept])
        self.assertEqual(alo.roots(doc), [0, free, kept])

    def test_link_records(self):
        self.assertEqual(alo.link_record(), alo.LINK_DEFAULT)
        rec = alo.link_record(mode=alo.LINK_ATTACH, delay=30, delay_random=2, repeat=10, repeat_random=1)
        self.assertEqual(struct.unpack_from('>HHHHB', rec, 4), (10, 1, 30, 2, 0x10))
        doc = _sample()
        k = alo.add_emitter(doc, alo.new_emitter())
        alo.link(doc, 0, k, alo.link_record(mode=alo.LINK_AT_END), per_particle=False)
        b = alo.write(doc)
        mover = _u32(b, 56)
        self.assertEqual(_u16(b, mover + 74), 1)        # started by the emitter itself (its own list at +0)
        rec = _u32(b, mover)
        self.assertEqual(_u32(b, rec), mover + 2 * alo.NODE_SIZE)
        self.assertEqual(b[rec + 12], alo.LINK_AT_END)
        self.assertEqual(alo.read(b)['emitters'][0]['children'][0]['emitter'], k)
        self.assertEqual(alo.roots(doc), [0])


if __name__ == '__main__':
    unittest.main()
