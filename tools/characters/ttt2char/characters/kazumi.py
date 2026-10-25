"""Kazumi Mishima from Tekken 7 (PC), in her default costume.

Only names, paths and rules live here; every byte of her model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs.
"""
NAME = 'Kazumi Mishima'
SHORT_NAME = 'KAZUMI'          # her name on the 2D screens (TTT2 shows short names: KAZUYA, HEIHACHI)
TEKKEN7_ID = 26               # her id in Tekken 7's roster (TKMovesets2 writes it into the .tkmvst header)
TEKKEN7_CODE = 'kzm'          # her code in Tekken 7's files (AkBankVoiceKZM.bnk, Item/Meshes/kzm/)
CODE = 'kzm'                  # her three-letter code in the pack (not a code TTT2 already has)
SEX = 'female'

# The donor: a TTT2 fighter with a similar body and style. Its skeleton is the base of hers, its materials are the
# templates of hers, its moveset gives the generic and tag moves, and its voice and effects are used for her.
DONOR = 'jun'                 # Jun Kazama: the 139-bone female skeleton
DONOR_ROW = 42                # Jun's motion row (data005 / data006 entry)

# ------------------------------------------------------------------------------------------------ model
_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    # colours of the default costume, player 1 and player 2 (convert/colours.py makes TTT2's 2P colours from both)
    'costume': _ITEM + '/Sets/KZM/CS_KZM_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/KZM/CS_KZM_DefaultCos2P',
    # the costume's skeletal meshes, by part, in this order
    'parts': [
        ('fac', _ITEM + '/Meshes/kzm/Face/kzm_fac/Meshes/SK_CH_kzm_fac'),
        ('har', _ITEM + '/Meshes/kzm/Hair/kzm_har_1p/Meshes/SK_CH_kzm_har_1p'),
        ('bdu', _ITEM + '/Meshes/kzm/Upper/kzm_bdu_1p/Meshes/SK_CH_kzm_bdu_1p'),
        ('bdl', _ITEM + '/Meshes/kzm/Lower/kzm_bdl_1p/Meshes/SK_CH_kzm_bdl_1p'),
    ],
    # the order their skeletons merge in: face joints from the face, body joints from the body
    'merge_order': ['fac', 'bdl', 'bdu', 'har'],
}

# part -> (TTT2 mesh name, the bone the mesh is bound to as a whole, draw order)
# Her tiger: Tekken 7 draws it as an actor of its own; here it becomes extra bones of hers, shown by her tiger moves
# (convert/tiger.py). Its animations by the index her moves give them (move property 0x828f); index 12 (Ka_test_n,
# a test move) has none.
_TIGER = '/Game/Character/CharacterAlly/KazumiTiger'
TIGER = {
    'mesh': _TIGER + '/Mesh/SK_CH_kzm_tiger',
    'animations': {
        0: _TIGER + '/Anim/SA_tiger_kazumiStart',             # intro (ka_s01)
        1: _TIGER + '/Anim/SA_tiger_kazumiVictory',           # win pose (ka_w01)
        2: _TIGER + '/Anim/SA_tiger_kazumiSpecialStart',      # ka_spstart01
        6: _TIGER + '/Anim/TA_Missle00_tackle',               # 2+3 (Ka_Tora00) and others
        7: _TIGER + '/Anim/TA_Missle01_rolling',              # 1+4 (Ka_Tora01), Ka_spamarm2
        9: _TIGER + '/Anim/TA_Effect00_tigeruc',              # Ka_66rp_EX2
        17: _TIGER + '/Anim/TA_Missile02_tigerattack',        # Ka_t_srpFH, Ka_10comb11
        18: _TIGER + '/Anim/TA_Effect01_chouhatsu1P',         # taunt (2+3+4)
        19: _TIGER + '/Anim/TA_Effect02_chouhatsu_2P',        # taunt as player 2
    },
}

MESHES = {
    'bdl': ('kzm_bdl_1p', 'KOSHI_jnt', 0),
    'bdu': ('kzm_bdu_1p', 'MUNE2_jnt', 1),
    'fac': ('kzm_fac', 'ATAMA_Joint', 2),
    'har': ('kzm_har_1p', 'ATAMA_jnt_assist', 3),
    'tiger': ('kzm_tiger', 'OPT600_HUMAN__group', 4),
}

# Tekken 7 material instance -> (template, use its normal map, keep the colour texture's alpha); None: left out
MATERIALS = {
    'MI_CH_mouth': ('mouth', True, False),
    'MI_CH_ask_teeth': ('mouth', False, False),
    'MI_CH_kzm_Eyes': ('eye', False, False),
    'MI_CH_kzm_eyelash': ('overlay', False, True),
    'MI_CH_kzm_eye_shadow': ('overlay', False, True),
    'MI_CH_kzm_skin_face': ('skin', True, False),
    'MI_CH_kzm_eyebrow': ('overlay', False, True),
    'MI_CH_kzm_lip': ('overlay', False, True),
    'MI_CH_kzm_Eye_ADD': None,                    # an additive reflection pass
    'MI_CH_kzm_hair2': None,                      # the second (blended) pass over the same hair triangles
    'MI_CH_kzm_hair_base1_22': ('hair', False, True),
    'MI_CH_kzm_hed_1P': ('cloth', True, False),
    'MI_CH_kzm_bdu_1p_02': ('cloth', True, False),
    'MI_CH_kzm_bdu_1p_01': ('cloth', True, False),
    'MI_CH_kzm_skin_body': ('skin', True, False),
    'MI_CH_kzm_skin_neck': ('skin', True, False),
    'MI_CH_kzm_skin_arm': ('skin', True, False),
    'MI_CH_kzm_skin_hand': ('skin', True, False),
    'MI_CH_kzm_bdl_1p_03': ('cloth', True, False),
    'MI_CH_kzm_bdl_1p_02': ('cloth', True, False),
    'MI_CH_kzm_bdl_1p_pants': ('cloth', True, False),
    'MI_CH_kzm_skin_leg': ('skin', True, False),
    'MI_CH_kzm_bdl_1p_01': ('cloth', True, False),
    'MI_CH_kzm_tiger01': ('skin', True, False),
    'MI_CH_kzm_tiger02': ('skin', True, False),
    'MI_CH_kzm_tiger_Eyes': ('eye', False, False),
    'MI_CH_kzm_tiger_hair': ('hair', False, True),
    'MI_CH_kzm_tiger_fur': None,                  # a shell-fur pass over the tiger's body
}

# Material templates in the donor's files: (archive entry name or key, mesh, polygon)
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_KZM_TK7_DEF', (314, 278, 142)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_KZM', (572, 318, 107)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_KZM',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_KZM_01',     # the costume chooser's picture (full body)
}

# ------------------------------------------------------------------------------------------------ voice
# her Tekken 7 voice events: <TEKKEN7_CODE>_atk_s01 .. (shouts, damage, KO, special), <prefix>_s00 .. (intro lines),
# <prefix>_w00 .. (win quotes)
VOICE_PREFIX = 'ka'

# ------------------------------------------------------------------------------------------------ moveset
# The id maps come from aligning movesets of both games, in this order: (moveset, TTT2 motion row) with 'heihachi'
# = Tekken 7 Heihachi (the same fighter in both games) and 'self' = this fighter's (its generic moves). TTT2 rows:
# 13 Heihachi, 14 Kazuya, 9 Jin, 42 Jun, 23 Asuka.
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', 42), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[KAZUMI]',
    # her own moves that are not converted (besides Rage Arts and story moves): customisation and story
    # variants, and her round-start move Ka_Direct (the donor's is kept)
    'drop_moves': r'^kz_co|^kz_90|Ka_Direct|^ka_sp',
    # the donor's round-start list keeps its targets except its own moves: (donor move name pattern, 'move': the
    # cancel goes to her move instead / 'anim': the donor's move stays (its camera, timing, voice) and plays one of
    # her animations, in turn; a win pose takes the one whose quote it plays)
    'direct': [(r'^Ju_sKAM00_$', 'move', ['Ka_sKAM00_']),                       # Jun's stance -> hers
               (r'^(ju|mk)_s', 'anim', ['ka_s%02d' % k for k in range(8)]),     # intros
               (r'^ju_w', 'anim', ['ka_w%02d' % k for k in range(3)])],         # win poses
}
