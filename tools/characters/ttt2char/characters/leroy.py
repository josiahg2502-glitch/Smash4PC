"""Leroy Smith from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. His materials are classified by the importer
(model.classify); MATERIALS lists only exceptions. His sunglasses (a static mesh on a socket) come with the costume
(model.socket_items).
"""
NAME = 'Leroy Smith'
SHORT_NAME = 'LEROY'
TEKKEN7_ID = 53
TEKKEN7_CODE = 'nsb'
CODE = 'nsb'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/NSB/CS_NSB_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/NSB/CS_NSB_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/nsb/Face/nsb_fac/Meshes/SK_CH_nsb_fac'),
        ('bdl', _ITEM + '/Meshes/nsb/Lower/nsb_bdl_1p/Meshes/SK_CH_nsb_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/nsb/Upper/nsb_bdu_1p/Meshes/SK_CH_nsb_bdu_1p'),
        ('har', _ITEM + '/Meshes/nsb/Hair/nsb_har_1p/Meshes/SK_CH_nsb_har_1p'),
        ('fah', _ITEM + '/Meshes/nsb/Face_Hair/nsb_fah_1p/Meshes/SK_CH_nsb_fah_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har', 'fah'],
}

MESHES = {
    'fac': ('nsb_fac', 'ATAMA_Joint', 0),
    'bdl': ('nsb_bdl', 'KOSHI_jnt', 1),
    'bdu': ('nsb_bdu', 'MUNE2_jnt', 2),
    'har': ('nsb_har', 'ATAMA_jnt_assist', 3),
    'fah': ('nsb_fah', 'ATAMA_jnt_assist', 4),
}

MATERIALS = {}
# his lenses: a tinted-glass shader without a colour texture; drawn in its tint
FLAT = {'MI_CH_nsb_gra_tr_1p_2': 'Color01'}

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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_NSB_7FR_DEF', (387, 204, 130)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_NSB', (588, 266, 100)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_NSB',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_NSB_01',
}

VOICE_PREFIX = 'nb'

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[LEROY]',
    'drop_moves': r'^Nb_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['nb_s%02d' % k for k in range(4)]),     # the donor's intros -> his
               (r'^la_w', 'anim', ['nb_w%02d' % k for k in range(3)])],         # win poses
}

# Tekken 7 simulates his dreadlocks, which its mesh models splayed out round his head: they hang down his back and
# over his shoulders and swing (convert/chains.py). (part, first bone, link directions: x his left, y up, z front)
CHAINS = [('har', 'nsb_1p_hair_OPT0%s' % n, [(x, -1, z)] * 3) for n, x, z in (
    ('01', 0.35, 0.35), ('05', 0.4, 0.2), ('09', 0.3, -0.35), ('13', 0.25, -0.4), ('17', 0.12, -0.4),
    ('21', 0, -0.4), ('25', -0.12, -0.4), ('029', -0.25, -0.4), ('033', -0.3, -0.35), ('37', -0.4, 0.2),
    ('41', -0.35, 0.35))]

# his cane, where Tekken 7 carries it (at his right hip; only one move of his draws it, the folding staff of his
# Rage Art, which is not drawn here: it hits next to his left hand)
WEAPONS = [{'mesh': _ITEM + '/Meshes/nsb/extra/nsb_exp_1p/Meshes/SK_CH_nsb_exp_1p', 'bone': 'ATH_cane',
            'parent': 'KOSHI_jnt', 'pair': None}]
HIT_LOCATIONS = {0x73: 0x0d}
