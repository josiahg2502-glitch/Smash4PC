"""Katarina Alves from Tekken 7 (PC), in her default costume.

Only names, paths and rules live here; every byte of her model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. Her materials are classified by the importer
(model.classify); MATERIALS lists only exceptions. Her sunglasses (a static mesh on a socket) are not converted.
"""
NAME = 'Katarina Alves'
SHORT_NAME = 'KATARINA'
TEKKEN7_ID = 21
TEKKEN7_CODE = 'ltn'
CODE = 'ltn'
SEX = 'female'

DONOR = 'jun'                 # Jun Kazama: the 139-bone female skeleton
DONOR_ROW = 42

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/LTN/CS_LTN_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/LTN/CS_LTN_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/ltn/Face/ltn_fac/Meshes/SK_CH_ltn_face'),
        ('bdl', _ITEM + '/Meshes/ltn/Lower/ltn_bdl_1P/Meshes/SK_CH_ltn_bdl_1P'),
        ('bdu', _ITEM + '/Meshes/ltn/Upper/ltn_bdu_1P/Meshes/SK_CH_ltn_bdu_1P'),
        ('har', _ITEM + '/Meshes/ltn/Hair/ltn_har_1p/Meshes/SK_CH_ltn_har_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har'],
}

MESHES = {
    'fac': ('ltn_fac', 'ATAMA_Joint', 0),
    'bdl': ('ltn_bdl', 'KOSHI_jnt', 1),
    'bdu': ('ltn_bdu', 'MUNE2_jnt', 2),
    'har': ('ltn_har', 'ATAMA_jnt_assist', 3),
}

MATERIALS = {}

# the material templates: Jun's (they are only shaders; the textures are hers)
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_LTN_TK7_DEF', (333, 253, 127)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_LTN', (574, 290, 100)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_LTN',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_LTN_01',
}

VOICE_PREFIX = 'ln'

# the id maps: Heihachi in both games, and her generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[KATARINA]',
    'drop_moves': r'^Ln_Direct$',
    'direct': [(r'^(ju|mk)_s', 'anim', ['ln_s%02d' % k for k in range(5)]),     # the donor's intros -> her
               (r'^ju_w', 'anim', ['ln_w%02d' % k for k in range(3)])],         # win poses
}
