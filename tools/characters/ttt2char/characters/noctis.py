"""Noctis Lucis Caelum (Final Fantasy XV) from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. Tekken 7 keeps his costume and voice encrypted: the
importer reads them as the running game reads them (dump_tekken7.py saves them). His materials are classified by the
importer (model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Noctis'
SHORT_NAME = 'NOCTIS'
TEKKEN7_ID = 44
TEKKEN7_CODE = 'mrz'
CODE = 'mrz'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/MRZ/CS_MRZ_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/MRZ/CS_MRZ_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/mrz/Face/mrz_fac/Meshes/SK_CH_mrz_fac'),
        ('bdf', _ITEM + '/Meshes/mrz/Full_Body/mrz_bdf_1p/Meshes/SK_CH_mrz_bdf_1p'),
        ('har', _ITEM + '/Meshes/mrz/Hair/mrz_har_1p/Meshes/SK_CH_mrz_har_1p'),
    ],
    'merge_order': ['fac', 'bdf', 'har'],
}

# his trousers and boots: an item his jacket depends on (CCI DependencyItems), worn with it
DEPENDENCY_ITEMS = True

MESHES = {
    'fac': ('mrz_fac', 'ATAMA_Joint', 0),
    'bdf': ('mrz_bdf', 'KOSHI_jnt', 1),
    'har': ('mrz_har', 'ATAMA_jnt_assist', 2),
}

MATERIALS = {}
# his outfit and hair are nearly black (their colours 0.02..0.06 of the textures'), which Tekken 7 lifts with its rim
# lights and TTT2 draws black (model.py): brighter, to look as in Tekken 7 next to his skin (measured on a Tekken 7
# screenshot: jacket 0.42, shirt 0.23, trousers 0.59, boots and gloves 0.40, hair 0.25 at the roots, 0.52 at the tips
# of his arm's brightness); his outfit's colours are blue-grey whatever the textures' mauve (GREY); his weapons (cloth
# materials too) keep their colours
GAIN = {'MI_CH_mrz_bdf_1p_cloth': 4.0, 'MI_CH_mrz_bdf_1p_bootsgloves': 1.6, 'hair': 4.0}
GREY = ['MI_CH_mrz_bdf_1p_cloth', 'MI_CH_mrz_bdf_1p_bootsgloves']

# the material templates: Jun's (they are only shaders; the textures are his)
TEMPLATES = {
    'skin': ('jun.nud', 'ude_A_O_OBJ', 0),
    'mouth': ('jun.nud', 'cha:kao_O_OBJ', 0),
    'eye': ('jun.nud', 'cha:kao_O_OBJ', 2),
    'overlay': ('jun.nud', 'cha:kao_O_OBJ', 7),
    'cloth': (0xD4F28D09, 'jun_upb_1p_b', 0),
    'hair': (0x5F412CA4, 'jun_hed_1p', 1),
    'colour': (0xD4F28D09, 'jun_upb_1p_b', 2),
    'colour_alpha': (0xD4F28D09, 'jun_upb_1p_b', 1),
}
TEMPLATE_TEXTURES = ['jun.nut', 0x24F8AE59, 0x277C7A37, 0xAD9B60F0, 0x900FD8C4]

_UI = '/Game/UI_Common/Texture2D/ReplaceImages'
ART = {
    'bust': (_UI + '/VS_CH_L/VS_CH_L_MRZ_7FR_DEF', (338, 231, 115)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_MRZ', (576, 298, 92)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_MRZ',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_MRZ_01',
}

VOICE_PREFIX = 'mz'

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[NOCTIS]',
    'drop_moves': r'^Mz_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['mz_s%02d' % k for k in range(5)]),     # the donor's intros -> his
               (r'^la_w', 'anim', ['mz_w%02d' % k for k in range(6)])],         # win poses
}

# His weapons, summoned for the moves that use them (Tekken 7's 0x82a7 n .. 0x82a8 n; convert/weapons.py): one mesh
# holds them all, a copy for each hand, on ATH_* bones his animations pose with their extra pairs (the pairs of one
# hand carry the same pose: the hand's grip). Summons: 0/1 the great sword (left / right), 2/3 the sword (his usual
# one: right), 4/5 the dagger, 6 the shield on his left forearm, 7 the spear (right); 'part' places a summon's
# particles (TTT2 body parts: 9 the right hand, 13 the left, 12 the left forearm). A move hits with a weapon's tip
# (Tekken 7's hitbox (tip, 0, tip, hand), and (tip, guard) pairs): a hand's weapons share the points' slots (the
# summoned one's counts; the first listed is the default), right tip 0, right guard 1, left tip 2.
_WEAPONS_MESH = _ITEM + '/Meshes/mrz/Extra/mrz_exp_1p/Meshes/SK_CH_mrz_exp_1p'
WEAPONS = [
    {'mesh': _WEAPONS_MESH, 'bone': 'ATH_R_weapon002', 'parent': 'TE_R_jnt', 'pair': 4, 'summon': 3, 'part': 9,
     'points': {0x63: ('ATH_R_weapon002_top', 0), 0x66: ('ATH_R_weapon002_hilp', 1)}},
    {'mesh': _WEAPONS_MESH, 'bone': 'ATH_L_weapon002', 'parent': 'TE_L_jnt', 'pair': 1, 'summon': 2, 'part': 13,
     'points': {0x5f: ('ATH_L_weapon002_top', 2)}},
    {'mesh': _WEAPONS_MESH, 'bone': 'ATH_R_weapon001', 'parent': 'TE_R_jnt', 'pair': 3, 'summon': 1, 'part': 9,
     'points': {0x62: ('ATH_R_weapon001_top', 0), 0x68: ('ATH_R_weapon001_hilp', 1)}},
    {'mesh': _WEAPONS_MESH, 'bone': 'ATH_L_weapon001', 'parent': 'TE_L_jnt', 'pair': 0, 'summon': 0, 'part': 13,
     'points': {0x5e: ('ATH_L_weapon001_top', 2)}},
    {'mesh': _WEAPONS_MESH, 'bone': 'ATH_R_weapon003', 'parent': 'TE_R_jnt', 'pair': 5, 'summon': 5, 'part': 9,
     'points': {0x64: ('ATH_R_weapon003_top', 0)}},
    {'mesh': _WEAPONS_MESH, 'bone': 'ATH_L_weapon003', 'parent': 'TE_L_jnt', 'pair': 2, 'summon': 4, 'part': 13,
     'points': {0x60: ('ATH_L_weapon003_top', 2)}},
    {'mesh': _WEAPONS_MESH, 'bone': 'ATH_R_weapon_spear', 'parent': 'TE_R_jnt', 'pair': 6, 'summon': 7, 'part': 9,
     'points': {0x65: ('ATH_R_weapon_spear_top', 0)}},
    {'mesh': _WEAPONS_MESH, 'bone': 'ATH_L_weapon_shield', 'parent': 'UDE_L_jnt', 'pair': None, 'summon': 6,
     'part': 12},
]
# his weapons take about 37,000 vertices (all eight are in the model; one or two show at a time), his trousers and
# boots 17,000 more (89,000 in all): the port's skinning buffers have room for two of him and two others in a tag
# match (roster.cpp, skinning room)
VERTEX_BUDGET = 90000
# Warp-Strike and its kin throw the sword (its animation flies it up to 4.8 m) and warp to it when it hits: the move
# a hit leads to holds it where it hit until the warp takes it back (moveset.WEAPON_HOLD_PROP; Tekken 7 dismisses it
# there with particles of its mesh, which are left out); on guard it is gone at once, as in Tekken 7
WEAPON_HOLD = r'^Mz_(rp01F|rush03C|t_slpF_RP|s_tlk01|WPF2_EX)_HIT$'
# the Rage Art's and the Sword of the Father's weapons (not drawn) hit with the right hand's weapon's tip
HIT_LOCATIONS = {0x67: 0x46, 0x69: 0x46}
