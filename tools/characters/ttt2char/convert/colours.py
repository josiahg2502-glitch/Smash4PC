"""Player 2's costume colours: TTT2's way of colouring a costume, fitted to a Tekken 7 fighter's 1P and 2P colours.

TTT2 gives player 2 the same model as player 1; "colourable" cloth materials (flags 0x244x0099) are tinted per
player: colour = 2 * shade * lerp(A, B, mask.b), shade from the grey of texture slot 0 (its colour is ignored), mask
in slot 3 (r = 1), A and B the colours of two palette numbers: NU_vReg_04 = (r, g, b, 500 + NN_A), NU_vReg_07 =
(r, g, b, 500 + NN_B). Palettes: XML entries `<Color id="SELECT_ID" palette="NN" x="X" y="Y" s="S" />`, player 1's
in data003[14] (key 9cdcd24f), player 2's in data003[13] (key a6e0a5e2), (x, y) a cell of the colour grid
data003[10] (32 rows x 64 columns x 3 colours x RGB f32), s a saturation step (9: the cell's colour). The fighter's
colours go into free cells of a copy of the grid (the pack replaces the grid: other fighters' cells are unchanged).

A Tekken 7 costume changes some texels between 1P and 2P and keeps the others (red trims stay red). The added
fighter keeps its 1P materials as they are and gets, over each cloth polygon that changes, a second polygon on the
same vertices (pushed out by a hair) with a colourable material that is alpha-tested to the changing texels: its
texels are split into two groups (k-means over their 1P and 2P colours), each group a palette number whose 1P and 2P
colours are the group's, and each texel's shade the brightness that best gives both.
"""
import re

import numpy as np

KEY_1P, KEY_2P, KEY_GRID = 0x9CDCD24F, 0xA6E0A5E2, 0x05ADD6D5
CHANGED = 20 / 255.0               # a texel changes when a channel differs by more than this between 1P and 2P
FIRST_PALETTE = 51                 # the palette numbers the added fighter's colourable materials use, from here on
ID_BASE = 500                      # NU_vReg_04.w / 07.w = ID_BASE + palette number (as the game's own costumes)


class Palette:
    """The colour grid with the added fighter's colours placed in cells no palette entry of the game uses: each in a
    cell whose three colours are all that colour, used with s = 9 (the cell's colour itself). Cells the game's own
    entries use are never changed (some of them render differently from what their numbers suggest)."""

    def __init__(self, grid_bytes, palette_texts):
        self.grid = np.frombuffer(grid_bytes, '>f4').reshape(32, 64, 3, 3).copy()
        used = set()
        for text in palette_texts:
            used |= {(int(x), int(y)) for x, y in re.findall(r'x="(\d+)" y="(\d+)"', text)}
        self.free = [(x, y) for y in range(32, 0, -1) for x in range(64, 0, -1) if (x, y) not in used]

    def place(self, rgb):
        """A palette entry (x, y, s) giving exactly rgb."""
        if not self.free:
            raise ValueError('no free cell left in the colour grid')
        x, y = self.free.pop(0)
        self.grid[y - 1, x - 1, :, :] = np.clip(np.asarray(rgb, np.float64), 0, 1)
        return (x, y, 9)

    def grid_bytes(self):
        return self.grid.astype('>f4').tobytes()


def _kmeans2(x, iters=20):
    """Two groups of rows of x (n, 6) -> labels."""
    lum = x[:, 3:].sum(1)
    low, high = x[lum <= np.median(lum)], x[lum > np.median(lum)]
    c = np.stack([low.mean(0), high.mean(0) if len(high) else low.mean(0) + 1e-3])
    lab = np.zeros(len(x), np.int64)
    for _ in range(iters):
        lab = ((x[:, None, :] - c[None]) ** 2).sum(-1).argmin(1)
        for k in range(2):
            if (lab == k).any():
                c[k] = x[lab == k].mean(0)
    return lab


def fit(c1, c2, palette):
    """1P and 2P colours (H, W, 3) in 0..1 (the texture's own encoding) -> None when nothing changes, else
    dict(alpha (H, W) 0/1, shade (H, W), b (H, W) 0/1, groups [((x, y, s) 1P, rgb 1P, (x, y, s) 2P, rgb 2P)] x 2)."""
    changed = np.abs(c1 - c2).max(-1) > CHANGED
    if changed.mean() < 0.002:
        return None
    x = np.concatenate([c1[changed], c2[changed]], 1)
    lab = _kmeans2(x) if len(x) > 16 else np.zeros(len(x), np.int64)
    groups, shade = [], np.zeros(changed.sum())
    for k in range(2):
        m = lab == k
        if not m.any():
            groups.append(groups[0])
            continue
        # the group's colours at shade 0.5 (colour = 2 * shade * palette colour): its mean colours
        q1, q2 = np.clip(x[m, :3].mean(0), 0, 1), np.clip(x[m, 3:].mean(0), 0, 1)
        groups.append((palette.place(q1), q1, palette.place(q2), q2))
        # each texel's shade: least squares over both players' colours
        num = (x[m, :3] * q1).sum(1) + (x[m, 3:] * q2).sum(1)
        den = 2.0 * ((q1 * q1).sum() + (q2 * q2).sum())
        shade[m] = np.clip(num / max(den, 1e-6), 0, 1)
    out_shade = np.zeros(changed.shape)
    out_shade[changed] = shade
    b = np.zeros(changed.shape)
    b[changed] = lab
    return dict(alpha=changed.astype(np.float64), shade=out_shade, b=b, groups=groups)


def palette_xml(text, select_id, entries):
    """A palette XML with entries {palette number: (x, y, s)} for select_id (replacing any it has)."""
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = [l for l in text.split(nl)
             if not re.search(r'<Color id="%d" palette="(%s)"' % (select_id, '|'.join(map(str, entries))), l)]
    end = max(i for i, l in enumerate(lines) if '</Colors>' in l)
    new = ['\t<Color id="%d" palette="%d" x="%d" y="%d" s="%d" />' % ((select_id, nn) + tuple(v))
           for nn, v in sorted(entries.items())]
    return nl.join(lines[:end] + new + lines[end:])
