"""Unit tests of the importer's formats and helpers on made-up data (no game files needed).

    python3 -m unittest discover tools/characters/tests
"""
import lzma
import os
import struct
import sys
import tempfile
import unittest
import zlib
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char import lz4, pack                        # noqa: E402
from ttt2char.convert import art, chains, colours, movelist, skin, strips, tiger  # noqa: E402
from ttt2char.tekken7 import autodump, pak, tkmvst, wwise  # noqa: E402
from ttt2char.ttt2 import archive, bone, bxml, dsp, motion, nud, nus3bank, nut  # noqa: E402


class Archive(unittest.TestCase):
    def test_keys(self):
        # a name's key: CRC-32 with the register starting at zero; a pack member: continued over base-4 digits
        self.assertEqual(archive.key_of('abc.nud'), zlib.crc32(b'abc.nud', 0xFFFFFFFF))
        base = archive.key_of('pack')
        self.assertEqual(archive.member_key(base, 0), base)
        self.assertEqual(archive.member_key(base, 6), zlib.crc32(bytes([2, 1]), base))      # 6 = 1*4 + 2

    def test_scramble_and_pack(self):
        data = bytes(range(256)) * 3 + b'tail'
        once = archive.scramble(data, 0x1234ABCD)
        self.assertNotEqual(once, data)
        self.assertEqual(archive.scramble(once, 0x1234ABCD), data)
        stored = archive.pack(data, 0x55AA55AA)
        self.assertEqual(archive.unpack(stored, 0x55AA55AA), data)

    def test_index(self):
        def chunk(tag, payload):
            return struct.pack('>I', 8 + len(payload)) + tag + payload
        keys, sizes = [0x11111111, 0x22222222], [20, 32]
        offsets, at = [], 0
        for size in sizes:
            offsets.append(at | (size % 16))
            at += (size + 15) & ~15
        offsets.append(at)
        ofs = (chunk(b'ftyp', b'nrfo\0\1\0\1') + chunk(b'head', bytes(20)) +
               chunk(b'ofsi', b''.join(struct.pack('>II', k, 0) for k in keys)) +
               chunk(b'ofsd', b''.join(struct.pack('>I', o) for o in offsets)))
        index = archive.parse_index(ofs)
        self.assertEqual([e[0] for e in index], keys)
        self.assertEqual([e[3] for e in index], sizes)


class Lz4(unittest.TestCase):
    def test_literals_and_matches(self):
        # token 0x4F: 4 literals "abcd", then a match of 4 + 15 + 1 = 20 bytes at distance 4 (overlapping copy);
        # last sequence: 5 literals
        block = bytes([0x4F]) + b'abcd' + struct.pack('<H', 4) + bytes([1]) + bytes([0x50]) + b'vwxyz'
        self.assertEqual(lz4.decompress_block(block, 4 + 20 + 5), b'abcd' * 6 + b'vwxyz')


class Textures(unittest.TestCase):
    def test_bc1_bc3(self):
        y, x = np.mgrid[0:16, 0:16]
        img = np.stack([64 + x * 4, 64 + y * 4, np.full_like(x, 128), 128 + (x + y) * 2], -1).astype(np.uint8)
        for fmt in (0, 2):
            t = nut.make_texture(img, 0x10408000, fmt, mips=3)
            self.assertEqual(len(t['mips']), 3)
            back = nut.read(nut.write(dict(textures=[t])))['textures'][0]
            out = nut.decode(back)[0] if isinstance(nut.decode(back), list) else nut.decode(back)
            rgb = np.asarray(out)[..., :3].astype(int)
            self.assertLess(np.abs(rgb - img[..., :3].astype(int)).mean(), 4, 'format %d' % fmt)


class Skeleton(unittest.TestCase):
    def test_round_trip(self):
        n = 3
        s = dict(scale=0.98, parent=[-1, 0, 1], t=np.arange(9, dtype=np.float32).reshape(3, 3),
                 r=np.zeros((3, 3), np.float32), opt=[7], params=[1.0] * 8, names=['root', 'a', 'b'],
                 s=np.ones((n, 3), np.float32))
        d = bone.write(s)
        back = bone.read(d)
        self.assertEqual(back['names'], s['names'])
        self.assertEqual(back['parent'], s['parent'])
        self.assertTrue(np.array_equal(back['t'], s['t']))
        self.assertEqual(bone.write(back), d)


class ItemDatabase(unittest.TestCase):
    def test_round_trip(self):
        tree = [dict(tag='InstanceInfo', attrs=[], children=[
                    dict(tag='Instance', attrs=[('label', 'x_1p'), ('index', '1')], children=[
                        dict(tag='Data', attrs=[('Name', 'one'), ('EquipType', 'kind')], children=[]),
                        dict(tag='Model', attrs=[('section', 'A')], children=[])])]),
                dict(tag='OffsetInfo', attrs=[], children=[])]
        d = bxml.write(tree)
        self.assertEqual(d[:4], b'BXML')
        self.assertEqual(bxml.write(bxml.parse(d)), d)


class Model(unittest.TestCase):
    def test_round_trip(self):
        vt, ut = 0x13, 0x12
        verts = np.zeros(3, nud.vertex_dtype(vt, ut))
        verts['pos'] = [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        verts['pw'] = 100.0
        verts['bones'] = [[1, 0, 0, 0]] * 3
        verts['w'] = [[1, 0, 0, 0]] * 3
        material = dict(flags=0x94010161, zero=0, src=0, dst=0, alpha_test=0, alpha_func=0, ref=0, cull=0x405,
                        u1=0, u2=0, zoff=0, textures=[dict(id=0x10408000, pad4=bytes(6), map=0, wrap_s=1,
                                                         wrap_t=1, min=1, mag=1, mip=1, b17=0, tail=bytes(6))],
                        props=[('NU_colorSamplerUV', [1.0, 1.0, 0.0, 0.0])])
        model = dict(version=0x0201, sphere=(0, 0, 0, 2), bone_first=0, bone_last=1, meshes=[
            dict(name='m', sphere=(0, 0, 0, 2), center=(0, 0, 0), sort_bias=0.0, zero=0, boneflag=4, singlebind=-1,
                 polys=[dict(vtype=vt, uvtype=ut, pflag=0, tail=bytes(12),
                             indices=np.array([0, 1, 2], np.uint16), verts=verts, materials=[material])])])
        d = nud.write(model)
        self.assertEqual(d[:4], b'NDP3')
        self.assertEqual(nud.write(nud.read(d)), d)


class Strips(unittest.TestCase):
    def test_stripify(self):
        rng = np.random.default_rng(1)
        grid = [(r * 6 + c, r * 6 + c + 1, (r + 1) * 6 + c) for r in range(5) for c in range(5)]
        tris = [grid[i] for i in rng.permutation(len(grid))]
        self.assertTrue(strips.check(tris, strips.stripify(tris)))


class Movesets(unittest.TestCase):
    def test_lzma_container(self):
        header = bytearray(0x100)
        header[:4] = b'TKM2'
        struct.pack_into('<I', header, 0x50, 0x100)
        payload = bytes(range(200)) * 10
        struct.pack_into('<Q', header, 0x58, len(payload))
        header[0x60] = 2
        packed = bytes(header) + lzma.compress(payload)
        out = tkmvst.decompress(packed)
        self.assertEqual(out[0x100:], payload)


class Voice(unittest.TestCase):
    def test_fnv(self):
        self.assertEqual(wwise.fnv('ABC'), wwise.fnv('abc'))          # ids hash the lower-case name
        h = 2166136261
        for c in b'abc':
            h = ((h * 16777619) & 0xFFFFFFFF) ^ c
        self.assertEqual(wwise.fnv('abc'), h)

    def test_wwise_ima(self):
        # one mono block: header {sample 0, step index 0}, codes 4 then 0 (low half first), then zeros
        block = struct.pack('<hBB', 0, 0, 0) + bytes([0x04]) + bytes(31)
        fmt = struct.pack('<HHIIHH', 2, 1, 48000, 27000, 36, 4) + struct.pack('<HHI', 6, 0, 0)
        body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(block)) + block
        rate, a = wwise.decode_wem(b'RIFF' + struct.pack('<I', len(body)) + body)
        self.assertEqual((rate, a.shape), (48000, (1, 64)))
        self.assertEqual(a[0, :2].tolist(), [7, 8])      # step 7: +7; then index 2 (step 9): +9 >> 3

    def test_dsp_round_trip(self):
        t = np.arange(14 * 200)
        x = (8000 * np.sin(t * 0.05) + 3000 * np.sin(t * 0.31)).astype(np.int16)
        rate, back = dsp.read_idsp(dsp.idsp([x, x // 2], 32000))
        self.assertEqual(rate, 32000)
        for orig, got in zip((x, x // 2), back):
            e = got.astype(float) - orig.astype(float)
            self.assertGreater(10 * np.log10((orig.astype(float) ** 2).mean() / max((e ** 2).mean(), 1e-9)), 20)

    def test_nus3bank(self):
        def tone(name, audio, link=None, at=0):
            n = name.encode() + b'\0'
            head = (bytes(4) + b'\x7f' + bytes(3)) if link else (bytes(4) + bytes([0xff, 0xff, 0x27, 0x84]) + b'\x7f' + bytes(3))
            t = bytearray(head + bytes([len(n)]) + n)
            while len(t) % 4:
                t.append(0)
            if link:
                t += b'\xff\xff\xff\xff' + struct.pack('<II', len(link), 1) + b''.join(
                    struct.pack('<fIIII', 1.0, k, 0, 0, 0) for k in link)
            else:
                t += struct.pack('<IIII', 0, 8, at, len(audio)) + bytes(16) + b'\xff\xff\xff\xff' + struct.pack(
                    '<III', 48000, 1, 10) + bytes(8)
            return bytes(t)
        tones = [tone('a_01', b'AAAA' * 4), tone('b_02', b'BB' * 8, at=16), tone('l_00', None, link=[0, 1])]
        table, body = struct.pack('<I', len(tones)), b''
        for t in tones:
            table += struct.pack('<II', 4 + 8 * len(tones) + len(body), len(t))
            body += t
        pack = b'AAAA' * 4 + b'BB' * 8
        chunks = [(b'BINF', struct.pack('<II', 0, 3) + bytes([5]) + b'test\0' + bytes(2) + struct.pack('<I', 7)),
                  (b'TONE', table + body), (b'PACK', pack)]
        toc = struct.pack('<I', len(chunks)) + b''.join(t + struct.pack('<I', len(d)) for t, d in chunks)
        rest = b'BANKTOC ' + struct.pack('<I', len(toc)) + toc + b''.join(t + struct.pack('<I', len(d)) + d
                                                                         for t, d in chunks)
        data = b'NUS3' + struct.pack('<I', len(rest)) + rest
        b = nus3bank.Bank(data)
        self.assertEqual(b.write(), data)
        self.assertEqual([b.name(i) for i in range(3)], ['a_01', 'b_02', 'l_00'])
        self.assertEqual([k for _, k in b.links(2)], [0, 1])
        b.replace(1, b'CCCCCCCC', 32000, 1, 4)
        k = b.append(b.renamed(0, 'a_09'), audio=b'DDDD')
        b2 = nus3bank.Bank(b.write())
        self.assertEqual((b2.audio[1], b2.format(1), b2.name(k), b2.audio[k]), (b'CCCCCCCC', (32000, 1, 4), 'a_09', b'DDDD'))
        # one bank of a fighter's shouts, then its win quotes: the link tone plays the moved tones
        from ttt2char.convert import voice
        merged, first = voice.merge(data, data)
        m = nus3bank.Bank(merged)
        self.assertEqual(first, 3)
        self.assertEqual([m.name(i) for i in range(6)], ['a_01', 'b_02', 'l_00'] * 2)
        self.assertEqual([k for _, k in m.links(5)], [3, 4])
        self.assertEqual((m.audio[3], m.audio[4]), (b'AAAA' * 4, b'BB' * 8))


class CommandList(unittest.TestCase):
    def test_convert(self):
        strings = bytearray(b'\0')                      # offset 0: the empty string
        def put(s):
            strings.extend(s.encode() + b'\0')
            return len(strings) - len(s.encode()) - 1
        names = [put('jp'), put('Move'), put('reading'), put('Move')] + [put('cmd%d' % i) for i in range(12)]
        so = 0x20
        names = [so + o for o in names]
        entry = bytearray(372)
        struct.pack_into('<16I', entry, 0, *names)
        struct.pack_into('<HHHH', entry, 0x44, 0, 1, 0, 1)
        drill = bytearray(372)
        struct.pack_into('<16I', drill, 0, *names)
        struct.pack_into('<HHHH', drill, 0x44, 0xFFFF, 0, 15, 0)
        while len(strings) % 4:
            strings.append(0)
        eo = so + len(strings)
        do = eo + 2 * 372
        io = do + 24
        demo = struct.pack('<8HIHH', 0, 2000, 0, 0, 0, 0, 0, 1, 0, 0, 0)
        m = struct.pack('<4sHHIIIIII', b'MVLT', 1000, 2, 2, 1, so, eo, do, io) + bytes(strings) + bytes(entry) + \
            bytes(drill) + demo + bytes([0x20, 0x80, 3, 1])
        out = movelist.convert(m, log=lambda s: None)
        magic, ver, _, n, nd, so2, eo2, do2, io2 = struct.unpack_from('>4sHHIIIIII', out, 0)
        self.assertEqual((magic, ver, n, nd), (b'MVLT', 2, 1, 1))
        first = struct.unpack_from('>12I', out, eo2)
        self.assertEqual(out[so2 + first[1]:].split(b'\0')[0], b'Move')
        self.assertEqual(out[so2 + first[4]:].split(b'\0')[0], b'cmd1')     # en <- Tekken 7's [5]
        self.assertEqual(out[io2:io2 + 4], bytes([0x80, 0x20, 3, 1]))          # buttons, direction swapped


class Colours(unittest.TestCase):
    def test_fit_and_palette(self):
        grid = np.zeros((32, 64, 3, 3), '>f4')
        texts = ['<Colors>\n\t<Color id="0" palette="51" x="64" y="32" s="0" />\n</Colors>']
        pal = colours.Palette(grid.tobytes(), texts)
        c1 = np.full((8, 8, 3), 0.9)
        c1[:, 4:] = (0.8, 0.1, 0.1)                     # red in both
        c2 = c1.copy()
        c2[:, :4] = (0.2, 0.2, 0.25)                     # white -> dark in 2P
        f = colours.fit(c1, c2, pal)
        self.assertTrue((f['alpha'][:, :4] == 1).all() and (f['alpha'][:, 4:] == 0).all())
        (e1, q1, e2, q2), _ = f['groups']
        self.assertNotEqual(e1[:2], (64, 32))            # a cell the game uses is never taken
        g = np.frombuffer(pal.grid_bytes(), '>f4').reshape(32, 64, 3, 3)
        self.assertTrue(np.allclose(g[e2[1] - 1, e2[0] - 1, 1], q2, atol=1e-6))
        xml = colours.palette_xml(texts[0], 61, {51: e1})
        self.assertIn('<Color id="61" palette="51" x="%d" y="%d" s="9" />' % e1[:2], xml)


class Lettering(unittest.TestCase):
    @staticmethod
    def name(widths, spaces=()):
        """A made-up name picture: opaque bars as letters (3 pixels apart, wider at `spaces`), a dark shadow."""
        pic = np.zeros((20, 8 + sum(widths) + 3 * len(widths) + 8 * len(spaces) + 8, 4), np.float32)
        x = 8
        for i, w in enumerate(widths):
            if i in spaces:
                x += 8
            pic[5:15, x:x + w] = (0.8, 0.8, 0.8, 1.0)
            x += w + 3
        solid = pic[..., 3] == 1.0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                near = np.roll(np.roll(solid, dy, 0), dx, 1) & ~solid
                pic[near] = (0.0, 0.0, 0.0, 0.5)
        return pic

    def test_letters(self):
        letters = art.Letters()
        letters.learn('AB C', self.name([4, 6, 5], spaces=(2,)))
        letters.learn('BA', self.name([6, 4]))
        self.assertEqual(sorted(letters.glyphs), ['A', 'B', 'C'])
        pic = letters.picture('CAB')
        cols = np.nonzero((pic[..., 3] > 0.95).any(0))[0]
        runs = np.split(cols, np.nonzero(np.diff(cols) > 1)[0] + 1)
        self.assertEqual([len(r) for r in runs], [5, 4, 6])              # C, A, B as learnt
        self.assertEqual([runs[i + 1][0] - runs[i][-1] - 1 for i in range(2)], [3, 3])
        self.assertTrue(0.4 < pic[10, cols[0] - 1, 3] < 0.6)            # the shadow next to a letter
        self.assertIsNone(letters.picture('Q'))


class Tiger(unittest.TestCase):
    @staticmethod
    def matrix(t, r):
        m = np.eye(4)
        m[:3, :3] = bone.matrix_from_euler_xyz(np.asarray(r, np.float64))
        m[:3, 3] = t
        return m

    def test_bones_and_channels(self):
        skel = dict(scale=1.0, parent=[-1], t=np.array([[0, 1000, 0]], np.float32), r=np.zeros((1, 3), np.float32),
                    s=np.ones((1, 3), np.float32), opt=[0], params=[1.0] * 8, names=['cha:HARA_ROT1'],
                    names_raw=None)
        root = self.matrix((0, 900, -400), (np.pi / 2, 0, 0))
        body = root @ self.matrix((300, 0, 0), (0, 0.3, 0))
        s, first = tiger.append_bones(skel, dict(names=['Root', 'Body'], parents=[-1, 0], world=[root, body]))
        self.assertEqual((first, s['names'][1:], s['parent'], s['opt']),
                         (1, ['cha:OPT600_Root', 'cha:OPT601_Body'], [-1, 0, 1], [0, 600, 601]))
        hide = self.matrix((0, tiger.HIDE_Y, 0), (0, 0, 0))
        w = bone.world_matrices(bone.read(bone.write(s)))
        self.assertTrue(np.allclose(w[1], hide @ root, atol=1e-2) and np.allclose(w[2], hide @ body, atol=1e-2))
        moved = self.matrix((0, 0, 200), (0, 0, 0))
        worlds = np.array([[root, body], [moved @ root, moved @ body @ self.matrix((0, 0, 0), (0.2, 0, 0))]])
        ch = tiger.channels(worlds, s, first, [0, 1])
        self.assertEqual([d for d, _ in ch], [11, 7, 3, 11, 7, 3])
        hara = bone.world_matrices(s)[0]
        for f in range(2):
            r = hara @ self.matrix(ch[0][1][f], ch[1][1][f])
            b = r @ self.matrix(ch[3][1][f], ch[4][1][f])
            self.assertTrue(np.allclose(r, worlds[f][0], atol=1e-3) and np.allclose(b, worlds[f][1], atol=1e-3))
        self.assertTrue(np.allclose(ch[0][1][2], s['t'][1]) and np.allclose(ch[4][1][2], s['r'][2]))  # parked
        a = tiger.raw_animation(ch)
        self.assertEqual(struct.unpack_from('>HHI6I', a, 0), (0xC8, 6, 3, 11, 7, 3, 11, 7, 3))
        self.assertEqual(len(a), 8 + 6 * 4 + 3 * 6 * 12)

    def test_spring_file(self):
        def mop(numbers, fill):
            out = struct.pack('>f', 5.0) + bytes(28) + struct.pack('>I', len(numbers)) + bytes(12)
            for n in numbers:
                out += struct.pack('>I', n) + bytes([fill + n % 7]) * (tiger.MOP_ENTRY - 4)
            return out + bytes(16)
        donor, template = mop([0], 1), mop([600, 601], 9)
        out = tiger.spring_file(donor, template, 3)
        n, = struct.unpack_from('>I', out, 0x20)
        entries = [out[0x30 + 0x60 * k:0x30 + 0x60 * (k + 1)] for k in range(n)]
        self.assertEqual([struct.unpack_from('>I', e)[0] for e in entries], [0, 600, 601, 602])
        self.assertEqual(entries[0], donor[0x30:0x90])
        self.assertTrue(all(e[4:] == template[0x34:0x90] for e in entries[1:]))
        self.assertEqual((out[:0x20], len(out)), (donor[:0x20], 0x30 + 0x60 * 4 + 16))

    def test_replace_mota(self):
        old = motion.extend_mota(b'MOTA\x00\x01\x00\x00' + struct.pack('>II', 0x14, 0) + bytes(4), [b'a' * 8, b'b' * 4])
        new = motion.replace_mota(old, [b'c' * 6])
        e, offs = motion.mota_offsets(new)
        self.assertEqual((e, len(offs), new[offs[0]:offs[0] + 6]), ('>', 1, b'c' * 6))


class Places(unittest.TestCase):
    def test_choose_slot(self):
        with tempfile.TemporaryDirectory() as folder:
            def install(code, slot=None):
                os.makedirs(os.path.join(folder, code))
                with open(os.path.join(folder, code, 'character.ini'), 'w') as f:
                    f.write('code = %s\ndonor = jun\n' % code + ('slot = %d\n' % slot if slot is not None else ''))
            self.assertEqual(pack.choose_slot(folder, 'aaa'), 0)
            install('bbb', 1)
            self.assertEqual((pack.choose_slot(folder, 'aaa'), pack.choose_slot(folder, 'bbb')), (0, 1))
            install('ccc')                                  # an old pack naming no place: the first free one
            self.assertEqual(pack.places_of(pack.installed(folder)), {'bbb': 1, 'ccc': 0})
            self.assertEqual((pack.choose_slot(folder, 'aaa'), pack.choose_slot(folder, 'ccc')), (2, 0))
        self.assertEqual(pack.place(0), dict(id=64, row=58, column=19, grid_row=0, anchor='edd', columns=0))
        self.assertEqual((pack.place(5)['row'], pack.place(7)['column'], pack.place(7)['id']), (69, 23, 71))
        self.assertEqual(pack.place(len(pack.CELLS))['anchor'], None)
        cells = [(c[0], c[1]) for c in pack.CELLS]
        self.assertEqual(len(set(cells)), len(cells))


class Chains(unittest.TestCase):
    def test_turn(self):
        for a, b in (((1, 0, 0), (0, -1, 0)), ((0, 1, 0), (0.6, 0, -0.8)), ((1, 0, 0), (-1, 0, 0))):
            a, b = np.array(a, float), np.array(b, float)
            r = chains.turn(a, b)
            self.assertTrue(np.allclose(r @ a, b) and np.allclose(r @ r.T, np.eye(3)))
            self.assertAlmostEqual(np.linalg.det(r), 1.0)

    def model(self):
        """A head bone and a three-bone chain sticking out along +x (the head scaled 1.2, as Tekken 7's are)."""
        world = np.tile(np.eye(4), (4, 1, 1))
        world[0][:3, :3] *= 1.2
        world[1:, :3, 3] = [(100, 0, 0), (300, 0, 0), (500, 0, 0)]
        world[1:, :3, :3] *= 1.2
        pos = np.array([[200.0, 10, 0], [400, -10, 0], [50, 0, 0]])
        joints = np.array([[1, 2] + [0] * 6, [2, 3] + [0] * 6, [0] * 8])
        weights = np.array([[0.5, 0.5] + [0] * 6, [1.0] + [0] * 7, [1.0] + [0] * 7])
        streams = dict(pos=pos, nrm=np.tile([0.0, 1, 0], (3, 1)), tan=np.tile([1.0, 0, 0, 1], (3, 1)),
                       joints=joints, weights=weights)
        return dict(bones=dict(names=['Head', 'Hair_OPT001', 'Hair_OPT002', 'Hair_OPT003end'],
                               parents=[-1, 0, 1, 2], world=world),
                    prims=[dict(streams), dict(streams)],
                    parts=dict(har=dict(names=['Head', 'X', 'Hair_OPT001', 'Hair_OPT002', 'Hair_OPT003end'],
                                        parents=[-1, 0, 0, 2, 3], remap=np.array([0, 0, 1, 2, 3]))))

    def test_drape(self):
        k = self.model()
        bones = chains.find(k, 'har', 'Hair_OPT001')
        self.assertEqual(bones, [1, 2, 3])
        moved = chains.drape(k, [(bones, [(0, -1, 0)])])           # one direction for every link: straight down
        w = k['bones']['world']
        self.assertEqual(moved, 2)
        self.assertTrue(np.allclose(w[1:, :3, 3], [(100, 0, 0), (100, -200, 0), (100, -400, 0)]))
        p = k['prims'][0]['pos']
        self.assertTrue(np.allclose(p[1], (90, -300, 0)))           # 100 along the second link, 10 off it
        self.assertTrue(np.allclose(p[2], (50, 0, 0)))              # the head's vertex stays
        self.assertTrue(np.allclose(p[0], (110, -100, 0)))          # on two bones that turn alike
        self.assertTrue(np.allclose(k['prims'][1]['pos'], p))       # shared streams moved once

    def test_bones_and_springs(self):
        k = self.model()
        bones = chains.find(k, 'har', 'Hair_OPT001')
        chains.drape(k, [(bones, [(0, -1, 0)])])
        skel = dict(scale=1.0, parent=[-1], t=np.array([[0, 0, 0]], np.float32), r=np.zeros((1, 3), np.float32),
                    s=np.ones((1, 3), np.float32), opt=[0], params=[1.0] * 8, names=['cha:ATAMA_jnt_assist'],
                    names_raw=None)
        s, made = chains.append_bones(skel, k, [bones], [0])
        self.assertEqual((made, s['parent'], s['opt']), ([[1, 2, 3]], [-1, 0, 1, 2], [0, 300, 301, 302]))
        self.assertEqual(s['names'][1], 'cha:OPT300_Hair_OPT001')
        w = bone.world_matrices(bone.read(bone.write(s)))
        for i, b in enumerate(bones):
            frame = k['bones']['world'][b].copy()
            frame[:3, :3] /= np.linalg.norm(frame[:3, :3], axis=0)
            self.assertTrue(np.allclose(w[1 + i], frame, atol=1e-3))
        self.assertTrue(np.allclose(s['t'][2], (200, 0, 0), atol=1e-3))   # a link along its parent's x axis

        def mop(numbers, colliders, fill):
            out = struct.pack('>f', 5.0) + bytes(28) + struct.pack('>I', len(numbers)) + bytes(12)
            for n in numbers:
                out += struct.pack('>I', n) + bytes([fill]) * (chains.MOP_ENTRY - 4)
            out += struct.pack('>I', len(colliders)) + bytes(12)
            return out + b''.join(struct.pack('>II', 1, c) + bytes(chains.MOP_COLLIDER - 8) for c in colliders)
        out = chains.spring_file(mop([0], [], 1), mop([150, 160], [73], 2), [300, 301])
        n, = struct.unpack_from('>I', out, chains.MOP_COUNT)
        entries = [out[0x30 + 0x60 * i:0x30 + 0x60 * (i + 1)] for i in range(n)]
        self.assertEqual([struct.unpack_from('>I', e)[0] for e in entries], [0, 300, 301])
        self.assertTrue(entries[0][4:] == bytes([1]) * 0x5C and entries[2][4:] == bytes([2]) * 0x5C)
        at = 0x30 + 0x60 * n
        self.assertEqual(struct.unpack_from('>I4xII', out, at)[0], 1)
        self.assertEqual(struct.unpack_from('>II', out, at + 16), (1, 73))
        self.assertEqual(len(out), at + 16 + chains.MOP_COLLIDER)


class Skin(unittest.TestCase):
    def test_gain(self):
        theirs = {'ann': np.array([0.6, 0.4, 0.3]), 'ask': np.array([0.5, 0.4, 0.3]), 'jul': np.array([0.6, 0.3, 0.3])}

        class Paks:
            @staticmethod
            def find(path):
                return any('/%s/' % c in path for c in theirs)

        class Materials:
            @staticmethod
            def effective(path, colours):
                return path

        def bake(path):
            return np.full((2, 2, 3), 0.2), None
        with mock.patch.object(skin, 'ttt2_face', lambda archives, code: theirs[code]):
            g, target = skin.gain(Paks(), None, 'female', Materials(), bake)
            self.assertTrue(np.allclose(g, (3.0, 2.0, 1.5)))           # the median of the ratios
            self.assertTrue(np.allclose(target, (0.6, 0.4, 0.3)))
            # a skin texture far more orange than theirs: its hue brought within HUE_SPAN, its red kept
            orange = skin.hue(np.full((2, 2, 3), (0.6, 0.2, 0.05)), target)
            m = orange.reshape(-1, 3).mean(0)
            self.assertAlmostEqual(m[0], 0.6)
            self.assertAlmostEqual(m[1] / m[0], 0.4 / 0.6 / skin.HUE_SPAN, places=5)
            self.assertAlmostEqual(m[2] / m[0], 0.3 / 0.6 / skin.HUE_SPAN, places=5)
            near = np.full((2, 2, 3), (0.6, 0.38, 0.29))
            self.assertTrue(np.allclose(skin.hue(near, target), near))  # within the span: as it is
            theirs.pop('jul')
            self.assertTrue(np.allclose(skin.gain(Paks(), None, 'female', Materials(), bake)[0], 1.0))   # too few



class Materials(unittest.TestCase):
    def test_classify(self):
        from ttt2char.convert.model import classify
        self.assertEqual(classify('MI_CH_x_skin_face', 'M_CH_skin_wet_SSP', None), ('skin', True, False))
        self.assertEqual(classify('MI_CH_x_Eyes', 'M_CH_Eye_IN', None), ('eye', False, False))
        self.assertEqual(classify('MI_CH_mrx_eye', 'M_CH_mrx_eye', None), ('eye', False, False))
        self.assertIsNone(classify('MI_CH_x_Eye_ADD', 'M_CH_Eye_ADD', None))
        self.assertIsNone(classify('MI_CH_tear', 'M_CH_tear', None))
        self.assertEqual(classify('MI_CH_x_teeth', 'M_CH_simple_skin', None), ('mouth', False, False))
        self.assertEqual(classify('MI_CH_mouth', 'M_CH_simple_skin', None), ('mouth', True, False))
        self.assertEqual(classify('MI_CH_x_eyelash', 'M_CH_trans', None), ('overlay', False, True))
        self.assertEqual(classify('MI_CH_x_har_1p', 'M_CH_AnisoHair_Pass1', None), ('hair', False, True))
        lace = np.ones((8, 8), np.float32)
        lace[:2] = 0
        self.assertEqual(classify('MI_CH_x_bdu', 'M_CH_Base_mask', lace), ('cloth', True, True))
        self.assertEqual(classify('MI_CH_x_bdu', 'M_CH_Base', np.ones((8, 8))), ('cloth', True, False))

    def test_texture_budget(self):
        from ttt2char.convert.model import TextureBank
        bank = TextureBank()
        img = np.zeros((128, 128, 4), np.uint8)
        a = bank.add(('img', 'a', 0, False), img, 0)
        n = bank.add(('nrm', 'n'), img, 2)
        bank.add(('img', 'b', 2, True), img, 2)
        size = sum(len(m) for t in bank.textures for m in t['mips'])
        self.assertEqual(bank.fit(size), 0)
        self.assertEqual(bank.fit(size - 1), 1)                 # the normal map first (BC3, as big as b)
        t = bank.textures[n - bank.first]
        self.assertEqual((t['w'], t['h'], len(t['mips']), t['gidx']), (64, 64, 3, n))
        self.assertEqual(bank.textures[a - bank.first]['w'], 128)
        self.assertEqual(bank.fit(0), 2)                        # down to 64 at most
        self.assertTrue(all(t['w'] == 64 for t in bank.textures))
        new = bank.keep([n, bank.first + 2])                    # a's left out: the others numbered again
        self.assertEqual(new, {n: bank.first, bank.first + 2: bank.first + 1})
        self.assertEqual([t['gidx'] for t in bank.textures], [bank.first, bank.first + 1])
        self.assertEqual(bank.normals, {0})
        self.assertEqual(len(bank.sources), 2)


class Cells(unittest.TestCase):
    def test_cells(self):
        # every cell its own (column, row), the top-row ends first, the columns beyond the grid after
        cells = [(c[0], c[1]) for c in pack.CELLS]
        self.assertEqual(len(set(cells)), len(cells))
        self.assertTrue(all(r == 0 and 0 <= c <= 22 for c, r in cells[:7]))
        self.assertTrue(all(c < 0 or c > 22 for c, _ in cells[7:]))
        self.assertEqual(pack.place(9), dict(id=73, row=73, column=23, grid_row=1, anchor='kum', columns=1))


class Effects(unittest.TestCase):
    def test_textures_of(self):
        from ttt2char.convert import effects
        alo = b'OELA' + bytes(12) + b'AL_EFFECT\0tfm_x00.nut\0tfm_cmn_part03.nut\0\0'
        self.assertEqual(effects.textures_of(alo), ['tfm_x00.nut', 'tfm_cmn_part03.nut'])
        self.assertEqual(effects.textures_of(b'OELA'), [])


def _fstring(s):
    b = s.encode('latin1') + b'\0'
    return struct.pack('<i', len(b)) + b


def write_pak(path, files):
    """A version-3 pak of uncompressed entries {name: data} (each flagged encrypted, as Tekken 7's are)."""
    body, records = b'', []
    for name, data in files.items():
        record = struct.pack('<qqqi', len(body), len(data), len(data), 0) + bytes(20) + b'\1' + struct.pack('<I', 0)
        records.append(_fstring(name) + record)
        body += record + data                      # the record's copy in front of the data (53 bytes)
    index = _fstring('../../../') + struct.pack('<i', len(files)) + b''.join(records)
    with open(path, 'wb') as f:
        f.write(body + index + struct.pack('<IIqq', pak.PAK_MAGIC, 3, len(body), len(index)) + bytes(20))


class EncryptedFiles(unittest.TestCase):
    def test_saved_copies(self):
        package = b'\xc1\x83\x2a\x9e' + bytes(12)
        files = {'TekkenGame/Content/Character/Item/Sets/ABC/CS_ABC.uasset': package,
                 'TekkenGame/Content/Character/Item/Sets/ABC/CS_ABC_2.uasset': b'\x09\xf8\x48\xc6' + bytes(12),
                 'TekkenGame/Content/WwiseAudio/Windows/AkBankVoiceABC.bnk': b'\x13\x2e\xf7\x04' + bytes(4),
                 'TekkenGame/Content/WwiseAudio/Windows/AkBankVoiceXYZ.bnk': b'BKHD' + bytes(4)}
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, 'Paks'))
            write_pak(os.path.join(d, 'Paks', 'a.pak'), files)
            saved = os.path.join(d, 'files')
            paks = pak.Paks(os.path.join(d, 'Paks'), saved=saved)
            self.assertEqual(paks.read('/Game/Character/Item/Sets/ABC/CS_ABC'), package)
            with self.assertRaises(pak.EncryptedError):
                paks.read('/Game/Character/Item/Sets/ABC/CS_ABC_2')
            self.assertEqual([e.path.rsplit('/', 1)[1] for e in autodump.unsaved_files(paks, 'abc')],
                             ['CS_ABC_2.uasset', 'AkBankVoiceABC.bnk'])
            self.assertEqual(autodump.unsaved_files(paks, 'xyz'), [])
            # the game's reading of it, saved below its pak path
            target = os.path.join(saved, 'TekkenGame', 'Content', 'Character', 'Item', 'Sets', 'ABC', 'CS_ABC_2.uasset')
            os.makedirs(os.path.dirname(target))
            with open(target, 'wb') as f:
                f.write(package)
            self.assertEqual(paks.read('/Game/Character/Item/Sets/ABC/CS_ABC_2'), package)
            self.assertEqual([e.path.rsplit('/', 1)[1] for e in autodump.unsaved_files(paks, 'abc')],
                             ['AkBankVoiceABC.bnk'])
            paks.close()


if __name__ == '__main__':
    unittest.main()
