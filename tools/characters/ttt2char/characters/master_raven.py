"""Master Raven from Tekken 7 (PC), in her default costume.

Only names, paths and rules live here; every byte of her model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. Her materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Master Raven'
SHORT_NAME = 'MASTER RAVEN'
TEKKEN7_ID = 29
TEKKEN7_CODE = 'frv'
CODE = 'frv'
SEX = 'female'

DONOR = 'jun'                 # Jun Kazama: the 139-bone female skeleton
DONOR_ROW = 42

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/FRV/CS_FRV_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/FRV/CS_FRV_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/frv/Face/frv_fac/Meshes/SK_CH_frv_fac'),
        ('bdl', _ITEM + '/Meshes/frv/Lower/frv_bdl_1p/Meshes/SK_CH_frv_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/frv/Upper/frv_bdu_1p/Meshes/SK_CH_frv_bdu_1p'),
        ('har', _ITEM + '/Meshes/frv/Hair/frv_har_1p/Meshes/SK_CH_frv_har_1p'),
        ('hed', _ITEM + '/Meshes/frv/Head/frv_hed_1p/Meshes/SK_CH_frv_hed_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har', 'hed'],
}

MESHES = {
    'fac': ('frv_fac', 'ATAMA_Joint', 0),
    'bdl': ('frv_bdl', 'KOSHI_jnt', 1),
    'bdu': ('frv_bdu', 'MUNE2_jnt', 2),
    'har': ('frv_har', 'ATAMA_jnt_assist', 3),
    'hed': ('frv_hed', 'ATAMA_jnt_assist', 4),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_FRV_7FR_DEF', (292, 268, 130)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_FRV', (566, 372, 80)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_FRV',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_FRV_01',
}

VOICE_PREFIX = 'fr'

# the id maps: Heihachi in both games, and her generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[MASTER RAVEN]',
    'drop_moves': r'^Fr_Direct$',
    'direct': [(r'^(ju|mk)_s', 'anim', ['fr_s%02d' % k for k in range(4)]),     # the donor's intros -> her
               (r'^ju_w', 'anim', ['fr_w%02d' % k for k in range(3)])],         # win poses
}

# Tekken 7 simulates her braids, which its mesh models straight out behind her head (and three over her left
# shoulder): they hang down her back and front and swing (convert/chains.py). (part, the chain's first bone, the
# directions its links hang in, from the first: x her left, y up, z her front)
def _back(x):
    return [(x, -0.3, -1), (x, -1, -0.6), (x * 0.5, -1, -0.25), (x * 0.3, -1, -0.1)]


CHAINS = [('har', 'Hair_OPT0%02d' % n, _back(x)) for n, x in ((16, 0), (21, -0.05), (26, 0.12), (31, 0.1), (36, 0.06),
                                                              (41, -0.01), (46, -0.08), (51, 0.01), (56, -0.02))]
CHAINS += [('har', 'Hair_OPT0%02d' % n, [(0.3, -1, 0.35), (0.1, -1, 0.15), (0.05, -1, 0.05), (0, -1, 0)])
           for n in (1, 6, 11)]

# her katana, sheathed on her back where Tekken 7 carries it (the copies for her hands are not drawn); the one move
# that draws it hits with its tip, 78 cm along the blade from her right hand (weapons.py)
_WEAPONS = _ITEM + '/Meshes/frv/extra/frv_exp_1p/Meshes/SK_CH_frv_exp_1p'
WEAPONS = [
    {'mesh': _WEAPONS, 'bone': 'ATH_katasaya', 'parent': 'MUNE2_jnt', 'pair': None},
    {'mesh': _WEAPONS, 'bone': 'ATH_katatuka', 'parent': 'MUNE2_jnt', 'pair': None},
    {'mesh': _WEAPONS, 'draw': False, 'bone': 'ATH_katanaR', 'parent': 'TE_R_jnt', 'pair': None,
     'points': {0x44: 78}},
]
