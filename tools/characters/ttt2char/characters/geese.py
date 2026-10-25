"""Geese Howard (Fatal Fury) from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. Tekken 7 keeps his costume encrypted: the
importer reads it as the running game reads it (dump_tekken7.py saves it). His materials are classified by the
importer (model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Geese Howard'
SHORT_NAME = 'GEESE'
TEKKEN7_ID = 43
TEKKEN7_CODE = 'mry'
CODE = 'mry'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/MRY/CS_MRY_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/MRY/CS_MRY_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/mry/Face/mry_fac/Meshes/SK_CH_mry_fac'),
        ('bdl', _ITEM + '/Meshes/mry/Lower/mry_bdl_1p/Meshes/SK_CH_mry_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/mry/Upper/mry_bdu_1p/Meshes/SK_CH_mry_bdu_1p'),
        ('har', _ITEM + '/Meshes/mry/Hair/mry_har_1p/Meshes/SK_CH_mry_har_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har'],
}

MESHES = {
    'fac': ('mry_fac', 'ATAMA_Joint', 0),
    'bdl': ('mry_bdl', 'KOSHI_jnt', 1),
    'bdu': ('mry_bdu', 'MUNE2_jnt', 2),
    'har': ('mry_har', 'ATAMA_jnt_assist', 3),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_MRY_7FR_DEF', (342, 225, 155)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_MRY', (582, 276, 125)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_MRY',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_MRY_01',
}

VOICE_PREFIX = 'my'

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[GEESE]',
    'drop_moves': r'^My_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['my_s%02d' % k for k in range(7)]),     # the donor's intros -> his
               (r'^la_w', 'anim', ['my_w%02d' % k for k in range(3)])],         # win poses
}
