"""The added fighter's item database (`<code>.xml`, BXML), made from the donor's.

A fighter's database has two top-level elements: InstanceInfo (one Instance per costume item: its Data, the Model
sections A/B naming part models in the fighter's own packs, effects) and OffsetInfo. The added fighter has no parts
of its own (everything is in its body model), so it keeps only the donor's default-costume Instances (same labels
and indices: upper and lower body, head, the default effects and the rage aura) with their part lists emptied. A
database naming parts the fighter's packs do not have makes the loading screen hang.

The fighter's effects: its moves spawn effects by id (move property 0x801f = body part << 8 | id), and the ids mean
the effect files that the worn items' EffectParameters name; the default set's character effect Instance
(`cmn_eff_default`) gets the fighter's ids, naming its effect files by bare name (the title loads
`<code>/alo/<name>.alo` with the fighter's code: copies of the game's own, effects.py).
"""
import copy

from ..ttt2 import bxml

DEFAULT_COSTUME = 'デフォルトコスチューム'       # the EquipType of default-costume items ("default costume")
EFFECT_INSTANCE = 'cmn_eff_default'     # the default set's character effects
ANY_BONE = '指定無し'                  # an effect parameter's bone: "unspecified" (the move's body part)


def body_only(donor_db, effects=None):
    """effects: {effect id: [(effect name, bone or None)]} added to the character effect Instance."""
    info, offsets = bxml.parse(donor_db)[:2]
    instances = []
    for inst in info['children']:
        data = [c for c in inst['children'] if c['tag'] == 'Data']
        if not data or dict(data[0]['attrs']).get('EquipType') != DEFAULT_COSTUME:
            continue
        inst = copy.deepcopy(inst)
        kids = []
        for c in inst['children']:
            if c['tag'] == 'Model' and not any(g['tag'] == 'EffectGroup' for g in c['children']):
                c['children'] = []               # no part model; effect groups stay
            if c['tag'] != 'ModelBConditions':
                kids.append(c)
        inst['children'] = kids
        if effects and dict(inst['attrs']).get('label') == EFFECT_INSTANCE:
            add_effects(inst, effects)
        instances.append(inst)
    return bxml.write([dict(tag=info['tag'], attrs=info['attrs'], children=instances),
                       dict(tag=offsets['tag'], attrs=offsets['attrs'], children=[])])


def effect_names(donor_db):
    """The bare effect names of the donor's character effect Instance."""
    info = bxml.parse(donor_db)[0]
    for inst in info['children']:
        if dict(inst['attrs']).get('label') == EFFECT_INSTANCE:
            return [dict(g['attrs']).get('Filename') for m in inst['children'] if m['tag'] == 'Model'
                    for g in m['children'] if g['tag'] == 'EffectGroup' and '/' not in dict(g['attrs']).get('Filename', '/')]
    return []


def add_effects(inst, effects):
    """Names the effect files in the Instance's model (section A) and maps the ids to them (in place of the
    donor's effects of the same ids)."""
    model = next(c for c in inst['children'] if c['tag'] == 'Model' and dict(c['attrs']).get('section') == 'A')
    params = next((c for c in inst['children'] if c['tag'] == 'EffectParameters'), None)
    if params is None:
        params = dict(tag='EffectParameters', attrs=[], children=[])
        inst['children'].append(params)
    params['children'] = [c for c in params['children'] if int(dict(c['attrs']).get('id', -1)) not in effects]
    used = {dict(c['attrs']).get('effect') for c in params['children']} | {p for fs in effects.values() for p, _ in fs}
    model['children'] = [c for c in model['children'] if c['tag'] != 'EffectGroup' or
                         dict(c['attrs']).get('Filename') in used]       # effect files no id names any more
    named = {dict(c['attrs']).get('Filename') for c in model['children'] if c['tag'] == 'EffectGroup'}
    for effect_id, files in sorted(effects.items()):
        for path, bone in files:
            if path not in named:
                model['children'].append(dict(tag='EffectGroup', attrs=[('Filename', path)], children=[]))
                named.add(path)
            params['children'].append(dict(tag='Parameter', attrs=[('id', str(effect_id)), ('bone', bone or ANY_BONE),
                                                                   ('effect', path)], children=[]))


def effect_files_named(db):
    """The bare effect names a database's character effect Instance maps ids to (its effect files still in use)."""
    info = bxml.parse(db)[0]
    for inst in info['children']:
        if dict(inst['attrs']).get('label') == EFFECT_INSTANCE:
            return {dict(p['attrs']).get('effect') for c in inst['children'] if c['tag'] == 'EffectParameters'
                    for p in c['children']}
    return set()


def effect_ids(db):
    """The effect ids a database's character effect Instance maps."""
    info = bxml.parse(db)[0]
    for inst in info['children']:
        if dict(inst['attrs']).get('label') == EFFECT_INSTANCE:
            return {int(dict(p['attrs']).get('id')) for c in inst['children'] if c['tag'] == 'EffectParameters'
                    for p in c['children']}
    return set()
