"""Akuma (Street Fighter) from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. His materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Akuma'
SHORT_NAME = 'AKUMA'
TEKKEN7_ID = 32
TEKKEN7_CODE = 'mrx'
CODE = 'mrx'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/MRX/CS_MRX_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/MRX/CS_MRX_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/mrx/Face/mrx_fac/Meshes/SK_CH_mrx_fac'),
        ('bdl', _ITEM + '/Meshes/mrx/Lower/mrx_bdl_1p/Meshes/SK_CH_mrx_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/mrx/Upper/mrx_bdu_1p/Meshes/SK_CH_mrx_bdu_1p'),
        ('har', _ITEM + '/Meshes/mrx/Hair/mrx_har_1p/Meshes/SK_CH_mrx_har_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har'],
}

MESHES = {
    'fac': ('mrx_fac', 'ATAMA_Joint', 0),
    'bdl': ('mrx_bdl', 'KOSHI_jnt', 1),
    'bdu': ('mrx_bdu', 'MUNE2_jnt', 2),
    'har': ('mrx_har', 'ATAMA_jnt_assist', 3),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_MRX_7FR_DEF', (290, 233, 75)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_MRX', (570, 274, 60)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_MRX',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_MRX_01',
}

VOICE_PREFIX = 'mx'

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[AKUMA]',
    'drop_moves': r'^Mx_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['mx_s%02d' % k for k in range(6)]),     # the donor's intros -> his
               (r'^la_w', 'anim', ['mx_w%02d' % k for k in range(2)])],         # win poses
}
