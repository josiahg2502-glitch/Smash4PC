"""Triangle strips for TTT2 models: greedy strips joined by 0xFFFF restarts.

In a strip s, triangle k is (s[k], s[k+1], s[k+2]) for even k and (s[k+1], s[k], s[k+2]) for odd k (as the game and
nud.strip_to_tris read them), so every input triangle keeps its orientation.
"""
from ..ttt2.nud import strip_to_tris


def stripify(tris):
    """tris: (m, 3) int array (counter-clockwise). Returns a list of indices with 0xFFFF between strips."""
    tris = [tuple(int(x) for x in t) for t in tris if len({int(x) for x in t}) == 3]
    edge = {}                                  # directed edge (a, b) -> list of triangle ids having a->b
    for i, (a, b, c) in enumerate(tris):
        for e in ((a, b), (b, c), (c, a)):
            edge.setdefault(e, []).append(i)
    used = [False] * len(tris)

    def rotations(t):
        a, b, c = t
        return ((a, b, c), (b, c, a), (c, a, b))

    def take(e):
        for i in edge.get(e, ()):
            if not used[i]:
                return i
        return None

    def third(i, a, b):
        t = tris[i]
        return next(x for x in t if x != a and x != b)

    def grow(strip):
        """Extend strip (already holding its first triangle) as far as possible."""
        while True:
            k = len(strip) - 2                      # index of the next triangle in the strip
            a, b = strip[-2], strip[-1]
            # next triangle is (a, b, c) for even k, (b, a, c) for odd k -> it must contain directed edge a->b
            # (even) or b->a (odd)
            e = (a, b) if k % 2 == 0 else (b, a)
            i = take(e)
            if i is None:
                return strip
            used[i] = True
            strip.append(third(i, a, b))

    # neighbour count for a cheap "start at the border" heuristic
    def free_neighbours(i):
        a, b, c = tris[i]
        n = 0
        for x, y in ((a, b), (b, c), (c, a)):
            for j in edge.get((y, x), ()):
                if not used[j]:
                    n += 1
        return n

    order = sorted(range(len(tris)), key=free_neighbours)
    out = []
    for i in order:
        if used[i]:
            continue
        used[i] = True
        best = None
        for rot in rotations(tris[i]):
            # try each rotation on a copy of the used flags and keep the longest strip
            saved = list(used)
            s = grow(list(rot))
            if best is None or len(s) > len(best[0]):
                best = (s, list(used))
            used[:] = saved
        s, state = best
        used[:] = state
        if out:
            out.append(0xFFFF)
        out.extend(s)
    return out


def check(tris, strip):
    """True if the strip decodes to exactly the input triangles with the same orientation."""
    canon = lambda t: min((t[0], t[1], t[2]), (t[1], t[2], t[0]), (t[2], t[0], t[1]))
    a = sorted(canon(tuple(int(x) for x in t)) for t in tris if len(set(int(x) for x in t)) == 3)
    b = sorted(canon(t) for t in strip_to_tris(strip))
    return a == b
