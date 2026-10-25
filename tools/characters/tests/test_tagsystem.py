"""Unit tests of ttt2char.convert.tagsystem (Tag Assault, Tag Crash, bounds) and the converter's air-slam rule, on
made-up tables (no game files needed).

    python3 -m pytest -q tools/characters/tests/test_tagsystem.py
"""
import collections
import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from ttt2char.convert import idmaps as M  # noqa: E402
from ttt2char.convert import moveset, tagsystem as TS  # noqa: E402
from ttt2char.movesets import TTT2, Moveset  # noqa: E402

STANCE = 0x8001
GROUND_UP, GROUND_DOWN = 0x8004, 0x8008             # alias slots of the landings (face up, face down)


class Tables:
    """Made-up TTT2 tables, built list by list."""

    def __init__(self):
        self.t = {name: [] for name in TTT2}
        self.t['moves'] = []
        self.t['cancel_extras'] = [dict(value=0x1000 + n) for n in range(16)]
        self.names = []
        self.aliases = [0] * 56

    def req(self, *conditions):
        return TS.write(self.t, 'requirements', [dict(condition=c, param=p) for c, p in conditions]
                        + [dict(condition=TS.TT_END, param=0)])

    def cancels(self, *rows):
        base = dict(direction=0, button=0, req=0, extradata=0, win_start=1, win_end=32767, start_frame=1, move_id=0,
                    option=0)
        return TS.write(self.t, 'cancels', [dict(base, **r) for r in rows])

    def hits(self, *entries):
        return TS.write(self.t, 'hit_conditions', [dict(req=r, damage=d, reactions=x) for r, d, x in entries])

    def props(self, *rows):
        return TS.write(self.t, 'extra_move_properties', [dict(frame=f, id=i, value=v) for f, i, v in rows]
                        + [dict(frame=0, id=0, value=0)])

    def reaction(self, default):
        row = {f: 0 for f, _, _, _ in TTT2['reactions'][1]}
        row['default'] = default
        return TS.write(self.t, 'reactions', [row])

    def move(self, name, **fields):
        row = {f: 0 for f, _, _, _ in TTT2['moves'][1]}
        row.update(name=len(self.names), cancel=None, hit_condition=None, extraprops=None, voiceclip=None,
                   begin_props=None, end_props=None, hitbox=b'\1\2\3\4')
        row.update(fields)
        self.names.append(name)
        return TS.write(self.t, 'moves', [row])

    def end(self, start=48):
        return dict(direction=M.LIST_END, win_start=start, start_frame=start)


def rows(t, table, i, last):
    return [dict(r) for r in TS.chain(t[table], i, last)]


class Assault(unittest.TestCase):
    def donor(self):
        """A donor with two Tag Assault triples: X (active 20-21) -tag-> XT -hit (468)-> XDA; the second X's hitlevel
        has a high half, and its XDA keeps a property the first one's leaves out."""
        d = Tables()
        d.move('stance')
        self.r_air, self.r_bound, self.r_ground, self.r_assault, self.r_none = (d.reaction(n) for n in range(5))
        always, never = d.req((0, 0)), d.req()
        hit, bound, air, assault = d.req((46, 0), (0x8003, 0)), d.req((467, 0)), d.req((466, 0)), d.req((468, 0))
        exit_ = d.req((46, 0), (481, 0), (488, 0))
        back1, back2 = d.req((313, 0), (176, 1)), d.req((313, 0), (177, 1))
        self.nohit = d.hits((always, 0, 0), (never, 0, self.r_none))
        x_hits = [(bound, 18, self.r_bound), (air, 18, self.r_air), (never, 18, self.r_ground)]
        on_hit = dict(req=hit, extradata=7, move_id=STANCE)
        voice = TS.write(d.t, 'voiceclips', [dict(value=0x02000001), dict(value=0xFFFFFFFF)])
        for name, hitlevel, kept in (('Ju_x', 0x217, ()), ('Ju_y', 0x200217, [(0x8001, 0x80f6, 1)])):
            x = len(d.t['moves'])
            xt, xda = x + 1, x + 2
            d.move(name, first_active=20, last_active=21, hitlevel=hitlevel, voiceclip=voice,
                   hit_condition=d.hits(*x_hits),
                   cancel=d.cancels(on_hit, dict(button=TS.TAG_BUTTON, req=always, extradata=11, win_end=19,
                                                 start_frame=19, move_id=xt), d.end()),
                   extraprops=d.props((0x8001, 0x80f8, 5), (0x8001, 0x80f6, 1), (13, 0x82f9, 6)))
            d.move(name + 'T', first_active=20, last_active=21, hitlevel=hitlevel, voiceclip=voice,
                   hit_condition=d.hits((assault, 18, self.r_assault), *x_hits),
                   cancel=d.cancels(on_hit, dict(req=exit_, extradata=11, move_id=xda), d.end()),
                   extraprops=d.t['moves'][x]['extraprops'])
            d.move(name + 'DA', hitlevel=hitlevel & 0xFFFF0000, hitbox=bytes(4), hit_condition=self.nohit,
                   cancel=d.cancels(dict(req=back1, extradata=7, win_start=22, win_end=22, start_frame=22,
                                         move_id=STANCE),
                                    dict(req=back2, extradata=7, win_start=22, win_end=22, start_frame=22,
                                         move_id=STANCE), d.end()),
                   extraprops=d.props((13, 0x82f9, 6), *kept, (22, 0x8256, 0), (22, 0x8081, 0), (22, 0x812A, 360),
                                      (24, 0x812A, 4096), (0x8001, 0x82ED, 0)))
        self.reqs = dict(always=always, never=never, hit=hit, bound=bound, air=air, assault=assault, exit=exit_,
                         back1=back1, back2=back2)
        return d

    def test_template_and_twins(self):
        d = self.donor()
        tpl = TS.assault_template(d.t, d.aliases)
        self.assertEqual(tpl['triples'], 2)
        self.assertEqual(tpl['leaves out'], {0x80f8, 0x80f6})               # 0x80f6: left out of one of two
        self.assertEqual(tpl['before'], {(STANCE, 0x1007)})                 # X's row into the stance, extradata 7
        self.assertEqual(tpl['fields'][('XDA', 'hitlevel')], TS.HIGH)
        T = copy.deepcopy(d.t)
        names = list(d.names)
        first = len(T['moves'])
        r = self.reqs
        x_hits = TS.write(T, 'hit_conditions', [dict(req=r['bound'], damage=21, reactions=self.r_bound),
                                                dict(req=r['air'], damage=21, reactions=self.r_air),
                                                dict(req=r['never'], damage=21, reactions=self.r_ground)])
        on_hit, split = 0x30, 0x31                     # X's own follow-ups (on hit; at its first active frame)
        x_cancels = TS.write(T, 'cancels', [
            dict(direction=0, button=0, req=r['hit'], extradata=7, win_start=1, win_end=32767, start_frame=1,
                 move_id=STANCE, option=0),
            dict(direction=0, button=0, req=r['hit'], extradata=11, win_start=1, win_end=32767, start_frame=1,
                 move_id=on_hit, option=0),
            dict(direction=0, button=0x40000002, req=r['always'], extradata=3, win_start=10, win_end=30,
                 start_frame=30, move_id=0, option=0),
            dict(direction=0, button=0, req=r['always'], extradata=11, win_start=16, win_end=16, start_frame=16,
                 move_id=split, option=0),
            dict(direction=M.LIST_END, button=0, req=0, extradata=0, win_start=50, win_end=32767, start_frame=50,
                 move_id=0, option=0)])
        x_props = TS.write(T, 'extra_move_properties', [dict(frame=0x8001, id=0x80f8, value=1),
                                                         dict(frame=12, id=0x801f, value=7),
                                                         dict(frame=0, id=0, value=0)])
        k = dict(d.t['moves'][1], name=len(names), first_active=16, last_active=17, hitlevel=0x200512,
                 hit_condition=x_hits, cancel=x_cancels, extraprops=x_props, voiceclip=9)
        names.append('Ka_6rk')
        T['moves'].append(k)
        before = copy.deepcopy(T['cancels'])

        def string(s):
            names.append(s)
            return len(names) - 1
        made = TS.add_tag_assault(T, tpl, first, lambda i: names[T['moves'][i]['name']], string)
        self.assertEqual(made, [(first, first + 1, first + 2)])
        x, xt, xda = (T['moves'][i] for i in made[0])
        self.assertEqual(T['cancels'][:len(before)], before)                    # X's old list unchanged
        self.assertNotEqual(x['cancel'], x_cancels)
        # X: its rows, the tag row 1-15 sf 15 -> XT, its end row
        cl = TS.cancels(T, x['cancel'])
        self.assertEqual([c['button'] for c in cl], [0, 0, 0x40000002, 0, TS.TAG_BUTTON, 0])
        tag = cl[4]
        self.assertEqual((tag['win_start'], tag['win_end'], tag['start_frame'], tag['move_id'], tag['extradata'],
                          tag['req']), (1, 15, 15, first + 1, 11, r['always']))
        # XT: X's animation and frames, [468] + X's hits, X's row into the stance, the exit row, X's other automatic
        # rows (its follow-ups on the hit and at the hit's frame: after the exit, which they would otherwise beat), the
        # end row
        self.assertEqual(names[xt['name']], 'Ka_6rkT')
        self.assertEqual((xt['first_active'], xt['last_active'], xt['hitlevel'], xt['voiceclip'], xt['extraprops']),
                         (16, 17, 0x200512, 9, x_props))
        hs = TS.hits(T, xt['hit_condition'])
        self.assertEqual([(h['req'], h['damage'], h['reactions']) for h in hs],
                         [(r['assault'], 21, self.r_assault), (r['bound'], 21, self.r_bound),
                          (r['air'], 21, self.r_air), (r['never'], 21, self.r_ground)])
        cl = TS.cancels(T, xt['cancel'])
        self.assertEqual([(c['req'], c['move_id']) for c in cl[:4]], [(r['hit'], STANCE), (r['exit'], first + 2),
                                                                     (r['hit'], on_hit), (r['always'], split)])
        self.assertEqual((cl[1]['win_start'], cl[1]['win_end'], cl[1]['start_frame'], cl[1]['extradata']),
                         (1, 32767, 1, 11))
        self.assertEqual((cl[4]['direction'], cl[4]['win_start']), (M.LIST_END, 50))
        # XDA: no hit (X's hitlevel's high half), the stance rows at 18, the exit's properties at 18 / 20 / the start,
        # X's 0x80f8 left out
        self.assertEqual(names[xda['name']], 'Ka_6rkDA')
        self.assertEqual((xda['hitbox'], xda['first_active'], xda['last_active'], xda['hitlevel'], xda['voiceclip'],
                          xda['hit_condition'], xda['anim'], xda['transition']),
                         (bytes(4), 0, 0, 0x200000, None, self.nohit, x['anim'], x['transition']))
        cl = TS.cancels(T, xda['cancel'])
        self.assertEqual([(c['req'], c['win_start'], c['win_end'], c['start_frame'], c['move_id']) for c in cl[:2]],
                         [(r['back1'], 18, 18, 18, STANCE), (r['back2'], 18, 18, 18, STANCE)])
        self.assertEqual(cl[2]['direction'], M.LIST_END)
        self.assertEqual([(p['frame'], p['id'], p['value']) for p in TS.props(T, xda['extraprops'])],
                         [(12, 0x801f, 7), (18, 0x8256, 0), (18, 0x8081, 0), (18, 0x812A, 360), (20, 0x812A, 4096),
                          (0x8001, 0x82ED, 0)])
        # a move that has its twin already is left alone
        self.assertEqual(TS.add_tag_assault(T, tpl, first, lambda i: names[T['moves'][i]['name']], string), [])

    def test_no_triple(self):
        d = self.donor()
        for x in (1, 4):
            d.t['moves'][x]['cancel'] = d.cancels(d.end())
        with self.assertRaises(ValueError):
            TS.assault_template(d.t, d.aliases)

    def test_bound_shape(self):
        d = self.donor()
        self.assertEqual(TS.bound_shape(d.t), (self.reqs['bound'], self.r_air))


class Slams(unittest.TestCase):
    """The converter's conv_hitconds on a made-up Tekken 7 hit-condition table (its other parts stubbed)."""

    def setUp(self):
        t7 = Moveset()
        t7.game = 't7'
        t7.tables = {'requirements': [], 'hit_conditions': []}
        self.t7 = t7
        T = Tables()
        self.T = T
        self.bound = T.reaction(10)                     # the donor's bound reaction (its default: move 10)
        self.after = T.reaction(11)                     # the donor's air hit after a bound
        self.slam = T.reaction(10)                      # a slam, mapped to the bound reaction's move
        self.air, self.ground, self.screw = T.reaction(12), T.reaction(13), T.reaction(14)
        self.r466, self.r467, self.r_none = T.req((466, 0)), T.req((467, 0)), T.req()
        c = moveset.Converter.__new__(moveset.Converter)
        c.k, c.T, c.stats = t7, T.t, collections.Counter()
        c.bound_reaction, c.bound_req, c.after_bound_reaction = self.bound, self.r467, self.after
        req_of = {}
        c.conv_reqs = lambda i: (req_of[i], True)
        c.conv_reaction = lambda rx: rx
        self.c, self.req_of = c, req_of

    def t7_hits(self, *entries):
        """[(Tekken 7 requirement ids, the converted reaction)] -> the Tekken 7 list's index."""
        start = len(self.t7.tables['hit_conditions'])
        for conds, rx in entries:
            ri = len(self.t7.tables['requirements'])
            self.t7.tables['requirements'] += [dict(condition=c, param=0) for c in conds] + [
                dict(condition=M.END_REQ['t7'], param=0)]
            self.req_of[ri] = {(): self.r_none, (614,): self.r466, (615,): self.r467}[tuple(conds)]
            self.t7.tables['hit_conditions'].append(dict(req=ri, damage=20, reactions=rx))
        return start

    def converted(self, i):
        return [(h['req'], h['reactions']) for h in TS.hits(self.c.T, self.c.conv_hitconds(i))]

    def test_three_entries(self):
        i = self.t7_hits(([614], self.slam), ([614], self.air), ([], self.ground))
        self.assertEqual(self.converted(i), [(self.r467, self.bound), (self.r466, self.air),
                                             (self.r_none, self.ground)])
        self.assertEqual(self.c.stats['Tekken 7 air slams -> bounds (467)'], 1)

    def test_two_entries(self):
        i = self.t7_hits(([614], self.slam), ([], self.ground))
        self.assertEqual(self.converted(i), [(self.r467, self.bound), (self.r466, self.after),
                                             (self.r_none, self.ground)])

    def test_screw_unchanged(self):
        i = self.t7_hits(([615], self.screw), ([614], self.air), ([], self.ground))
        self.assertEqual(self.converted(i), [(self.r467, self.bound), (self.r466, self.air),
                                             (self.r_none, self.ground)])
        self.assertEqual(self.c.stats['Tekken 7 air slams -> bounds (467)'], 0)

    def test_air_hit_unchanged(self):
        i = self.t7_hits(([614], self.air), ([], self.ground))
        self.assertEqual(self.converted(i), [(self.r466, self.air), (self.r_none, self.ground)])

    def test_slam_after_a_bound(self):
        i = self.t7_hits(([615], self.screw), ([614], self.slam), ([], self.ground))
        self.assertEqual(self.converted(i), [(self.r467, self.bound), (self.r466, self.after),
                                             (self.r_none, self.ground)])


class Landing(unittest.TestCase):
    def setUp(self):
        d = Tables()
        self.d = d
        up, down = d.move('landing face up'), d.move('landing face down')
        d.aliases[GROUND_UP - 0x8000], d.aliases[GROUND_DOWN - 0x8000] = up, down
        self.ao_hlp, self.ao, self.ut_hlp, self.ut = (d.move(n) for n in ('AoHlp', 'Ao', 'UtHlp', 'Ut'))
        self.tech_up, self.tech_down, self.attack = d.move('tech'), d.move('tech face down'), d.move('attack')
        self.help_req, self.tag_req, self.side_req = d.req((156, 0), (481, 0)), d.req((481, 0)), d.req((146, 0))

        def landing(trans, ws, we, hlp_ws, hlp, plain, tech, extra=()):
            rows = [dict(req=self.side_req, win_start=1, win_end=30, move_id=up)]
            rows += [dict(button=TS.HELP_BUTTON, req=self.help_req, win_start=hlp_ws, win_end=we, start_frame=we,
                          move_id=hlp)]
            rows += [dict(button=b, req=self.side_req, win_start=ws, win_end=we, start_frame=we, move_id=tech)
                     for b in (0x20000003, 0x2000000c)]
            rows += [dict(button=TS.TAG_BUTTON, req=self.tag_req, win_start=ws, win_end=we, start_frame=we,
                          move_id=plain)] + list(extra) + [d.end(60)]
            return d.move('reaction', transition=trans, cancel=d.cancels(*rows))
        landing(GROUND_UP, 34, 43, 34, self.ao_hlp, self.ao, self.tech_up)
        landing(GROUND_UP, 31, 40, 31, self.ao_hlp, self.ao, self.tech_up)
        landing(GROUND_DOWN, 31, 40, 31, self.ut_hlp, self.ut, self.tech_down)
        landing(GROUND_UP, 24, 38, 29, self.ao_hlp, self.ao, self.tech_up)          # a wall reaction
        self.tpl = TS.landing_template(d.t, d.aliases)
        self.T = copy.deepcopy(d.t)
        self.first = len(self.T['moves'])

    def converted(self, trans, ws, we, tech=None, sf=None, extra=()):
        tech = self.tech_up if tech is None else tech
        rows = [dict(direction=0, button=0, req=self.side_req, extradata=0, win_start=1, win_end=30, start_frame=1,
                     move_id=0, option=0)]
        rows += [dict(direction=0, button=b, req=self.side_req, extradata=0, win_start=ws, win_end=we,
                      start_frame=we if sf is None else sf, move_id=tech, option=0) for b in (0x20000003, 0x2000000c)]
        rows += list(extra) + [dict(direction=M.LIST_END, button=0, req=0, extradata=0, win_start=60, win_end=32767,
                                    start_frame=60, move_id=0, option=0)]
        i = TS.write(self.T, 'moves', [dict(self.d.t['moves'][0], transition=trans,
                                            cancel=TS.write(self.T, 'cancels', rows))])
        return i

    def rows_of(self, i):
        return [(c['button'], c['win_start'], c['win_end'], c['start_frame'], c['move_id'], c['req'])
                for c in TS.cancels(self.T, self.T['moves'][i]['cancel'])]

    def test_template(self):
        self.assertEqual(self.tpl['tech'], {0x20000003, 0x2000000c})
        self.assertEqual(self.tpl['last'], 10)
        self.assertEqual(self.tpl['pair'], {self.ao_hlp: self.ao, self.ut_hlp: self.ut})
        self.assertEqual(self.tpl['side'][self.d.aliases[GROUND_DOWN - 0x8000]], self.ut_hlp)

    def test_face_up(self):
        i = self.converted(GROUND_UP, 34, 43)
        old = copy.deepcopy(self.T['cancels'])
        self.assertEqual(TS.add_landing_rows(self.T, self.tpl, self.first, self.d.aliases), 1)
        self.assertEqual(self.T['cancels'][:len(old)], old)
        self.assertEqual(self.rows_of(i), [
            (0, 1, 30, 1, 0, self.side_req),
            (TS.HELP_BUTTON, 34, 43, 43, self.ao_hlp, self.help_req),
            (0x20000003, 34, 43, 43, self.tech_up, self.side_req),
            (0x2000000c, 34, 43, 43, self.tech_up, self.side_req),
            (TS.TAG_BUTTON, 34, 43, 43, self.ao, self.tag_req),
            (0, 60, 32767, 60, 0, 0)])

    def test_face_down(self):
        i = self.converted(GROUND_DOWN, 31, 40, tech=self.tech_down)
        TS.add_landing_rows(self.T, self.tpl, self.first, self.d.aliases)
        self.assertEqual([r[4] for r in self.rows_of(i) if r[0] in (TS.HELP_BUTTON, TS.TAG_BUTTON)],
                         [self.ut_hlp, self.ut])

    def test_long_window(self):
        i = self.converted(GROUND_UP, 24, 38)
        TS.add_landing_rows(self.T, self.tpl, self.first, self.d.aliases)
        self.assertEqual([r[1:4] for r in self.rows_of(i) if r[0] == TS.HELP_BUTTON], [(29, 38, 38)])
        self.assertEqual([r[1:4] for r in self.rows_of(i) if r[0] == TS.TAG_BUTTON], [(24, 38, 38)])

    def test_unchanged(self):
        has_row = dict(direction=0, button=TS.HELP_BUTTON, req=self.help_req, extradata=0, win_start=34, win_end=43,
                       start_frame=43, move_id=self.ao_hlp, option=0)
        a = self.converted(GROUND_UP, 34, 43, extra=[has_row])
        b = self.converted(GROUND_UP, 34, 43, tech=self.attack)              # a string: those buttons, no tech
        c = self.converted(GROUND_UP, 34, 43)
        self.T['moves'][c]['cancel'] = self.T['moves'][0]['cancel']          # no tech rows at all
        before = [self.T['moves'][i]['cancel'] for i in (a, b, c)]
        self.assertEqual(TS.add_landing_rows(self.T, self.tpl, self.first, self.d.aliases), 0)
        self.assertEqual([self.T['moves'][i]['cancel'] for i in (a, b, c)], before)

    def test_shared_list_copied_once(self):
        a = self.converted(GROUND_UP, 34, 43)
        b = TS.write(self.T, 'moves', [dict(self.T['moves'][a])])
        n = len(self.T['cancels'])
        self.assertEqual(TS.add_landing_rows(self.T, self.tpl, self.first, self.d.aliases), 2)
        self.assertEqual(self.T['moves'][a]['cancel'], self.T['moves'][b]['cancel'])
        self.assertEqual(len(self.T['cancels']), n + 6)                     # one new list: 4 rows + 2

    def test_no_tag_crash_rows(self):
        d = Tables()
        d.move('stance', cancel=d.cancels(d.end()))
        with self.assertRaises(ValueError):
            TS.landing_template(d.t, d.aliases)


class Hubs(unittest.TestCase):
    def test_kept_hub(self):
        tag_in, own, item = 5, 6, 7
        rows = [dict(direction=0, move_id=m) for m in (tag_in, tag_in, tag_in, own, item)] + [
            dict(direction=M.LIST_END, move_id=0)]
        is_tag, is_item = (lambda m: m == tag_in), (lambda m: m == item)
        self.assertEqual([r['move_id'] for r in TS.kept_hub(rows, is_tag, is_item)], [tag_in] * 3 + [own, 0])
        self.assertIsNone(TS.kept_hub(rows[2:], is_tag, is_item))           # one of three
        self.assertIsNone(TS.kept_hub(rows[1:], is_tag, is_item))           # half: not most


if __name__ == '__main__':
    unittest.main()
