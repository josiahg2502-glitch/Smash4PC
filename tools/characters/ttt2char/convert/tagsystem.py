"""TTT2's tag system on the added fighter's converted moves: Tag Assault, Tag Crash and bounds.

Tekken 7 has no tag, and TTT2 keeps its tag system in the movesets, as moves and cancels the converted moves lack:

* Tag Assault: a TTT2 bound move X (a hit condition on 467) has a cancel on the tag button during its startup into a
  twin XT: X with a hit condition on 468 in front (its reaction bounces the opponent for the partner) and a cancel on
  that hit into XDA, X's animation without a hit, whose properties bring the partner in (its Co_Change_Kam takes the
  tag-in kind 11) while it returns to the stance. The converter makes Tekken 7's screws bounds but has no XT or XDA
  to give them: add_tag_assault makes both, after the donor's own triples (assault_template).
* Bounds: where TTT2 has a bound, Tekken 7 often has an air slam instead (the same move of Heihachi's in both games:
  [614 airborne -> slam][614 -> air][default] in Tekken 7, [467 -> bound][466 -> air][default] in TTT2); the id maps
  make the slam the bound reaction without the 467 test, so it bounds on every air hit (TTT2 has no such entry).
  gated_slam gives such entries TTT2's shape.
* Tag Crash: a juggled fighter plays its reactions from the ATTACKER's moveset (sub_041570D8), and TTT2's landing
  reactions have a cancel on 2 + tag held, the partner in Rage, into the tag crash just before their tech (ukemi)
  rows, and one on tag into the plain tag-out after them. landing_template / add_landing_rows give the converted
  reactions the donor's.
* The partner's tag-in dispatcher (Co_Change_Kam: requirement 480 picks the tag-in kind) is a TTT2-only hub: kept_hub
  keeps the donor's list where Tekken 7's leads to none of the fighter's own moves.
* Tag out: TTT2's movesets have one tag-out group each (TAG -> Co_StChgOut{F,B,L,R}[Jmp], Jun 1625, Lars 1612) in
  every movement state (standing, walks, dashes, backdash, crouch, crouch walks, crouch starts, rising, side steps;
  57 of Jun's moves); a converted move gets it with the donor's other TTT2-only rows when it has a donor counterpart
  (moveset.py merge_donor_extras). The fighter's own walks, dashes and crouch starts (Ka_sWALKB, Kz_cWALKB,
  sWALK_00B, ...) are movement states by TTT2's own movesets: TagStates.

The functions work on tables (a moveset's `tables` or the converter's T: {table name: rows}) and never change a list
in place (lists are shared through the converter's cache, by several moves and by Tekken 7 itself): a list that
changes is written anew and the move repointed. Every row they add is learnt from the donor's moveset by majority;
only the requirement ids and buttons that identify the rows are written down here (TTT2's requirement evaluator
sub_0404FF30, its jump table at 0x0405010C).
"""
import collections
import re

from . import idmaps as M

TT_END = M.END_REQ['ttt2']
# requirements: the opponent is airborne; ... and a bound is still available (the juggle's bound counter, player+204
# plus opponent+480); ... and the partner can come in for a Tag Assault (player+172 able to tag, opponent+7808 /
# +1148 / +1152 in the bound state); XT's exit into XDA (the same partner and opponent tests)
AIR, BOUND, BOUND_TAG, ASSAULT_EXIT = 466, 467, 468, 488
TAG_BUTTON = 0x40000100              # the tag button, alone
TAG_OUT = 'Co_StChgOut'              # the tag-out moves (F, B, L, R, and their Jmp)
TAG_PRESSED = 0x100
HELP_BUTTON = 0x20400                # 2 + tag held: the Tag Crash
# frames of a property row from here on are not frame numbers (0x4001 every frame from 1, 0x8001 at the start)
FRAME_FLAGS = 0x4000
# move fields XT and XDA take from the donor's triples (X's own value, or the one the donor's give them); the others
# are X's (its animation, transition and further cancel lists: TTT2's twins play X) or are made here
VALUE_FIELDS = ('vuln', 'hitlevel', 'c2_int', 'c3_int', 'c4_int', 'm_56', 'anim_len', 'airborne_start',
                'airborne_end', 'ground_fall', 'bitfield', 'hitbox', 'first_active', 'last_active', 'm_a8',
                'distance')
LIST_FIELDS = ('voiceclip', 'begin_props', 'end_props')
SAME = 'same'                        # (a learnt field) X's own
# ... or X's own with its low half cleared: TTT2's XDAs have X's hitlevel so in all 616 triples of its motion rows
# (0x200217 -> 0x200000 in the donors' item moves, 0x2C0217 -> 0x2C0000 ...), where a value of 0 fits only the moves
# without a high half
HIGH, HIGH_HALF = 'high half', 0xFFFF0000


# ------------------------------------------------------------------------------------------------ lists
def chain(table, i, last):
    """The rows of the list at row i of `table`, up to and including the first one `last` accepts."""
    out = []
    while i is not None and i < len(table):
        out.append(table[i])
        if last(table[i]):
            break
        i += 1
    return out


def reqs(tables, i):
    return [(r['condition'], r['param']) for r in chain(tables['requirements'], i, lambda r: r['condition'] == TT_END)
            if r['condition'] != TT_END]


def has(tables, i, condition):
    return any(c == condition for c, _ in reqs(tables, i))


def cancels(tables, i):
    return chain(tables['cancels'], i, lambda c: c['direction'] == M.LIST_END)


def hits(tables, i):
    """A hit-condition list ends with the entry whose requirement list is empty."""
    return chain(tables['hit_conditions'], i, lambda h: not reqs(tables, h['req']))


def props(tables, i):
    """A move's property rows, without the list's end row."""
    return [x for x in chain(tables['extra_move_properties'], i, lambda x: x['id'] == 0 and x['frame'] == 0)
            if not (x['id'] == 0 and x['frame'] == 0)]


def voices(tables, i):
    return [v['value'] for v in chain(tables['voiceclips'], i, lambda v: v['value'] == 0xFFFFFFFF)]


def begin_end(tables, table, i):
    return [(reqs(tables, p['req']), p['id'], p['value']) for p in chain(tables[table], i, lambda p: p['id'] == TT_END)]


LIST_CONTENT = {'voiceclip': voices, 'begin_props': lambda t, i: begin_end(t, 'move_begin_props', i),
                'end_props': lambda t, i: begin_end(t, 'move_end_props', i)}


def resolve(aliases, mid):
    return aliases[mid - 0x8000] if mid >= 0x8000 and mid - 0x8000 < len(aliases) else mid


def write(tables, table, rows):
    """A new list (the rows appended) -> its index."""
    start = len(tables[table])
    tables[table] += rows
    return start


def is_automatic(c):
    """A cancel taken by the game (on hit, on block, when the move ends ...), not by the player's input."""
    return c['direction'] == 0 and c['button'] == 0


def majority(votes, name):
    if not votes:
        raise ValueError('the donor has no %s to learn from' % name)
    return votes.most_common(1)[0][0]


def plain_frame(f):
    return f < FRAME_FLAGS


# ------------------------------------------------------------------------------------------------ bounds
def bound_shape(tables):
    """The donor's bound hit condition: (the requirement list of its 467 entries, the reaction of the air entry
    (466) that follows one: what an air hit plays once the bound is used up), each the donor's most common."""
    req, after = collections.Counter(), collections.Counter()
    for k, h in enumerate(tables['hit_conditions']):
        if h['reactions'] is None or not has(tables, h['req'], BOUND):
            continue
        req[h['req']] += 1
        nxt = tables['hit_conditions'][k + 1] if k + 1 < len(tables['hit_conditions']) else None
        if nxt is not None and nxt['reactions'] is not None and [c for c, _ in reqs(tables, nxt['req'])] == [AIR]:
            after[nxt['reactions']] += 1
    return majority(req, 'bound hit conditions (467)'), majority(after, 'air hit after a bound (466)')


def same_reaction(tables, a, b):
    """Whether reaction rows a and b play the same move on a standing hit (their 'default' move)."""
    if a is None or b is None:
        return False
    return tables['reactions'][a]['default'] == tables['reactions'][b]['default']


def gated_slam(tables, entry, before, bound_req, bound_reaction, after_reaction, alone):
    """A Tekken 7 air slam's hit-condition entry (converted: its 466 requirement list, the bound reaction) -> TTT2's
    entries for it: [467 -> bound][the air entry], as TTT2 Heihachi's same moves have (alone: Tekken 7 has no air
    entry after the slam (Negan's, Geese's, Noctis's): the air entry made, with the donor's reaction after a bound);
    after a bound entry already in the list (`before`, the entries converted so far), only the air entry."""
    if any(has(tables, e['req'], BOUND) for e in before):
        return [dict(entry, reactions=after_reaction)]
    out = [dict(entry, req=bound_req, reactions=bound_reaction)]
    if alone:
        out.append(dict(entry, reactions=after_reaction))
    return out


# ------------------------------------------------------------------------------------------------ Tag Assault
def triples(tables, aliases):
    """The donor's Tag Assault moves: [(X, XT, XDA, X's tag row, XT's exit row)] (move indices and cancel rows)."""
    out = []
    moves = tables['moves']
    for i, x in enumerate(moves):
        if not any(has(tables, h['req'], BOUND) for h in hits(tables, x['hit_condition'])):
            continue
        tag = [c for c in cancels(tables, x['cancel']) if c['direction'] == 0 and c['button'] == TAG_BUTTON]
        if not tag:
            continue
        j = resolve(aliases, tag[0]['move_id'])
        if j >= len(moves) or not any(has(tables, h['req'], BOUND_TAG)
                                      for h in hits(tables, moves[j]['hit_condition'])):
            continue
        exit_ = [c for c in cancels(tables, moves[j]['cancel'])
                 if c['direction'] < M.LIST_END and has(tables, c['req'], ASSAULT_EXIT)]
        if not exit_ or resolve(aliases, exit_[0]['move_id']) >= len(moves):
            continue
        out.append((i, j, resolve(aliases, exit_[0]['move_id']), tag[0], exit_[0]))
    return out


def learn_fields(votes, tables, x, twin, kind):
    """Counts, per move field, the rules that give the donor's twin (XT or XDA) its value: X's (SAME), X's high half
    (HIGH), the value itself; a twin can count for several."""
    for f in VALUE_FIELDS:
        v = votes['%s %s' % (kind, f)]
        v[('value', twin[f])] += 1
        v[SAME] += twin[f] == x[f]
        v[HIGH] += isinstance(x[f], int) and twin[f] == x[f] & HIGH_HALF
    for f in LIST_FIELDS:
        same = LIST_CONTENT[f](tables, twin[f]) == LIST_CONTENT[f](tables, x[f])
        votes['%s %s' % (kind, f)][SAME if same else ('value', None if twin[f] is None else 'other')] += 1


def field_rule(votes, kind, f):
    """X's value (SAME) unless the donor's twins mostly have another; then the most common other one, or X's high half
    (HIGH) where that gives more of them theirs; a list the twins have but X's not: X's."""
    v = votes['%s %s' % (kind, f)]
    values = collections.Counter({r: n for r, n in v.items() if r not in (SAME, HIGH)})
    if v[SAME] * 2 >= sum(values.values()):
        return SAME
    best, n = values.most_common(1)[0]
    if v[HIGH] > n:
        return HIGH
    return SAME if best[1] == 'other' else best[1]


def row_kind(tables, c):
    """What tells X's automatic cancels apart for XT's exit row: the move they go to (as written: the stance is an
    alias) and their extradata's value."""
    return c['move_id'], None if c['extradata'] is None else tables['cancel_extras'][c['extradata']]['value']


def assault_template(tables, aliases):
    """The donor's Tag Assault, learnt from its own X -> XT -> XDA triples (the most common form of each part):
    X's tag row (requirements, extradata, option; window from frame 1 to first_active - 1, as TTT2's), XT's 468 entry
    (requirements, reaction), whether XT keeps X's automatic cancels and which kinds (row_kind) before its exit row,
    the exit row, XDA's rows back to the stance (relative to the frame they are at, `leave` frames after X's last active
    one), its hit list, the properties it adds (relative alike) and the ones of X it leaves out, and every other field
    XT and XDA take from X or not. Raises ValueError if the donor has none."""
    found = triples(tables, aliases)
    if not found:
        raise ValueError('the donor has no Tag Assault moves to learn from (a bound move with a tag cancel into a '
                         'twin that hits with 468 and exits with 488): pick another donor')
    votes = collections.defaultdict(collections.Counter)
    seen, left = collections.Counter(), collections.Counter()      # X's property ids; ... the ones XDA leaves out
    kept, before = collections.Counter(), collections.Counter()     # X's automatic rows in XT; ... before its exit
    moves = tables['moves']
    for i, j, k, tag, exit_ in found:
        x, xt, xda = moves[i], moves[j], moves[k]
        fa, la = x['first_active'], x['last_active']
        votes['tag'][(tag['req'], tag['extradata'], tag['option'], tag['win_start'], tag['win_end'] - fa,
                      tag['start_frame'] - fa)] += 1
        h = [h for h in hits(tables, xt['hit_condition']) if has(tables, h['req'], BOUND_TAG)][0]
        votes['assault'][(h['req'], h['reactions'])] += 1
        votes['exit'][(exit_['req'], exit_['extradata'], exit_['option'], exit_['win_start'], exit_['win_end'],
                       exit_['start_frame'])] += 1
        # XT's cancels: X's automatic ones kept ('separate': before or after the exit row, by kind), folded into the
        # exit row's requirements ('merged') or left out ('alone')
        auto = [c for c in cancels(tables, x['cancel']) if is_automatic(c)]
        xt_rows = cancels(tables, xt['cancel'])
        at = next(n for n, c in enumerate(xt_rows) if c is exit_)
        for c in auto:
            pos = [n for n, c2 in enumerate(xt_rows) if c2 is not exit_ and is_automatic(c2) and
                   (c2['req'], c2['move_id']) == (c['req'], c['move_id'])]
            if pos:
                kept[row_kind(tables, c)] += 1
                before[row_kind(tables, c)] += pos[0] < at
        if auto:
            if all(any(c2 is not exit_ and c2['req'] == c['req'] and c2['move_id'] == c['move_id'] for c2 in xt_rows)
                   for c in auto):
                votes['layout']['separate'] += 1
            elif any(set(reqs(tables, c['req'])) <= set(reqs(tables, exit_['req'])) for c in auto):
                votes['layout']['merged'] += 1
            else:
                votes['layout']['alone'] += 1
        back = [c for c in cancels(tables, xda['cancel']) if c['direction'] != M.LIST_END]
        if not back:
            continue
        n = min(c['win_start'] for c in back)
        votes['leave'][n - la] += 1
        votes['back'][tuple((c['direction'], c['button'], c['req'], c['extradata'], c['option'], c['move_id'],
                             c['win_start'] - n, c['win_end'] - n, c['start_frame'] - n) for c in back)] += 1
        votes['no hit'][xda['hit_condition']] += 1
        own = [(p['frame'], p['id'], p['value']) for p in props(tables, x['extraprops'])]
        rows = [(p['frame'], p['id'], p['value']) for p in props(tables, xda['extraprops'])]
        votes['adds'][tuple((f - n if plain_frame(f) else f, pid, v) for f, pid, v in rows
                            if (f, pid, v) not in own)] += 1
        for pid in {pid for _, pid, _ in own}:
            seen[pid] += 1
            left[pid] += any((f, p, v) not in rows for f, p, v in own if p == pid)
        learn_fields(votes, tables, x, xt, 'XT')
        learn_fields(votes, tables, x, xda, 'XDA')
    tpl = {key: majority(votes[key], 'Tag Assault ' + key) for key in ('tag', 'assault', 'exit', 'leave', 'back',
                                                                       'no hit', 'adds')}
    layout = votes['layout']
    tpl['layout'] = max(layout, key=lambda form: (layout[form], form == 'separate')) if layout else 'separate'
    tpl['before'] = {kind for kind in kept if before[kind] * 2 > kept[kind]}
    # a property the donor's XDAs leave out as often as they keep it is left out: Jun has 0x80f6 only in its two item
    # moves, one XDA without it; TTT2's XDAs leave it out in 174 of 255 triples (108 of 123 outside the item moves),
    # Lars's in 2 of 3
    tpl['leaves out'] = {pid for pid in seen if left[pid] * 2 >= seen[pid]}
    tpl['fields'] = {(kind, f): field_rule(votes, kind, f) for kind in ('XT', 'XDA')
                     for f in VALUE_FIELDS + LIST_FIELDS}
    tpl['triples'] = len(found)
    return tpl


def twin(x, tpl, kind):
    """A copy of move row x with the fields the donor's twins of this kind take from X or not."""
    out = dict(x)
    for (k, f), rule in tpl['fields'].items():
        if k == kind and rule != SAME:
            out[f] = x[f] & HIGH_HALF if rule == HIGH else rule
    return out


def add_tag_assault(tables, tpl, first, name_of, string):
    """Every converted bound move X (move index >= first: a 467 hit condition, no 468 one, no tag cancel, active from
    frame 2 on) gets TTT2's Tag Assault after `tpl` (assault_template): its tag row (in a new list: X's rows, the tag
    row, X's end row) into XT, appended with XDA. name_of(i): move i's name; string(s): the name's string offset.
    -> [(X, XT, XDA)]"""
    moves = tables['moves']
    taken = {name_of(i) for i in range(len(moves))}

    def name(s):
        n, k = s, 1
        while n in taken:
            k += 1
            n = '%s%d' % (s, k)
        taken.add(n)
        return string(n)
    made = []
    for i in range(first, len(moves)):
        x = moves[i]
        fa, la = x['first_active'], x['last_active']
        hs = hits(tables, x['hit_condition'])
        rows = cancels(tables, x['cancel'])
        if x['hit_condition'] is None or not rows or fa < 2 or la < fa \
                or not any(has(tables, h['req'], BOUND) for h in hs) \
                or any(has(tables, h['req'], BOUND_TAG) for h in hs) \
                or any(c['direction'] == 0 and c['button'] == TAG_BUTTON for c in rows):
            continue
        body, end = rows[:-1], rows[-1]
        xt_i, xda_i = len(moves), len(moves) + 1
        req, extra, option, ws, we, sf = tpl['tag']
        tag = dict(direction=0, button=TAG_BUTTON, req=req, extradata=extra, win_start=ws, win_end=fa + we,
                   start_frame=fa + sf, move_id=xt_i, option=option)
        xt = twin(x, tpl, 'XT')
        xt['name'] = name(name_of(i) + 'T')
        req, reaction = tpl['assault']
        xt['hit_condition'] = write(tables, 'hit_conditions', [dict(req=req, damage=hs[0]['damage'],
                                                                     reactions=reaction)] + [dict(h) for h in hs])
        req, extra, option, ws, we, sf = tpl['exit']
        exit_ = dict(direction=0, button=0, req=req, extradata=extra, win_start=ws, win_end=we, start_frame=sf,
                     move_id=xda_i, option=option)
        # X's automatic rows: before the exit row the kinds the donor's XTs have there (Jun's and Lars's: the rows into
        # the stance, extradata 0x2800), the others after it, as TTT2's own XTs have them: of X's 58 automatic rows
        # into other moves in its 616 triples, 27 come after the exit row (on hit into X's follow-up: He_kakatAT,
        # Fu_ryuho00T; the split at the last active frame: Fu_R_rp01T), 24 are left out and the 7 before it are on
        # block, outside the active frames or under further conditions. Before it, a row taken on the hit (or on its
        # frame) would leave XT with the opponent bounced for the partner and the partner never coming in
        keep = [dict(c) for c in body if is_automatic(c)] if tpl['layout'] != 'alone' else []
        first_ = [c for c in keep if row_kind(tables, c) in tpl['before']]
        then = [c for c in keep if row_kind(tables, c) not in tpl['before']]
        xt['cancel'] = write(tables, 'cancels', first_ + [exit_] + then + [dict(end)])
        xda = twin(x, tpl, 'XDA')
        xda['name'] = name(name_of(i) + 'DA')
        xda['hit_condition'] = tpl['no hit']
        n = la + tpl['leave']
        back = [dict(direction=dr, button=bt, req=rq, extradata=ex, option=op, move_id=mid, win_start=n + a,
                     win_end=n + b, start_frame=n + s) for dr, bt, rq, ex, op, mid, a, b, s in tpl['back']]
        xda['cancel'] = write(tables, 'cancels', back + [dict(end)])
        # X's property rows themselves (not copies: the converter renumbers effects in them after run()), less the
        # ones the donor's XDAs leave out, then the ones they add
        own = [p for p in props(tables, x['extraprops']) if p['id'] not in tpl['leaves out']]
        adds = [dict(frame=n + f if plain_frame(f) else f, id=pid, value=v) for f, pid, v in tpl['adds']]
        adds = [p for p in adds if (p['frame'], p['id'], p['value']) not in
                {(q['frame'], q['id'], q['value']) for q in own}]
        xda['extraprops'] = write(tables, 'extra_move_properties', own + adds + [dict(frame=0, id=0, value=0)])
        x['cancel'] = write(tables, 'cancels', [dict(c) for c in body] + [tag, dict(end)])
        moves += [xt, xda]
        made.append((i, xt_i, xda_i))
    return made


# ------------------------------------------------------------------------------------------------ Tag Crash
def landing_template(tables, aliases):
    """The donor's Tag Crash rows, learnt from its reactions: the Tag Crash row (HELP_BUTTON: requirements,
    extradata, option, target) right before a run of tech rows (buttons and targets learnt too), the plain tag row
    right after them (TAG_BUTTON, with the tech rows' window), which target goes with which (face up / face down) and
    which one each landing (the move's transition) leads to; the Tag Crash row has the tech rows' window, or its last
    `last` frames when the window is longer and ends at its start frame (TTT2's wall reactions). Raises ValueError if
    the donor has none."""
    v = collections.defaultdict(collections.Counter)
    side = collections.defaultdict(collections.Counter)             # landing (transition) -> Tag Crash target
    pair = collections.defaultdict(collections.Counter)             # Tag Crash target -> plain tag row's
    for x in tables['moves']:
        rows = cancels(tables, x['cancel'])
        for k, c in enumerate(rows):
            if c['direction'] >= M.LIST_END or c['button'] != HELP_BUTTON:
                continue
            v['help'][(c['req'], c['extradata'], c['option'])] += 1
            side[resolve(aliases, x['transition'])][c['move_id']] += 1
            v['help target'][c['move_id']] += 1
            tech = []
            for c2 in rows[k + 1:]:
                if c2['direction'] >= M.LIST_END or c2['button'] in (0, TAG_BUTTON):
                    break
                tech.append(c2)
            if not tech:
                continue                                    # the lying states: no tech rows, the whole time
            for t in tech:
                v['tech'][t['button']] += 1
                v['tech target'][resolve(aliases, t['move_id'])] += 1
            f = tech[0]
            if (c['win_end'], c['start_frame']) == (f['win_end'], f['start_frame']) and \
                    c['win_start'] > f['win_start'] and f['win_end'] == f['start_frame']:
                v['last'][c['win_end'] - c['win_start'] + 1] += 1
            nxt = rows[k + 1 + len(tech)]
            if nxt['button'] == TAG_BUTTON and nxt['direction'] < M.LIST_END:
                v['tag'][(nxt['req'], nxt['extradata'], nxt['option'])] += 1
                pair[c['move_id']][nxt['move_id']] += 1
    help_ = majority(v['help'], 'Tag Crash rows (2 + tag)')
    return dict(help=help_, tag=majority(v['tag'], 'tag rows after the tech rows'),
                tech=set(v['tech']), tech_targets=set(v['tech target']),
                last=majority(v['last'], 'shortened Tag Crash windows') if v['last'] else None,
                side={t: c.most_common(1)[0][0] for t, c in side.items()},
                any_side=majority(v['help target'], 'Tag Crash targets'),
                pair={h: c.most_common(1)[0][0] for h, c in pair.items()})


def add_landing_rows(tables, tpl, first, aliases):
    """Every converted move (index >= first) with tech rows (a tpl['tech'] button into a tech move) and no Tag Crash
    row gets the two rows of `tpl` (landing_template): the Tag Crash row before its first tech row, the plain tag row
    after the tech rows of that window; a list several moves share is written anew once per landing. -> how many
    moves changed."""
    moves = tables['moves']
    made, changed = {}, 0
    for i in range(first, len(moves)):
        x = moves[i]
        rows = cancels(tables, x['cancel'])
        if any(c['button'] == HELP_BUTTON and c['direction'] < M.LIST_END for c in rows):
            continue
        tech = [k for k, c in enumerate(rows) if c['direction'] < M.LIST_END and c['button'] in tpl['tech']
                and resolve(aliases, c['move_id']) in tpl['tech_targets']]
        if not tech:
            continue
        help_target = tpl['side'].get(resolve(aliases, x['transition']), tpl['any_side'])
        key = (x['cancel'], help_target)
        if key not in made:
            f = rows[tech[0]]
            ws, we, sf = f['win_start'], f['win_end'], f['start_frame']
            last = tpl['last']
            if last is not None and we - ws + 1 > last and we == sf:
                ws = we - last + 1
            end = tech[0]
            while end + 1 < len(rows) and end + 1 in tech and \
                    (rows[end + 1]['win_start'], rows[end + 1]['win_end']) == (f['win_start'], f['win_end']):
                end += 1
            req, extra, option = tpl['help']
            help_ = dict(direction=0, button=HELP_BUTTON, req=req, extradata=extra, win_start=ws, win_end=we,
                         start_frame=sf, move_id=help_target, option=option)
            req, extra, option = tpl['tag']
            tag = dict(direction=0, button=TAG_BUTTON, req=req, extradata=extra, win_start=f['win_start'],
                       win_end=f['win_end'], start_frame=f['start_frame'],
                       move_id=tpl['pair'].get(help_target, help_target), option=option)
            out = [dict(c) for c in rows]
            made[key] = write(tables, 'cancels', out[:tech[0]] + [help_] + out[tech[0]:end + 1] + [tag]
                              + out[end + 1:])
        x['cancel'] = made[key]
        changed += 1
    return changed


# ------------------------------------------------------------------------------------------------ tag hubs
def kept_hub(rows, ttt2_only, donor_specific):
    """A generic move whose Tekken 7 list leads to none of the fighter's own moves: when most of the donor's rows
    (`rows`, its cancel list) lead into TTT2-only moves (ttt2_only(move id): the partner's tag-in dispatcher), the
    donor's list without its rows into donor-specific moves (donor_specific(move id): its item moves) -> the new
    list's rows; else None (Tekken 7's list, as other hubs)."""
    plain = [c for c in rows if c['direction'] < M.LIST_END]
    if not plain or sum(1 for c in plain if ttt2_only(c['move_id'])) * 2 <= len(plain):
        return None
    return [dict(c) for c in rows if not (c['direction'] < M.LIST_END and donor_specific(c['move_id']))]


# ------------------------------------------------------------------------------------------------ tag out
STATE_SHARE = 3                      # a generic movement state: tag-out in at least 1 / 3 of TTT2's movesets


def state_name(name):
    """A move name without the fighter's prefix, '_' and '00': the same movement state in every moveset (La_sWALKB,
    sWALK_00B and Ka_sWALKB are sWALKB; Kz_st2cr_Rv and st2cr_00_Rv are st2crRv)."""
    return re.sub(r'^[A-Z][a-z0-9]_', '', name).replace('_', '').replace('00', '')


def tag_out_moves(m):
    """The moves of the TTT2 moveset m (a Moveset) with a tag-out row (TAG pressed -> Co_StChgOut*) in their cancel
    list or its groups."""
    nm = len(m.tables['moves'])

    def tags(c):
        t = m.resolve_alias(c['move_id'])
        return bool(c['button'] & TAG_PRESSED) and t is not None and t < nm and m.move_name(t).startswith(TAG_OUT)
    groups, out = {}, set()
    for i, mv in enumerate(m.tables['moves']):
        for c in M.cancels(m, mv['cancel']):
            if c['direction'] == M.GROUP:
                g = c['move_id']
                if g not in groups:
                    groups[g] = any(tags(x) for x in M.cancels(m, g, 'group_cancels', M.GROUP_END)
                                    if x['direction'] != M.GROUP_END)
                hit = groups[g]
            else:
                hit = c['direction'] < M.LIST_END and tags(c)
            if hit:
                out.add(i)
                break
    return out


class TagStates:
    """TTT2's own answer to "may a fighter tag out from the move named X?", from all of its fighters' movesets: a move
    name TTT2 has is a tag-out state where most of the movesets with it let it tag out (Raven's Nj_step_00F, Bryan's
    Ba_hadoh00: no); another name is one when its state_name is a generic tag-out state (one in at least a third of
    the movesets: walks, dashes, backdash, crouch, crouch walks, crouch starts, rising, side steps)."""

    def __init__(self, movesets):
        self.exact = collections.defaultdict(collections.Counter)      # name -> {True: movesets, False: movesets}
        common = collections.Counter()
        for m in movesets:
            yes, seen = tag_out_moves(m), set()
            for i in range(len(m.tables['moves'])):
                n = m.move_name(i)
                if n not in seen:
                    seen.add(n)
                    self.exact[n][i in yes] += 1
            common.update({state_name(m.move_name(i)) for i in yes})
        self.common = {s for s, n in common.items() if n * STATE_SHARE >= len(movesets)}

    def __call__(self, name):
        e = self.exact.get(name)
        if e:
            return e[True] > 0 and e[True] >= e[False]
        return state_name(name) in self.common
