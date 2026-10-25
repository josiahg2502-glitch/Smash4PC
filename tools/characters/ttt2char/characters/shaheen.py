"""Shaheen from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. His materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Shaheen'
SHORT_NAME = 'SHAHEEN'
TEKKEN7_ID = 23
TEKKEN7_CODE = 'arb'
CODE = 'arb'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/ARB/CS_ARB_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/ARB/CS_ARB_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/arb/Face/arb_fac/Meshes/SK_CH_arb_fac'),
        ('bdl', _ITEM + '/Meshes/arb/Lower/arb_bdl_1p/Meshes/SK_CH_arb_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/arb/Upper/arb_bdu_1p/Meshes/SK_CH_arb_bdu_1p'),
        ('hed', _ITEM + '/Meshes/arb/Head/arb_hed_1p/Meshes/SK_CH_arb_hed_1p'),
        ('fah', _ITEM + '/Meshes/arb/FACE_HAIR/arb_fah_1p/Meshes/SK_CH_arb_fah_1p'),
        ('har', _ITEM + '/Meshes/arb/Hair/arb_har_1p/Meshes/SK_CH_arb_har_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'hed', 'fah', 'har'],
}

MESHES = {
    'fac': ('arb_fac', 'ATAMA_Joint', 0),
    'bdl': ('arb_bdl', 'KOSHI_jnt', 1),
    'bdu': ('arb_bdu', 'MUNE2_jnt', 2),
    'hed': ('arb_hed', 'ATAMA_jnt_assist', 3),
    'fah': ('arb_fah', 'ATAMA_jnt_assist', 4),
    'har': ('arb_har', 'ATAMA_jnt_assist', 5),
}

MATERIALS = {}

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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_ARB_TK7_DEF', (370, 252, 135)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_ARB', (656, 320, 95)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_ARB',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_ARB_01',
}

VOICE_PREFIX = 'ab'

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[SHAHEEN]',
    'drop_moves': r'^Ab_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['ab_s%02d' % k for k in range(3)]),     # the donor's intros -> his
               (r'^la_w', 'anim', ['ab_w%02d' % k for k in range(3)])],         # win poses
}
