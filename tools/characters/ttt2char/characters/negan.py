"""Negan (The Walking Dead) from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. His materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Negan'
SHORT_NAME = 'NEGAN'
TEKKEN7_ID = 50
TEKKEN7_CODE = 'nsa'
CODE = 'nsa'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/NSA/CS_NSA_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/NSA/CS_NSA_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/nsa/Face/nsa_fac/Meshes/SK_CH_nsa_fac'),
        ('bdl', _ITEM + '/Meshes/nsa/Lower/nsa_bdl_1p/Meshes/SK_CH_nsa_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/nsa/Upper/nsa_bdu_1p/Meshes/SK_CH_nsa_bdu_1p'),
        ('fah', _ITEM + '/Meshes/nsa/Face_Hair/nsa_fah_1p/Meshes/SK_CH_nsa_fah_1p'),
        ('har', _ITEM + '/Meshes/nsa/Hair/nsa_har_1p/Meshes/SK_CH_nsa_har_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'fah', 'har'],
}

MESHES = {
    'fac': ('nsa_fac', 'ATAMA_Joint', 0),
    'bdl': ('nsa_bdl', 'KOSHI_jnt', 1),
    'bdu': ('nsa_bdu', 'MUNE2_jnt', 2),
    'fah': ('nsa_fah', 'ATAMA_jnt_assist', 3),
    'har': ('nsa_har', 'ATAMA_jnt_assist', 4),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_NSA_7FR_DEF', (313, 224, 155)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_NSA', (616, 258, 150)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_NSA',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_NSA_01',
}

VOICE_PREFIX = 'na'

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[NEGAN]',
    'drop_moves': r'^Na_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['na_s%02d' % k for k in range(4)]),     # the donor's intros -> his
               (r'^la_w', 'anim', ['na_w%02d' % k for k in range(4)])],         # win poses
}

# his bat (Lucille) in his right hand, posed by his animations' second extra pair (the first poses the copy for his
# left hand, not drawn; weapons.py); the points it hits with (Tekken 7 hit locations: its tip, Tekken 7's own point;
# the others along the bat, in cm from Tekken 7's grip)
WEAPONS = [{'mesh': _ITEM + '/Meshes/nsa/extra/nsa_exp_1p/Meshes/SK_CH_nsa_exp_1p', 'bone': 'ATH_R_weapon001',
            'parent': 'TE_R_jnt', 'pair': 1, 'points': {0x70: 'ATH_R_weapon001_top', 0x72: 70, 0x6f: 45}}]
# the knife and the bat in his left hand (not drawn) hit next to the hand
HIT_LOCATIONS = {0x6b: 0x0d, 0x6e: 0x0d}
