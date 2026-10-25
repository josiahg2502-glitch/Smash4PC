"""The added fighter's TTT2 skeleton: the donor's skeleton with its joints moved to the Tekken 7 fighter's joints,
and which TTT2 bone each Tekken 7 bone's skin weights go to.

* All the donor's bones are kept (names, order, parents, rest rotations, OPT list, parameters): bones 0..137 have the
  same names, order and parents in every TTT2 skeleton and the game drives helpers by index, so nothing is reordered.
* Joint positions move to the Tekken 7 fighter's: animated joints, assists and fingers are anchored to the Tekken 7
  joint with the same role (ANCHOR); limb helpers keep their place along their limb segment (fraction t and a
  sideways offset); everything else moves with its parent.
* Face joints keep the donor's offsets to the head: the face animations set absolute local positions made for the
  donor's face, so a face joint moved to the new face would be pulled back by every face animation (the lips pushed
  into the face).
* Scale = the Tekken 7 BASE joint's height / 1150 (TTT2 puts HARA_ROT1 at 1150 x scale); body bone scales follow it,
  hand and face scales stay the donor's.
TTT2 units are millimetres, Y up, the fighter facing +Z with its left at +X.
"""
import numpy as np

from ..ttt2 import bone

# TTT2 bone (name without 'cha:') -> the Tekken 7 bone whose bind position anchors it
ANCHOR = {'HARA_ROT1': 'BASE', 'MUNE1_jnt': 'Spine1', 'MUNE2_jnt': 'Spine2', 'KOSHI_jnt': 'Hip',
          'KUBI_jnt_assist': 'Neck', 'ATAMA_jnt_assist': 'Head', 'KUBI_Joint': 'Neck', 'ATAMA_Joint': 'Head'}
for s in 'RL':
    ANCHOR.update({'SAKOTSU_%s_jnt' % s: '%s_Shoulder' % s, 'KATA_%s_jnt' % s: '%s_Arm' % s,
                   'UDE_%s_jnt' % s: '%s_ForeArm' % s, 'TE_%s_jnt' % s: '%s_Hand' % s,
                   'MOMO_%s_jnt' % s: '%s_UpLeg' % s, 'HIZA_%s_jnt' % s: '%s_Leg' % s,
                   'ASHI_%s_jnt' % s: '%s_Foot' % s, 'TSUMASAKI_%s_jnt' % s: '%s_Toe' % s,
                   'KO_%s_BASE' % s: '%s_Pinky' % s})
    for ttt2, t7 in (('OYA', 'Thumb'), ('HITO', 'Index'), ('NAKA', 'Middle'), ('KUSU', 'Ring'), ('KO', 'Pinky')):
        for k in (1, 2, 3):                       # TTT2 3 = proximal ... 1 = distal; Tekken 7 1 = proximal
            ANCHOR['%s%d_%s' % (ttt2, 4 - k, s)] = '%s_%s%d' % (s, t7, k)
FACE = ['L_Mayu1_Joint', 'L_Mayu2_Joint', 'L_Mayu3_Joint', 'R_Mayu1_Joint', 'R_Mayu2_Joint', 'R_Mayu3_Joint',
        'L_Eye_Joint', 'L_UwaMabuta_Joint', 'L_SitaMabuta_Joint', 'R_Eye_Joint', 'R_UwaMabuta_Joint',
        'R_SitaMabuta_Joint', 'L_LipU_Joint', 'L_LipE_Joint', 'L_LipD_Joint', 'R_LipU_Joint', 'R_LipE_Joint',
        'R_LipD_Joint', 'M_LipU_Joint', 'M_LipD_Joint', 'Jaw_Joint', 'Bero1_Joint', 'Bero2_Joint', 'L_Hana_Joint',
        'L_Hoho_Joint', 'L_Chobo_Joint', 'L_Kobana_Joint', 'R_Hana_Joint', 'R_Hoho_Joint', 'R_Chobo_Joint',
        'R_Kobana_Joint']
# joints that keep the donor's offset to another (already placed) bone instead of an anchor; the face joints keep
# theirs to the head (face animations are made for the donor's face)
KEEP_OFFSET_TO = {'KUBI_jnt': 'KUBI_jnt_assist', 'ATAMA_jnt': 'ATAMA_jnt_assist'}
KEEP_OFFSET_TO.update({f: 'ATAMA_Joint' for f in FACE})
# limb segments (TTT2 names) for helpers
SEGMENTS = []
for s in 'RL':
    SEGMENTS += [('SAKOTSU_%s_jnt' % s, 'KATA_%s_jnt' % s), ('KATA_%s_jnt' % s, 'UDE_%s_jnt' % s),
                 ('UDE_%s_jnt' % s, 'TE_%s_jnt' % s), ('MOMO_%s_jnt' % s, 'HIZA_%s_jnt' % s),
                 ('HIZA_%s_jnt' % s, 'ASHI_%s_jnt' % s), ('ASHI_%s_jnt' % s, 'TSUMASAKI_%s_jnt' % s)]


# the Tekken 7 joints this file names, by their lower-case names: some skeletons spell them otherwise (Gigas's
# 'Base', 'HEAD'); canonical() gives the spelling used here
T7_JOINTS = set(ANCHOR.values()) | set(FACE) | {
    'Head', 'Head__offset', 'ATH_ornaments', 'Neck', 'Neck__offset', 'Neck_back__offset', 'Bero1_Joint_pos',
    'Bero3_Joint', 'Spine2', 'Spine2__offset', 'Spine2up__offset', 'Spine1', 'Spine1__offset', 'Hip', 'Hip__offset',
    'Hip_front__offset', 'BASE'}
_CANONICAL = {n.lower(): n for n in T7_JOINTS}


def canonical(name):
    """A Tekken 7 joint's name as this file spells it (unknown names as they are)."""
    return _CANONICAL.get(name.lower(), name)


def _strip(n):
    return n[4:] if n.startswith('cha:') else n


def t7_weight_target(t7name, part):
    """TTT2 bone name for a weighted T7 bone (None = resolve through the T7 parent)."""
    n = t7name.split('@')[0]
    if n in FACE:
        return n
    if n in ('Head', 'Head__offset'):
        return 'ATAMA_Joint' if part == 'fac' else 'ATAMA_jnt_assist'
    if n == 'ATH_ornaments':
        return 'ATAMA_jnt_assist'
    if n in ('Neck', 'Neck__offset', 'Neck_back__offset'):
        return 'KUBI_jnt_assist'
    if n.endswith('_UpEyelid1') or n.endswith('_UpEyelid2'):
        return n[0] + '_UwaMabuta_Joint'
    if n.endswith('_LowEyelid1') or n.endswith('_LowEyelid2'):
        return n[0] + '_SitaMabuta_Joint'
    if n.endswith('_Eyecorner'):
        return 'ATAMA_Joint'
    if n == 'Bero1_Joint_pos':
        return 'Bero1_Joint'
    if n == 'Bero3_Joint':
        return 'Bero2_Joint'
    if n in ('Spine2', 'Spine2__offset', 'Spine2up__offset'):
        return 'MUNE2_jnt'
    if n in ('Spine1', 'Spine1__offset'):
        return 'MUNE1_jnt'
    if n in ('Hip', 'Hip__offset', 'Hip_front__offset', 'BASE'):
        return 'KOSHI_jnt'
    if 'bust' in n:
        return 'OPT0_busts'
    if len(n) > 2 and n[1] == '_' and n[0] in 'LR' or n.startswith('HLP_'):
        hlp = n.startswith('HLP_')
        s = n[4] if hlp else n[0]
        base = n[6:] if hlp else n[2:]
        base = base.replace('__offset', '')
        lo = s.lower()
        table = {
            'Butt': 'HIP_%s' % s, 'Inguinal': 'MOMO_F0_%s' % s, 'UpLeg': 'MOMO_0_%s' % s,
            'UpLeg_Top': 'MOMO_0_%s' % s, 'UpLeg_Mid': 'MOMO_50_%s' % s, 'Knee': 'HIZA_%s' % s,
            'Leg': 'SUNE_%s' % s, 'Calf_Slide': 'slide_fukura_%s' % lo, 'Foot': 'ASHI_%s_jnt' % s,
            'Ankle': 'ASHI_%s_jnt' % s, 'Toe': 'TSUMASAKI_%s_jnt' % s,
            'Shoulder': 'SAKOTSU_%s_jnt' % s, 'Shldr_B': 'KATA_B0_%s' % s, 'ShldrBlade': 'KENKO_%s_jnt' % s,
            'CollarBone': 'SAKOTSU_0_%s_jnt' % s, 'CollarBone_up': 'SAKOTSU_0_%s_jnt' % s,
            'Shldr_F': 'KATA_F0_%s' % s, 'Pectoral': 'MUNE_%s_jnt' % s,
            'Arm': 'KATA_0_%s_jnt' % s, 'Arm_Top': 'KATA_0_%s_jnt' % s, 'Arm_Mid': 'KATA_50_%s' % s,
            'Arm_Btm': 'KATA_100_%s' % s, 'Bicep_Slide': 'slide_kata_%s' % lo, 'Tricep_Slide': 'slide_hiji_%s' % lo,
            'ForeArm': 'UDE_%s_jnt' % s, 'ForeArm_Slide': 'slide_ude_%s' % lo, 'ForeArm_Mid': 'UDE_50_%s' % s,
            'ForeArm_Btm': 'UDE_100_%s' % s, 'Hand': 'TE_%s_jnt' % s, 'Hand2': 'TE_%s_jnt' % s,
            'FingerBase': 'TE_%s_jnt' % s, 'Middle': 'TE_%s_jnt' % s, 'Ring': 'TE_%s_jnt' % s,
            'Pinky': 'KO_%s_BASE' % s}
        if base in table:
            return table[base]
        for ttt2, t7 in (('OYA', 'Thumb'), ('HITO', 'Index'), ('NAKA', 'Middle'), ('KUSU', 'Ring'), ('KO', 'Pinky')):
            for k in (1, 2, 3):
                if base == '%s%d' % (t7, k):
                    return '%s%d_%s' % (ttt2, 4 - k, s)
    return None


def weight_map(t7bones, ttt2names, part_of_bone=None):
    """Array t7 bone index -> TTT2 bone index (walking up T7 parents for unmapped bones such as cloth/hair OPT and
    'transformNN' bones). part_of_bone: optional function(t7 index) -> part tag ('fac', 'har', ...)."""
    idx = {_strip(n): i for i, n in enumerate(ttt2names)}
    out = np.full(len(t7bones['names']), -1, np.int64)
    via = {}
    for i, n in enumerate(t7bones['names']):
        j, k = i, None
        while j >= 0:
            k = t7_weight_target(t7bones['names'][j], part_of_bone(i) if part_of_bone else None)
            if k is not None and k in idx:      # (a bone the donor lacks, e.g. a male donor's busts: its parent's)
                break
            k = None
            j = t7bones['parents'][j]
        out[i] = idx[k] if k is not None else idx['KOSHI_jnt']
        via[i] = (t7bones['names'][j] if j >= 0 else None, k)
    return out, via


def build(t7bones, donor):
    """t7bones: {'names', 'parents', 'world' (n, 4, 4) bind matrices in TTT2 space}; donor: bone.read() of the
    donor's skeleton. Returns the new skeleton (a bone dict for bone.write)."""
    names = [_strip(n) for n in donor['names']]
    idx = {n: i for i, n in enumerate(names)}
    W = bone.world_matrices(donor)
    P = W[:, :3, 3].copy()
    t7pos = {n: t7bones['world'][i][:3, 3] for i, n in enumerate(t7bones['names'])}
    if 'BASE' not in t7pos:
        raise ValueError('the Tekken 7 skeleton has no BASE joint')
    newP = {}
    n = len(names)
    # 1. anchors
    for i, nm in enumerate(names):
        a = ANCHOR.get(nm)
        if a is not None and a in t7pos:
            newP[i] = t7pos[a].copy()
    # 2. the donor's offsets to an anchored bone
    for nm, ref in KEEP_OFFSET_TO.items():
        i, r = idx[nm], idx[ref]
        newP[i] = newP[r] + (P[i] - P[r])
    # 3. limb helpers along their segment, 4. the rest with their parent (in index order: parents first)
    seg = [(idx[a], idx[b]) for a, b in SEGMENTS]
    for i in range(n):
        if i in newP:
            continue
        best = None
        if i > 21:
            for a, b in seg:
                if a not in newP or b not in newP:
                    continue
                ab = P[b] - P[a]
                t = float((P[i] - P[a]) @ ab / (ab @ ab))
                if -0.15 <= t <= 1.15:
                    lat = P[i] - (P[a] + t * ab)
                    d = float(np.linalg.norm(lat))
                    if d < 80 and (best is None or d < best[0]):
                        best = (d, a, b, t, lat)
        if best is not None:
            d, a, b, t, lat = best
            newP[i] = newP[a] + t * (newP[b] - newP[a]) + lat
        else:
            p = donor['parent'][i]
            newP[i] = P[i] + (newP[p] - P[p]) if p >= 0 else P[i]
    # local translations under the donor's world rotations
    out = dict(donor)
    out['t'] = donor['t'].copy()
    for i in range(n):
        p = donor['parent'][i]
        out['t'][i] = newP[i] if p < 0 else W[p][:3, :3].T @ (newP[i] - newP[p])
    scale = float(t7pos['BASE'][1] / 1150.0)
    out['scale'] = scale
    s = donor['s'].copy()
    body = np.isclose(s, donor['scale'], atol=2e-3)
    s[body] = s[body] * (scale / donor['scale'])
    out['s'] = s
    out['names_raw'] = None
    return out
