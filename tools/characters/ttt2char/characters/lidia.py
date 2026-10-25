"""Lidia Sobieska from Tekken 7 (PC), in her default costume.

Only names, paths and rules live here; every byte of her model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. Her materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Lidia Sobieska'
SHORT_NAME = 'LIDIA'
TEKKEN7_ID = 56
TEKKEN7_CODE = 'nsd'
CODE = 'nsd'
SEX = 'female'

DONOR = 'jun'                 # Jun Kazama: the 139-bone female skeleton
DONOR_ROW = 42

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/NSD/CS_NSD_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/NSD/CS_NSD_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/nsd/Face/nsd_fac/Meshes/SK_CH_nsd_fac'),
        ('bdl', _ITEM + '/Meshes/nsd/Lower/nsd_bdl_1p/Meshes/SK_CH_nsd_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/nsd/Upper/nsd_bdu_1p/Meshes/SK_CH_nsd_bdu_1p'),
        ('har', _ITEM + '/Meshes/nsd/Hair/nsd_har_1p/Meshes/SK_CH_nsd_har_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har'],
}

MESHES = {
    'fac': ('nsd_fac', 'ATAMA_Joint', 0),
    'bdl': ('nsd_bdl', 'KOSHI_jnt', 1),
    'bdu': ('nsd_bdu', 'MUNE2_jnt', 2),
    'har': ('nsd_har', 'ATAMA_jnt_assist', 3),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_NSD_7FR_DEF', (388, 233, 120)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_NSD', (612, 280, 95)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_NSD',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_NSD_01',
}

VOICE_PREFIX = 'nd'

# the id maps: Heihachi in both games, and her generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[LIDIA]',
    'drop_moves': r'^Nd_Direct$',
    'direct': [(r'^(ju|mk)_s', 'anim', ['nd_s%02d' % k for k in range(3)]),     # the donor's intros -> her
               (r'^ju_w', 'anim', ['nd_w%02d' % k for k in range(3)])],         # win poses
}
