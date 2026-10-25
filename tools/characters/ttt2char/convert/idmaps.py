"""Tekken 7 -> TTT2 id maps, derived on the player's machine by aligning movesets of the two games.

Requirement and property ids (and hit location codes) are ordered lists in both games, Tekken 7's with insertions.
Moves with the same name exist in both games (all of Heihachi's, and the generic moves every fighter has), so for
each pair of same-named moves the linked lists (cancels -> requirement lists, extra properties, begin/end
properties, hit conditions -> requirement lists and reactions) are aligned element by element and every (Tekken 7
value, TTT2 value) pair is counted. Each namespace is then modelled as the heaviest strictly increasing chain of
well-supported pairs: ids on the chain map as observed, ids between two anchors with the same offset map with that
offset, strong pairs off the chain are kept as exceptions; anything else has no mapping.
"""
import bisect
import collections
import difflib

from ..movesets import REACTION_MOVE_FIELDS

END_REQ = {'t7': 881, 'ttt2': 690}     # the requirement / property list end marker
GROUP, GROUP_END, LIST_END = 0x800B, 0x800C, 0x8000


# ------------------------------------------------------------------------------------------------ lists
def reqs(m, i):
    if i is None:
        return []
    end = END_REQ[m.game]
    return [(r['condition'], r['param']) for _, r in m.iter_list('requirements', i, lambda r: r['condition'] == end)
            if r['condition'] != end]


def cancels(m, i, table='cancels', end=LIST_END):
    if i is None:
        return []
    return [c for _, c in m.iter_list(table, i, lambda c: c['direction'] == end)]


def xps(m, i):
    if i is None:
        return []
    return [(x['frame'], x['id'], x['value'])
            for _, x in m.iter_list('extra_move_properties', i, lambda x: x['id'] == 0 and x['frame'] == 0)]


def props(m, table, i):
    if i is None:
        return []
    end = END_REQ[m.game]
    return [(reqs(m, p['req']), p['id'], p['value']) for _, p in m.iter_list(table, i, lambda p: p['id'] == end)]


def hitconds(m, i):
    """A hit-condition list ends with the entry whose requirement list is empty."""
    if i is None:
        return []
    out, t = [], m.tables['hit_conditions']
    while i < len(t):
        rl = reqs(m, t[i]['req'])
        out.append((rl, t[i]['damage'], t[i]['reactions']))
        if not rl:
            break
        i += 1
    return out


def voices(m, i):
    if i is None:
        return []
    return [v['value'] for _, v in m.iter_list('voiceclips', i, lambda v: v['value'] == 0xFFFFFFFF)]


def target(m, mid):
    """The name of a move id's move (through the alias table for ids >= 0x8000)."""
    r = m.resolve_alias(mid)
    if r is None or r >= len(m.tables['moves']):
        return '?%#x' % mid
    return m.move_name(r)


# ------------------------------------------------------------------------------------------------ evidence
class Evidence:
    """pairs[(a, b)] = how often a (Tekken 7) and b (TTT2) were seen in the same place."""

    def __init__(self):
        self.pairs = collections.Counter()

    def add(self, a, b, n=1):
        self.pairs[(a, b)] += n


def _req_pair(ra, rb, ev):
    if len(ra) == len(rb):
        for (c1, _), (c2, _) in zip(ra, rb):
            ev['req'].add(c1, c2)
    else:
        ev['reqlist_unequal'].add(tuple(c for c, _ in ra), tuple(c for c, _ in rb))


def align(a, b, ev):
    """Count the value pairs of the same-named moves of a (Tekken 7) and b (TTT2) into ev (key -> Evidence)."""
    A, B = a.tables, b.tables
    na = {a.move_name(i): i for i in range(len(A['moves']))}
    nb = {b.move_name(i): i for i in range(len(B['moves']))}

    # a row: its input and target. Directions below 0x8000 count too: a Tekken 7-only automatic row
    # `0 [806] -> sWALK_02L` in front of the neutral row `0x20 [0] -> sWALK_02L` both games have once paired with
    # TTT2's, which made 806 "always true" (convert/moveset.py REQ_FALSE)
    def sig(m):
        return lambda c: ((c['direction'], target(m, c['move_id'])) if c['direction'] < 0x8000 else
                          target(m, c['move_id']) if c['direction'] in (0x8001, 0x8002) else '', c['button'])
    sig_a, sig_b = sig(a), sig(b)
    for i, j in ((na[n], nb[n]) for n in na if n in nb):
        x, y = A['moves'][i], B['moves'][j]
        for p, q in zip(x['hitbox'], y['hitbox']):
            ev['hitbox.byte'].add(p, q)
        ca, cb = cancels(a, x['cancel']), cancels(b, y['cancel'])
        matcher = difflib.SequenceMatcher(None, [sig_a(c) for c in ca], [sig_b(c) for c in cb], autojunk=False)
        for blk in matcher.get_matching_blocks():
            for t in range(blk.size):
                _req_pair(reqs(a, ca[blk.a + t]['req']), reqs(b, cb[blk.b + t]['req']), ev)
        xa, xb = xps(a, x['extraprops']), xps(b, y['extraprops'])
        if len(xa) == len(xb):
            for p, q in zip(xa, xb):
                ev['xp.id'].add(p[1], q[1])
        else:
            ev['xplist_unequal'].add(tuple(p[1] for p in xa), tuple(q[1] for q in xb))
        for table, key in (('move_begin_props', 'begin_props'), ('move_end_props', 'end_props')):
            pa, pb = props(a, table, x[key]), props(b, table, y[key])
            if len(pa) == len(pb):
                for p, q in zip(pa, pb):
                    ev['prop.id'].add(p[1], q[1])
                    _req_pair(p[0], q[0], ev)
        ha, hb = hitconds(a, x['hit_condition']), hitconds(b, y['hit_condition'])
        if len(ha) == len(hb):
            for p, q in zip(ha, hb):
                _req_pair(p[0], q[0], ev)
                if p[2] is not None and q[2] is not None:
                    rx, ry = A['reactions'][p[2]], B['reactions'][q[2]]
                    for f in REACTION_MOVE_FIELDS:
                        ev['react.move'].add(a.move_name(rx[f]) if rx[f] < len(A['moves']) else rx[f],
                                             b.move_name(ry[f]) if ry[f] < len(B['moves']) else ry[f])


# ------------------------------------------------------------------------------------------------ id maps
def anchors(ev, keys, lo, hi):
    """Evidence of several keys merged, Tekken 7 ids in [lo, hi): a -> (best b, its count, total, top 3)."""
    c = collections.defaultdict(collections.Counter)
    for k in keys:
        for (a, b), n in ev[k].pairs.items():
            if isinstance(a, int) and isinstance(b, int) and lo <= a < hi:
                c[a][b] += n
    out = {}
    for a, cc in c.items():
        (b, n), tot = cc.most_common(1)[0], sum(cc.values())
        out[a] = (b, n, tot, dict(cc.most_common(3)))
    return out


def chain(anc):
    """Heaviest chain increasing in both ids over the pairs that have at least 60 % of their id's evidence."""
    items = sorted((a, b, n) for a, (b, n, tot, alt) in anc.items() if n / tot >= 0.6)
    if not items:
        return []
    W, P = [0] * len(items), [-1] * len(items)
    for i, (a, b, n) in enumerate(items):
        W[i] = n
        for j in range(i):
            if items[j][1] < b and W[j] + n > W[i] and (b - items[j][1]) <= (a - items[j][0]) + 400:
                W[i] = W[j] + n
                P[i] = j
    i = max(range(len(items)), key=lambda k: W[k])
    best = []
    while i >= 0:
        best.append(items[i])
        i = P[i]
    return best[::-1]


class IdMap:
    def __init__(self, anc):
        self.anc = anc
        self.chain = chain(anc)
        self.ca = [a for a, b, n in self.chain]
        on_chain = set(self.ca)
        # strong pairs off the chain (renumbered ids) are kept as exceptions
        self.exc = {a: (b, n, tot) for a, (b, n, tot, alt) in anc.items()
                    if a not in on_chain and n >= 2 and n / tot >= 0.9}

    def lookup(self, x):
        """-> (TTT2 id or None, how it was found)"""
        if x in self.anc and x in set(self.ca):
            return self.anc[x][0], 'observed'
        if x in self.exc:
            return self.exc[x][0], 'observed-exception'
        k = bisect.bisect_left(self.ca, x)
        if k == 0:
            a2, b2, _ = self.chain[0]
            return (x + b2 - a2 if x + b2 - a2 >= 0 else None), 'extrapolated-below'
        if k == len(self.ca):
            a1, b1, _ = self.chain[-1]
            return x + b1 - a1, 'extrapolated-above'
        a1, b1, _ = self.chain[k - 1]
        a2, b2, _ = self.chain[k]
        if b1 - a1 == b2 - a2:
            return x + b1 - a1, 'interpolated'
        return None, 'gap'


def build_maps(ev):
    """(requirement map, property map) from the evidence; lists of unequal length are aligned in a second pass on
    the ids the first pass maps."""
    req = IdMap(anchors(ev, ['req'], 0, 0x8000))
    prop = IdMap(anchors(ev, ['req', 'xp.id', 'prop.id'], 0x8000, 0x10000))

    def known(x):
        b, how = (req if x < 0x8000 else prop).lookup(x)
        return b if how in ('observed', 'observed-exception', 'interpolated') else ('?', x)
    for key in ('reqlist_unequal', 'xplist_unequal'):
        for (xs, ys), n in ev[key].pairs.items():
            matcher = difflib.SequenceMatcher(None, [known(x) for x in xs], list(ys), autojunk=False)
            for op, i1, i2, j1, j2 in matcher.get_opcodes():
                if op == 'replace' and i2 - i1 == j2 - j1:
                    for x, y in zip(xs[i1:i2], ys[j1:j2]):
                        ev['refined'].add(x, y, n)
    req = IdMap(anchors(ev, ['req', 'refined'], 0, 0x8000))
    prop = IdMap(anchors(ev, ['req', 'xp.id', 'prop.id', 'refined'], 0x8000, 0x10000))
    req.exc[END_REQ['t7']] = (END_REQ['ttt2'], 0, 0)
    return req, prop


def reaction_remap(ev):
    """Tekken 7 move name -> the TTT2 move most often in the same reaction slot (where the names differ)."""
    c = collections.defaultdict(collections.Counter)
    for (x, y), n in ev['react.move'].pairs.items():
        if isinstance(x, str) and isinstance(y, str) and x != y:
            c[x][y] += n
    return {x: cc.most_common(1)[0][0] for x, cc in c.items()}


def hitbox_remap(ev, min_count=2):
    """Hit location codes (a move's four hitbox bytes) that Tekken 7 numbers differently: the heaviest chain over
    the observed (Tekken 7, TTT2) code pairs (0, no location, left out) that increases in the Tekken 7 code and
    never decreases in the TTT2 code (Tekken 7 has more locations: several may become one). Codes on the chain map
    to their partner; the others stay as they are. -> {code: TTT2 code} of the codes that change."""
    items = sorted((a, b, n) for (a, b), n in ev['hitbox.byte'].pairs.items() if a and b and n >= min_count)
    if not items:
        return {}
    W, P = [0] * len(items), [-1] * len(items)
    for i, (a, b, n) in enumerate(items):
        W[i] = n
        for j in range(i):
            if items[j][0] < a and items[j][1] <= b and W[j] + n > W[i]:
                W[i] = W[j] + n
                P[i] = j
    i = max(range(len(items)), key=lambda k: W[k])
    out = {}
    while i >= 0:
        a, b, _ = items[i]
        if a != b:
            out[a] = b
        i = P[i]
    return out


def ttt2_usage(movesets):
    """Requirement and property ids that some TTT2 moveset uses: {'req': set, 'prop': set}."""
    use = {'req': set(), 'prop': set()}
    for m in movesets:
        for r in m.tables['requirements']:
            use['req' if r['condition'] < 0x8000 else 'prop'].add(r['condition'])
        for t in ('extra_move_properties', 'move_begin_props', 'move_end_props'):
            for x in m.tables[t]:
                use['prop' if x['id'] >= 0x8000 else 'req'].add(x['id'])
    return use
