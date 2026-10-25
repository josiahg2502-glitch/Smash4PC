"""Unit tests of the added fighter's movement: TTT2's tag-out states (tagsystem.TagStates), the converter's movement
counterparts and transitions, and the id maps' row alignment, on made-up tables (no game files needed).

    python3 -m pytest -q tools/characters/tests/test_movement.py
"""
import collections
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_tagsystem import Tables  # noqa: E402
from ttt2char.convert import idmaps as M  # noqa: E402
from ttt2char.convert import moveset, tagsystem as TS  # noqa: E402
from ttt2char.movesets import Moveset  # noqa: E402

STANCE = 0x8001
B, DB, N = 0x10, 0x2, 0x20


def moveset_of(tables, game='ttt2'):
    """A Moveset over a Tables' rows (move names as a string block)."""
    m = Moveset()
    m.game, m.aliases = game, list(tables.aliases)
    buf, offs = bytearray(), []
    for n in tables.names:
        offs.append(len(buf))
        buf += n.encode('latin1') + b'\0'
    m.namebuf = bytes(buf)
    m.tables = dict(tables.t)
    m.tables['moves'] = [dict(r, name=offs[r['name']]) for r in tables.t['moves']]
    return m


def tag_group(t, tag_out):
    """A group with TTT2's tag-out row (TAG -> tag_out) and an automatic row."""
    start = len(t.t['group_cancels'])
    base = dict(direction=0, button=0, req=t.req((0, 0)), extradata=0, win_start=1, win_end=32767, start_frame=1,
                move_id=0, option=0)
    t.t['group_cancels'] += [dict(base, button=TS.TAG_BUTTON, move_id=tag_out), dict(base, move_id=STANCE),
                             dict(base, direction=M.GROUP_END)]
    return start


def ttt2_fighter(prefix, step_tags=False):
    """A made-up TTT2 moveset: stance, walk back and crouch walk back tag out (by a group; the crouch walk by a row
    of its own), an attack and a step do not (unless step_tags)."""
    t = Tables()
    stance, walk, cwalk, attack, step, out = range(6)
    group = tag_group(t, out)
    g = dict(direction=M.GROUP, move_id=group)
    t.move(prefix + '_sKAM00_', cancel=t.cancels(g, dict(direction=B, move_id=walk), t.end()))
    t.move(prefix + '_sWALKB', cancel=t.cancels(g, t.end()), transition=STANCE)
    t.move(prefix + '_cWALKB', cancel=t.cancels(dict(button=TS.TAG_BUTTON, move_id=out), t.end()))
    t.move(prefix + '_lp00', cancel=t.cancels(t.end()))
    t.move('Nj_step_00F', cancel=t.cancels(*([g] if step_tags else []), t.end()))
    t.move('Co_StChgOutB')
    t.aliases[1] = stance
    return moveset_of(t)


class TagStates(unittest.TestCase):
    def test_tag_out_moves(self):
        self.assertEqual(TS.tag_out_moves(ttt2_fighter('Ju')), {0, 1, 2})

    def test_state_name(self):
        self.assertEqual({TS.state_name(n) for n in ('La_sWALKB', 'sWALK_00B', 'Ka_sWALKB')}, {'sWALKB'})
        self.assertEqual(TS.state_name('Kz_st2cr_Rv'), TS.state_name('st2cr_00_Rv'))

    def test_states(self):
        st = TS.TagStates([ttt2_fighter('Ju'), ttt2_fighter('La'), ttt2_fighter('Pl', step_tags=True)])
        self.assertTrue(st('Ka_sWALKB'))                   # a generic state under the fighter's own prefix
        self.assertTrue(st('sWALK_00B'))
        self.assertTrue(st('Ju_cWALKB'))                   # a TTT2 name with tag out
        self.assertFalse(st('Ka_lp00'))                    # an attack
        # a third of the movesets tag out of the step, so its state is generic, but TTT2's own move of that name
        # mostly does not: the name wins
        self.assertEqual(st.common, {'sKAM', 'sWALKB', 'cWALKB', 'stepF'})
        self.assertFalse(st('Nj_step_00F'))
        self.assertTrue(st('Ka_step_00F'))
        self.assertTrue(TS.TagStates([ttt2_fighter('Ju', step_tags=True)])('Nj_step_00F'))


class Movement(unittest.TestCase):
    """The converter's is_state, state_counterpart and own_transitions (its other parts stubbed)."""

    def setUp(self):
        # Tekken 7 fighter: 0 stance -b-> 1 walk back; -db-> 3 st2cr_00B (a generic move) -> 4 its crouch walk back;
        # 2 a dash reached by a button only; 5 an attack; 6 a state TTT2's donor has no move of
        k = Tables()
        k.move('Ka_sKAM00_', cancel=k.cancels(dict(direction=B, move_id=1), dict(direction=DB, move_id=3),
                                              dict(button=1, move_id=2), dict(button=2, move_id=5),
                                              dict(direction=N, move_id=6), k.end()))
        k.move('Ka_sWALKB', cancel=k.cancels(k.end()), transition=STANCE)
        k.move('Ka_sDASHF', cancel=k.cancels(k.end()), transition=STANCE)
        k.move('st2cr_00B', cancel=k.cancels(k.end()), transition=4)
        k.move('Kz_cWALKB', cancel=k.cancels(k.end()), transition=4)
        k.move('Ka_lp00', cancel=k.cancels(k.end()), transition=STANCE)
        k.move('Ka_sDASHB', cancel=k.cancels(k.end()), transition=STANCE)
        k.aliases[1] = 0
        self.k = moveset_of(k, 't7')
        # donor: 0 stance, 1 walk back, 2 crouch walk back (tag out), 3 st2cr_00B -> 2 (its own crouch walk),
        # 4 st2cr_00F -> the stance alias, 5 tag out
        d = Tables()
        group = tag_group(d, 5)
        g = dict(direction=M.GROUP, move_id=group)
        d.move('Ju_sKAM00_', cancel=d.cancels(g, d.end()))
        d.move('Ju_sWALKB', cancel=d.cancels(g, d.end()))
        d.move('Ju_cWALKB', cancel=d.cancels(g, d.end()))
        d.move('st2cr_00B', cancel=d.cancels(d.end()), transition=2)
        d.move('st2cr_00F', cancel=d.cancels(d.end()), transition=STANCE)
        d.move('Co_StChgOutB')
        d.aliases[1] = 0
        self.d = moveset_of(d)
        c = moveset.Converter.__new__(moveset.Converter)
        c.k, c.d, c.stats = self.k, self.d, collections.Counter()
        c.T = {'moves': [dict(r) for r in self.d.tables['moves']]}
        states = {'sKAM', 'sWALKB', 'cWALKB', 'sDASHF', 'sDASHB', 'st2crB'}
        c.tag_states = lambda n: TS.state_name(n) in states
        c._reachable = c.donor_states = None
        c.cls = ['K', 'K', 'K', 'G', 'K', 'K', 'K']
        c.dn = {self.d.move_name(i): i for i in range(len(self.d.tables['moves']))}
        c.kn = {self.k.move_name(i): i for i in range(len(self.k.tables['moves']))}
        c.ttt2_only_generic = {'Co_StChgOutB'}
        c.newidx = {i: 10 + i for i in (0, 1, 2, 4, 5, 6)}
        c.aliases = list(self.d.aliases)
        c.aliases[1] = c.newidx[0]
        c.delta_move, c.react_remap, c.alloc_ok, c.queue = {}, {}, 0, []
        self.c = c

    def test_is_state(self):
        self.assertEqual([self.c.is_state(i) for i in range(7)], [True, True, False, True, True, False, True])

    def test_counterparts(self):
        cp = self.c.state_counterpart
        self.assertEqual((cp(1), cp(4)), (1, 2))          # the donor's walk back, crouch walk back
        self.assertEqual(cp(6), 0)                         # no donor move of the state: its stance
        self.assertIsNone(cp(2))                           # reached by a button only
        self.assertIsNone(cp(5))                           # an attack

    def test_no_tag_states(self):
        self.c.tag_states = None
        self.assertIsNone(self.c.state_counterpart(1))

    def test_own_transitions(self):
        self.c.own_transitions()
        self.assertEqual(self.c.T['moves'][3]['transition'], 14)      # the fighter's crouch walk, not Jun's
        self.assertEqual(self.c.T['moves'][4]['transition'], STANCE)  # the stance alias is the fighter's already
        self.assertEqual(self.c.stats['generic moves going on to the fighter\'s movement state, not the donor\'s'], 1)


class Alignment(unittest.TestCase):
    def test_direction_in_signature(self):
        """Rows on different directions never pair: Tekken 7's automatic row `0 [806] -> sWALK_02L` where TTT2 has
        the neutral row `0x20 [0] -> sWALK_02L` (it made Tekken 7's 806 TTT2's 0, "always true")."""
        a, b = Tables(), Tables()
        a_end, b_end = [dict(condition=M.END_REQ['t7'], param=0)], [dict(condition=M.END_REQ['ttt2'], param=0)]

        def req(t, end, *conditions):
            return TS.write(t.t, 'requirements', [dict(condition=c, param=p) for c, p in conditions] + end)
        req(a, a_end), req(b, b_end)                                    # list 0 (the list ends'): empty
        a.move('sWALK_01L', cancel=a.cancels(dict(direction=0, req=req(a, a_end, (806, 0)), move_id=1),
                                             dict(direction=N, req=req(a, a_end, (0, 0)), move_id=2), a.end()))
        b.move('sWALK_01L', cancel=b.cancels(dict(direction=N, req=req(b, b_end, (0, 0)), move_id=1),
                                             dict(direction=N, req=req(b, b_end, (0, 0)), move_id=2), b.end()))
        for t in (a, b):
            t.move('sWALK_02L')
            t.move('sKAM')
        ev = collections.defaultdict(M.Evidence)
        M.align(moveset_of(a, 't7'), moveset_of(b), ev)
        self.assertNotIn((806, 0), ev['req'].pairs)
        self.assertEqual(ev['req'].pairs[(0, 0)], 1)                   # the neutral rows into sKAM

    def test_806_false(self):
        self.assertIn(806, moveset.REQ_FALSE)


if __name__ == '__main__':
    unittest.main()
