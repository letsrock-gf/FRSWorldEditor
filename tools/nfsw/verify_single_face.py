"""Under every added ground polygon of the edit list, one ground height per point.

A wheel keeps the face it found while its point stays inside it in plan
(nfsw.exe 0x6dcbc0), so road left under a bump makes the height jump. For
points inside each added ground polygon (centroid and corners pulled in 10 %),
the faces of the BUN that hold the point in plan within 1.2 m of it: PASS when
none sees two heights more than 2 cm apart and none sees no face.
Usage: verify_single_face.py <edit list> <STREAML5RA_25000.BUN>
"""
import math, sys
import col_lib as L
import os
LIMIT = int(os.environ.get("LIMIT", "5"))

def tris_of(line):
    w = line.split(); f = [float(x) for x in w[11:]]
    v = [tuple(f[i:i + 3]) for i in range(0, 12, 3)]
    return w, (v if int(w[3]) else v[:3])

def height(t, x, y):
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(d) < 1e-12: return None
    l1 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d
    l2 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d
    l3 = 1 - l1 - l2
    if min(l1, l2, l3) < -1e-5: return None
    return l1 * az + l2 * bz + l3 * cz

def main(edits, bun):
    points = []
    for line in open(edits):
        if not line.startswith("A "): continue
        w, c = tris_of(line)
        if int(w[2]): continue
        lo, hi = min(p[2] for p in c), max(p[2] for p in c)
        if hi - lo >= 0.5: continue          # walls are barriers
        cx = sum(p[0] for p in c) / len(c); cy = sum(p[1] for p in c) / len(c); cz = sum(p[2] for p in c) / len(c)
        points.append((cx, cy, cz))
        for p in c: points.append((p[0] * .9 + cx * .1, p[1] * .9 + cy * .1, p[2] * .9 + cz * .1))
    cell = {}
    for n, d in L.packs(open(bun, "rb").read()):
        arts, insts = L.load(d)
        for ins in insts:
            a = arts.get(ins.article)
            if not a: continue
            for t in a.tris:
                g = [L.game(ins.to_world(p)) for p in t[:3]]
                x0, x1 = min(p[0] for p in g), max(p[0] for p in g)
                y0, y1 = min(p[1] for p in g), max(p[1] for p in g)
                if x1 - x0 > 400 or y1 - y0 > 400: continue
                for i in range(int(x0 // 8), int(x1 // 8) + 1):
                    for j in range(int(y0 // 8), int(y1 // 8) + 1):
                        cell.setdefault((i, j), []).append(g)
    two = none = 0
    for x, y, z in points:
        hs = [h for t in cell.get((int(x // 8), int(y // 8)), []) for h in [height(t, x, y)] if h is not None and abs(h - z) < 1.2]
        if not hs:
            none += 1
            if none <= LIMIT: print("  no face at (%.2f %.2f %.2f)" % (x, y, z))
        elif max(hs) - min(hs) > 0.02:
            two += 1
            if two <= LIMIT: print("  heights %s at (%.2f %.2f)" % (sorted(set(round(h, 2) for h in hs)), x, y))
    print("%d points: %d see two heights, %d see no face -> %s" % (len(points), two, none, "PASS" if not two and not none else "FAIL"))

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
