"""Kunimitsu II from Tekken 7 (PC), in her default costume.

Only names, paths and rules live here; every byte of her model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. Her materials are classified by the importer
(model.classify); MATERIALS lists only exceptions. TTT2 has a Kunimitsu with her Tekken 7 code knm: her pack code is kmi.
"""
NAME = 'Kunimitsu II'
SHORT_NAME = 'KUNIMITSU'
TEKKEN7_ID = 55
TEKKEN7_CODE = 'knm'
CODE = 'kmi'
SEX = 'female'

DONOR = 'jun'                 # Jun Kazama: the 139-bone female skeleton
DONOR_ROW = 42

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/KNM/CS_KNM_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/KNM/CS_KNM_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/knm/Face/knm_fac/Meshes/SK_CH_knm_fac'),
        ('bdl', _ITEM + '/Meshes/knm/Lower/knm_bdl_1p/Meshes/SK_CH_knm_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/knm/Upper/knm_bdu_1p/Meshes/SK_CH_knm_bdu_1p'),
        ('har', _ITEM + '/Meshes/knm/Hair/knm_har_1p/Meshes/SK_CH_knm_har_1p'),
        ('gla', _ITEM + '/Meshes/knm/Glasses/knm_gla_1p/Meshes/SK_CH_knm_gla_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har', 'gla'],
}

MESHES = {
    'fac': ('kmi_fac', 'ATAMA_Joint', 0),
    'bdl': ('kmi_bdl', 'KOSHI_jnt', 1),
    'bdu': ('kmi_bdu', 'MUNE2_jnt', 2),
    'har': ('kmi_har', 'ATAMA_jnt_assist', 3),
    'gla': ('kmi_gla', 'ATAMA_jnt_assist', 4),
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
    'bust': (_UI + '/VS_CH_L/VS_CH_L_KNM_7FR_DEF_1', (320, 247, 115)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_KNM', (564, 260, 105)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_KNM',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_KNM_01',
}

VOICE_PREFIX = 'kt'

# the id maps: Heihachi in both games, and her generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'name': '[KUNIMITSU]',
    'drop_moves': r'^Kt_Direct$',
    'direct': [(r'^(ju|mk)_s', 'anim', ['kt_s%02d' % k for k in range(6)]),     # the donor's intros -> her
               (r'^ju_w', 'anim', ['kt_w%02d' % k for k in range(4)])],         # win poses
}

# Tekken 7 simulates her scarf's back flap as cloth its mesh models as a sheet a metre behind her (on a rigid bone),
# and her mask has a second copy for the moves she holds it in (on a hand attachment bone): both left out.
MATERIALS['MI_CH_knm_bdu_1p_MufflerBack'] = None
HIDE = [('gla', 'ATH_L_hand_mask_knm')]
# her fringe is modelled through the mask (its hair item has no variant for it: the one her other fox mask names has
# no scalp); Tekken 7 shows the mask over it: the hair in front of the mask's face is left out
COVERED = [('har', 'MI_CH_knm_gla_1p_Mask')]

# her blades, not drawn, posed by her animations' extra pairs (weapons.py): the left hand's short sword (pair 0; with
# the other left-hand pairs it is the kunai she throws, which Tekken 7 flies by these pairs, up to 7 m out) and the
# right hand's (pair 3); the points they hit with (Tekken 7 hit locations) are Tekken 7's own: the swords' tips, the
# kunai's ends
_WEAPONS = _ITEM + '/Meshes/knm/extra/knm_exp_1p/Meshes/SK_CH_knm_exp_1p'
WEAPONS = [
    {'mesh': _WEAPONS, 'draw': False, 'bone': 'ATH_L_weapon001', 'parent': 'TE_L_jnt', 'pair': 0,
     'points': {0x74: 'ATH_L_weapon001_top', 0x76: 'ATH_L_weapon001_top', 0x77: 'ATH_L_weapon001_hilp'}},
    {'mesh': _WEAPONS, 'draw': False, 'bone': 'ATH_R_weapon001', 'parent': 'TE_R_jnt', 'pair': 3,
     'points': {0x75: 'ATH_R_weapon001_top'}},
]
# her Piercing Kunai Throw (d, d/f, f + 1) and the kunai ending her low-high kicks: Tekken 7 flies the kunai by those
# pairs, which TTT2 cannot follow at range; a TTT2 projectile instead (convert/moveset.py thrown_rows), flying as the
# kunai does: from 1.2 m ahead of her hips at its first active frame, 1.1 m up, 460 mm a frame for 13 frames
# The kunai is seen flying with it: the blade of her left hand (its triangles in her weapon mesh, tip forward), with
# the speed line Tekken 7 trails behind it (her effect 40)
_KUNAI = {'mesh': _WEAPONS, 'bone': 'ATH_L_weapon001', 'tip': 'ATH_L_weapon001_top'}
THROWN = {'Kt_236LP': dict(frame=15, forward=1200, up=350, speed=460, frames=13, radius=150, height=250,
                           item=_KUNAI, trail=40),
          'Kt_2lkrk_LP': dict(frame=26, forward=1200, up=350, speed=470, frames=13, radius=150, height=250,
                              item=_KUNAI, trail=40)}
