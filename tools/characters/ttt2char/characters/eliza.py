"""Eliza from Tekken 7 (PC), in her default costume.

Only names, paths and rules live here; every byte of her model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. Her meter (the gauge her EX moves and Super
spend in Tekken 7) is not converted.
"""
NAME = 'Eliza'
SHORT_NAME = 'ELIZA'          # her name on the 2D screens
TEKKEN7_ID = 36               # her id in Tekken 7's roster (TKMovesets2 writes it into the .tkmvst header; it calls her
                              # "Vampire")
TEKKEN7_CODE = 'elz'          # her code in Tekken 7's files (AkBankVoiceELZ.bnk, Item/Meshes/elz/)
CODE = 'elz'                  # her three-letter code in the pack (not a code TTT2 already has)
SEX = 'female'

DONOR = 'jun'                 # Jun Kazama: the 139-bone female skeleton
DONOR_ROW = 42                # Jun's motion row (data005 / data006 entry)

_ITEM = '/Game/Character/Item'
_MESHES = _ITEM + '/Meshes/elz'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/ELZ/CS_ELZ_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/ELZ/CS_ELZ_DefaultCos2P',
    'parts': [
        ('fac', _MESHES + '/face/elz_fac/Meshes/SK_CH_elz_fac'),           # with her horns
        ('har', _MESHES + '/hair/elz_har_1p/Meshes/SK_CH_elz_har_1p'),
        ('bdu', _MESHES + '/upper/elz_bdu_1p/Meshes/SK_CH_elz_bdu_1p'),
        ('bdl', _MESHES + '/lower/elz_bdl_1p/Meshes/SK_CH_elz_bdl_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har'],
}

# Tekken 7 simulates her two long front locks, which its mesh models bent forward at shoulder height (half a metre
# out in front of her): they hang down her front at rest and swing (convert/chains.py). (part, the chain's first
# bone, the directions its links hang in, from the first: x her left, y up, z her front)
CHAINS = [
    ('har', 'Hair_OPT034', [(0, -1, 0), (0, -1, 0.9), (0, -1, 0.8), (0, -1, 0.6), (0, -1, 0.3), (0, -1, 0.05),
                            (0, -1, -0.1), (0, -1, -0.15)]),
    ('har', 'Hair_OPT043', [(0, -1, 0), (0, -1, 0.9), (0, -1, 0.8), (0, -1, 0.6), (0, -1, 0.3), (0, -1, 0.05),
                            (0, -1, -0.1), (0, -1, -0.15)]),
]

MESHES = {
    'bdl': ('elz_bdl_1p', 'KOSHI_jnt', 0),
    'bdu': ('elz_bdu_1p', 'MUNE2_jnt', 1),
    'fac': ('elz_fac', 'ATAMA_Joint', 2),
    'har': ('elz_har_1p', 'ATAMA_jnt_assist', 3),
}

MATERIALS = {
    'MI_CH_mouth': ('mouth', True, False),
    'MI_CH_elz_teeth': ('mouth', False, False),
    'MI_CH_elz_Eyes': ('eye', False, False),
    'MI_CH_elz_Eye_ADD': None,                    # an additive reflection pass
    'MI_CH_tear': None,                           # a wet layer over the eyes
    'MI_CH_elz_eye_shadow': ('overlay', False, True),
    'MI_CH_elz_eyelash': ('overlay', False, True),
    'MI_CH_elz_eyebrow': ('overlay', False, True),
    'MI_CH_elz_lip': ('overlay', False, True),
    'MI_CH_elz_skin_face': ('skin', True, False),
    'MI_CH_elz_fac_horn': ('cloth', True, False),
    'MI_CH_elz_har_1p': ('hair', False, True),
    'MI_CH_elz_har_1p_gradation': ('hair', False, True),     # the long locks' red ends
    'MI_CH_elz_har_1p_suki_hair': ('hair', False, True),
    'MI_CH_elz_har_1p2': None,                    # the second (blended) passes over the same hair triangles
    'MI_CH_elz_har_1p_gradation2': None,
    'MI_CH_elz_skin_neck': ('skin', True, False),
    'MI_CH_elz_skin_hand': ('skin', True, False),
    'MI_CH_elz_skin_arm': ('skin', True, False),
    'MI_CH_elz_skin_body': ('skin', True, False),
    'MI_CH_elz_skin_leg': ('skin', True, False),
    'MI_CH_elz_bdu_1p_Rose_': ('cloth', True, False),
    'MI_CH_elz_bdu_1p_button': ('cloth', True, False),
    'MI_CH_elz_bdu_1p_dress': ('cloth', True, False),
    'MI_CH_elz_bdu_1p_dressura': ('cloth', True, False),
    'MI_CH_elz_bdu_1p_sode': ('cloth', True, False),
    'MI_CH_elz_bdu_1p_sodefril': ('cloth', True, True),      # lace frills: alpha-tested
    'MI_CH_elz_bdu_1p_kata': ('cloth', True, False),
    'MI_CH_elz_bdl_1p_pants': ('cloth', True, False),
    'MI_CH_elz_bdl_1p_tights_race': ('cloth', True, True),   # the stockings' lace tops
    'MI_CH_elz_bdl_1p_shoes': ('cloth', True, False),
    'MI_CH_elz_bdl_1p_tights': ('cloth', True, True),
    'MI_CH_elz_bdl_1p_skirt': ('cloth', True, False),
    'MI_CH_elz_bdl_1p_Rose_': ('cloth', True, False),
}

TEMPLATES = {
    'skin': ('jun.nud', 'ude_A_O_OBJ', 0),        # skin with a normal map
    'mouth': ('jun.nud', 'cha:kao_O_OBJ', 0),     # mouth and teeth
    'eye': ('jun.nud', 'cha:kao_O_OBJ', 2),
    'overlay': ('jun.nud', 'cha:kao_O_OBJ', 7),   # a blended layer over the face
    'cloth': (0xD4F28D09, 'jun_upb_1p_b', 0),     # a costume part of Jun's (her model pack, member 95)
    'hair': (0x5F412CA4, 'jun_hed_1p', 1),        # (member 7): alpha-tested, two-sided
    'colour': (0xD4F28D09, 'jun_upb_1p_b', 2),    # colourable cloth (2P colour layers)
    'colour_alpha': (0xD4F28D09, 'jun_upb_1p_b', 1),   # its alpha-tested variant (layers and cloth with holes)
}
# where the templates' own textures are (their average colours fill the slots Tekken 7 has nothing for)
TEMPLATE_TEXTURES = ['jun.nut', 0x24F8AE59, 0x277C7A37, 0xAD9B60F0, 0x900FD8C4]

# ------------------------------------------------------------------------------------------------ pictures
# her Tekken 7 UI pictures (convert/art.py): a bust and her whole body, each with where her face is in it (the eyes'
# centre x, y and eyes-to-chin, in its pixels), and her name
_UI = '/Game/UI_Common/Texture2D/ReplaceImages'
ART = {
    'bust': (_UI + '/VS_CH_L/VS_CH_L_ELZ_7FR_DEF', (367, 245, 122)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_ELZ', (625, 310, 115)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_ELZ',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_ELZ_01',     # the costume chooser's picture (full body)
}

# ------------------------------------------------------------------------------------------------ voice
VOICE_PREFIX = 'vp'           # her intro and win lines' events in Tekken 7's bank: vp_s00.., vp_w00..

# ------------------------------------------------------------------------------------------------ moveset
# the id maps: Heihachi's moveset in both games, and her generic moves against some TTT2 fighters' (importer.py)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', 42), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[ELIZA]',
    'drop_moves': r'^Vp_Direct$',
    'direct': [(r'^Ju_sKAM00_$', 'move', ['Jn_sKAM00_']),                       # Jun's stance -> hers
               (r'^(ju|mk)_s', 'anim', ['vp_s%02d' % k for k in range(5)]),     # intros
               (r'^ju_w', 'anim', ['vp_w%02d' % k for k in range(3)])],         # win poses
}
