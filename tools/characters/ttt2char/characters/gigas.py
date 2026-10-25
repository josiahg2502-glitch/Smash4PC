"""Gigas from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. His materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Gigas'
SHORT_NAME = 'GIGAS'
TEKKEN7_ID = 25
TEKKEN7_CODE = 'crz'
CODE = 'crz'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/CRZ/CS_CRZ_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/CRZ/CS_CRZ_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/crz/Face/crz_fac/Meshes/SK_CH_crz_fac'),
        ('bdl', _ITEM + '/Meshes/crz/Lower/crz_bdl_1p/Meshes/SK_CH_crz_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/crz/Upper/crz_bdu_1p/Meshes/SK_CH_crz_bdu_1p'),
        ('hef', _ITEM + '/Meshes/crz/Full_Head/crz_hef_1p/Meshes/SK_CH_crz_hef_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'hef'],
}

MESHES = {
    'fac': ('crz_fac', 'ATAMA_Joint', 0),
    'bdl': ('crz_bdl', 'KOSHI_jnt', 1),
    'bdu': ('crz_bdu', 'MUNE2_jnt', 2),
    'hef': ('crz_hef', 'ATAMA_jnt_assist', 3),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_CRZ_TK7_DEF', (373, 287, 113)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_CRZ', (514, 300, 105)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_CRZ',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_CRZ_01',
}

VOICE_PREFIX = 'cz'

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[GIGAS]',
    'drop_moves': r'^Cz_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['cz_s%02d' % k for k in range(3)]),     # the donor's intros -> his
               (r'^la_w', 'anim', ['cz_w%02d' % k for k in range(3)])],         # win poses
}

# Tekken 7 simulates the five tubes on his back as chains its mesh models 2.6 m straight out behind him (its physics
# arcs them over his shoulders): left out (the triangles skinned to them, convert/model.py HIDE).
SKIN_HUE = 'own'              # his red skin keeps its hue (skin.py)
HIDE = [('bdu', 'HLP_L_CollarBone__offset_OPT001'), ('bdu', 'HLP_R_CollarBone__offset_OPT001'),
        ('bdu', 'Spine2__offset_OPT001'), ('bdu', 'Spine2__offset_OPT009'), ('bdu', 'Spine2__offset_OPT017')]
