"""Josie Rizal from Tekken 7 (PC), in her default costume.

Only names, paths and rules live here; every byte of her model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. Her materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Josie Rizal'
SHORT_NAME = 'JOSIE'
TEKKEN7_ID = 24
TEKKEN7_CODE = 'mut'
CODE = 'mut'
SEX = 'female'

DONOR = 'jun'                 # Jun Kazama: the 139-bone female skeleton
DONOR_ROW = 42

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/MUT/CS_MUT_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/MUT/CS_MUT_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/mut/Face/mut_fac/Meshes/SK_CH_mut_fac'),
        ('bdl', _ITEM + '/Meshes/mut/Lower/mut_bdl_1p/Meshes/SK_CH_mut_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/mut/Upper/mut_bdu_1p/Meshes/SK_CH_mut_bdu_1p'),
        ('hed', _ITEM + '/Meshes/mut/Head/mut_hed_1p/Meshes/SK_CH_mut_hed_1p'),
        ('har', _ITEM + '/Meshes/mut/Hair/mut_har_1p/Meshes/SK_CH_mut_har_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'hed', 'har'],
}

MESHES = {
    'fac': ('mut_fac', 'ATAMA_Joint', 0),
    'bdl': ('mut_bdl', 'KOSHI_jnt', 1),
    'bdu': ('mut_bdu', 'MUNE2_jnt', 2),
    'hed': ('mut_hed', 'ATAMA_jnt_assist', 3),
    'har': ('mut_har', 'ATAMA_jnt_assist', 4),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_MUT_TK7_DEF', (391, 255, 118)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_MUT', (656, 314, 96)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_MUT',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_MUT_01',
}

VOICE_PREFIX = 'mu'

# the id maps: Heihachi in both games, and her generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[JOSIE]',
    'drop_moves': r'^Ba_Direct$',
    'direct': [(r'^(ju|mk)_s', 'anim', ['mu_s%02d' % k for k in range(4)]),     # the donor's intros -> her
               (r'^ju_w', 'anim', ['mu_w%02d' % k for k in range(3)])],         # win poses
}
