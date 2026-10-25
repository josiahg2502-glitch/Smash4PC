"""Claudio Serafino from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. His materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Claudio Serafino'
SHORT_NAME = 'CLAUDIO'
TEKKEN7_ID = 20
TEKKEN7_CODE = 'ita'
CODE = 'ita'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
_MESHES = _ITEM + '/Meshes/ita'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/ITA/CS_ITA_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/ITA/CS_ITA_DefaultCos2P',
    'parts': [
        ('fac', _MESHES + '/Face/ita_fac/Meshes/SK_CH_ita_face'),
        ('har', _MESHES + '/Hair/ita_har_1P/Meshes/SK_CH_ita_hair'),
        ('bdu', _MESHES + '/Upper/ita_bdu_1P/Meshes/SK_CH_ita_bdu_1P'),
        ('bdl', _MESHES + '/Lower/ita_bdl_1P/Meshes/SK_CH_ita_bdl_1P'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har'],
}

MESHES = {
    'bdl': ('ita_bdl_1p', 'KOSHI_jnt', 0),
    'bdu': ('ita_bdu_1p', 'MUNE2_jnt', 1),
    'fac': ('ita_fac', 'ATAMA_Joint', 2),
    'har': ('ita_har_1p', 'ATAMA_jnt_assist', 3),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_ITA_TK7_DEF', (412, 232, 130)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_ITA', (651, 286, 95)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_ITA',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_ITA_01',
}

VOICE_PREFIX = 'it'

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[CLAUDIO]',
    'drop_moves': r'^It_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['it_s%02d' % k for k in range(4)]),     # Lars's intros -> his
               (r'^la_w', 'anim', ['it_w%02d' % k for k in range(3)])],         # win poses
}
