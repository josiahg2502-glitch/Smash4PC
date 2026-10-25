"""Lucky Chloe from Tekken 7 (PC), in her default costume.

Only names, paths and rules live here; every byte of her model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs.
"""
NAME = 'Lucky Chloe'
SHORT_NAME = 'CHLOE'          # her name on the 2D screens (TTT2 shows short names: KAZUYA, HEIHACHI)
TEKKEN7_ID = 22               # her id in Tekken 7's roster (TKMovesets2 writes it into the .tkmvst header)
TEKKEN7_CODE = 'dnc'          # her code in Tekken 7's files (AkBankVoiceDNC.bnk, Item/Meshes/dnc/)
CODE = 'dnc'                  # her three-letter code in the pack (not a code TTT2 already has)
SEX = 'female'

DONOR = 'jun'                 # Jun Kazama: the 139-bone female skeleton
DONOR_ROW = 42                # Jun's motion row (data005 / data006 entry)

_ITEM = '/Game/Character/Item'
_MESHES = _ITEM + '/Meshes/dnc'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/DNC/CS_DNC_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/DNC/CS_DNC_DefaultCos2P',
    'parts': [
        ('fac', _MESHES + '/Face/dnc_fac/Meshes/SK_CH_dnc_fac'),
        ('har', _MESHES + '/Hair/dnc_har_1p/Meshes/SK_CH_dnc_har_1P'),
        ('hed', _MESHES + '/Head/dnc_hed_1p/Meshes/SK_CH_dnc_hed_1p'),      # the cat-ear headphones
        ('bdu', _MESHES + '/Upper/dnc_bdu_1p/Meshes/SK_CH_dnc_bdu_1p'),
        ('bdl', _MESHES + '/Lower/dnc_bdl_1p/Meshes/SK_CH_dnc_bdl_1p'),
        ('tail', _MESHES + '/Lower_Acc/dnc_opl_tail_1p/Meshes/SK_CH_dnc_opl_tail'),   # her tail (CHAINS)
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har', 'hed', 'tail'],
}

# Tekken 7 simulates her twin tails and her tail, which its meshes model straight out from her (the twin tails 1.3 m
# out sideways, the tail up behind her): they hang at rest and swing (convert/chains.py). (part, the chain's first
# bone, the directions its links hang in, from the first: x her left, y up, z her front)
CHAINS = [
    ('har', 'Hair_L_OPT001', [(1, 0, 0), (1, -0.5, -0.25), (0.45, -1, -0.3), (0.15, -1, -0.25), (0.05, -1, -0.2),
                              (0, -1, -0.15)]),
    ('har', 'Hair_R_OPT001', [(-1, 0, 0), (-1, -0.5, -0.25), (-0.45, -1, -0.3), (-0.15, -1, -0.25),
                              (-0.05, -1, -0.2), (0, -1, -0.15)]),
    ('tail', 'Hip__offset_B_OPT001', [(0, -0.2, -1), (0, -0.5, -1), (0, -1, -1), (0, -1, -0.6), (0, -1, -0.35),
                                      (0, -1, -0.2)]),
]

MESHES = {
    'bdl': ('dnc_bdl_1p', 'KOSHI_jnt', 0),
    'tail': ('dnc_opl_tail', 'KOSHI_jnt', 1),
    'bdu': ('dnc_bdu_1p', 'MUNE2_jnt', 2),
    'fac': ('dnc_fac', 'ATAMA_Joint', 3),
    'har': ('dnc_har_1p', 'ATAMA_jnt_assist', 4),
    'hed': ('dnc_hed_1p', 'ATAMA_jnt_assist', 5),
}

MATERIALS = {
    'MI_CH_dnc_mouth': ('mouth', True, False),
    'MI_CH_dnc_teeth': ('mouth', False, False),
    'MI_CH_dnc_Eyes': ('eye', False, False),
    'MI_CH_dnc_eyelash': ('overlay', False, True),
    'MI_CH_dnc_eye_shadow': ('overlay', False, True),
    'MI_CH_dnc_skin_face': ('skin', True, False),
    'MI_CH_dnc_eyebrow': ('overlay', False, True),
    'MI_CH_dnc_lip': ('overlay', False, True),
    'MI_CH_tear': None,                           # a wet layer over the eyes
    'MI_CH_dnc_Eye_ADD': None,                    # an additive reflection pass
    'MI_CH_dnc_hair_base1': ('hair', False, True),
    'MI_CH_dnc_hair_base2': None,                 # the second (blended) pass over the same hair triangles
    'MI_CH_dnc_hair_mae1': ('hair', False, True), # her fringe
    'MI_CH_dnc_hair_mae2': None,                  # its second pass
    'MI_CH_dnc_hed_1p': ('cloth', True, False),
    'MI_CH_dnc_bdu_1p_01': ('cloth', True, False),
    'MI_CH_dnc_bdu_1p_03': ('cloth', True, False),
    'MI_CH_dnc_skin_arm': ('skin', True, False),
    'MI_CH_dnc_skin_neck': ('skin', True, False),
    'MI_CH_dnc_skin_body': ('skin', True, False),
    'MI_CH_dnc_bdl_1p_01': ('cloth', True, False),
    'MI_CH_dnc_bdl_1p_02': ('cloth', True, False),
    'MI_CH_dnc_bdl_1p_03': ('cloth', True, False),
    'MI_CH_dnc_skin_leg': ('skin', True, False),
    'MI_CH_dnc_opl_tail': ('cloth', True, False),
}

TEMPLATES = {
    'skin': ('jun.nud', 'ude_A_O_OBJ', 0),        # skin with a normal map
    'mouth': ('jun.nud', 'cha:kao_O_OBJ', 0),     # mouth and teeth
    'eye': ('jun.nud', 'cha:kao_O_OBJ', 2),
    'overlay': ('jun.nud', 'cha:kao_O_OBJ', 7),   # a blended layer over the face
    'cloth': (0xD4F28D09, 'jun_upb_1p_b', 0),     # a costume part of Jun's (her model pack, member 95)
    'hair': (0x5F412CA4, 'jun_hed_1p', 1),        # (member 7): alpha-tested, two-sided
    'colour': (0xD4F28D09, 'jun_upb_1p_b', 2),    # colourable cloth (2P colour layers)
    'colour_alpha': (0xD4F28D09, 'jun_upb_1p_b', 1),   # its alpha-tested variant (the layers take its alpha test)
}
# where the templates' own textures are (their average colours fill the slots Tekken 7 has nothing for)
TEMPLATE_TEXTURES = ['jun.nut', 0x24F8AE59, 0x277C7A37, 0xAD9B60F0, 0x900FD8C4]

# ------------------------------------------------------------------------------------------------ pictures
# her Tekken 7 UI pictures (convert/art.py): a bust and her whole body, each with where her face is in it (the eyes'
# centre x, y and eyes-to-chin, in its pixels), and her name
_UI = '/Game/UI_Common/Texture2D/ReplaceImages'
ART = {
    'bust': (_UI + '/VS_CH_L/VS_CH_L_DNC_TK7_DEF', (388, 269, 126)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_DNC', (676, 314, 115)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_DNC',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_DNC_01',     # the costume chooser's picture (full body)
}

# ------------------------------------------------------------------------------------------------ voice
VOICE_PREFIX = 'dc'           # her intro and win lines' events in Tekken 7's bank: dc_s00.., dc_w00..

# ------------------------------------------------------------------------------------------------ moveset
# the id maps: Heihachi's moveset in both games, and her generic moves against some TTT2 fighters' (importer.py)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', 42), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[CHLOE]',
    'drop_moves': r'^Dc_Direct$',
    'direct': [(r'^Ju_sKAM00_$', 'move', ['Dc_sKAM00_']),                       # Jun's stance -> hers
               (r'^(ju|mk)_s', 'anim', ['dc_s%02d' % k for k in range(3)]),     # intros
               (r'^ju_w', 'anim', ['dc_w%02d' % k for k in range(3)])],         # win poses
}
