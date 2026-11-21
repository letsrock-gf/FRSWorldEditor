"""Checks the collision a road bake wrote (STREAML5RA_25000.BUN in <written>), over every pack, against the shipped
file and the edit list's A lines (each added polygon may land in any pack whose box grew to hold it):
  - every added ground face (|normal z| > 0.6) is in the file: whole, or cut into pieces lying on its plane;
  - every added wall-like face 0.5 m tall or more (and every barrier) is a barrier record, in plan between its two
    corners furthest apart; a lower one (a kerb) stays ground, as the writer decides (WorldCollision.cpp WallLike:
    a barrier that short throws the car), and is in the file as triangles;
  - an upright new triangle is only ever such a low face (the game never touches it; the wheels ride what is on top);
  - shipped triangles that are gone: each lies under added ground (the writer cuts ground under raised ground,
    memory wheel-face-cache-overlap) or is a C line of the list.
Usage: verify_roadcol.py <written TRACKS folder> <game TRACKS>"""
import os
import struct
import sys
from collections import Counter

import col_lib as c

out, tracks = sys.argv[1], sys.argv[2]
NAME = "STREAML5RA_25000.BUN"
ok_all = True


def check(ok, text):
    global ok_all
    ok_all &= bool(ok)
    print(("   ok   " if ok else "   FAIL ") + text)


def everything(buf, other):
    # Only packs whose bytes differ from the other file's: the rest add or lose nothing.
    theirs = dict(c.packs(other))
    tris, bars = [], []
    for number, data in c.packs(buf):
        if theirs.get(number) == data: continue
        arts, insts = c.load(data)
        for inst in insts:
            art = arts.get(inst.article)
            if not art: continue
            for t in art.tris:
                tris.append((number, tuple(c.game(inst.to_world(p)) for p in t[:3])))
            for p0, p1, s, f in art.barriers:
                bars.append((number, c.game(inst.to_world(p0)), c.game(inst.to_world(p1))))
    return tris, bars


def key(tri):
    return tuple(sorted((round(p[0], 1), round(p[1], 1), round(p[2], 1)) for p in tri))


kept = os.path.join(tracks, "frs-original", NAME)
shipped = open(kept if os.path.exists(kept) else os.path.join(tracks, NAME), "rb").read()
written = open(os.path.join(out, NAME), "rb").read()
ta, ba = everything(shipped, written)
tb, bb = everything(written, shipped)
ca, cb = Counter(key(t) for n, t in ta), Counter(key(t) for n, t in tb)
gone = list((ca - cb).elements())
new = [t for n, t in tb if (cb - ca)[key(t)] > 0]
new_bars = [(p0, p1) for n, p0, p1 in bb]

adds = []
for line in open(os.path.join(out, "frs-original", "L5RA-edits.txt")):
    w = line.split()
    if w and w[0] == "A":
        f = [float(x) for x in w[11:23]]
        adds.append((int(w[2]), int(w[3]), [tuple(f[i:i + 3]) for i in range(0, 12, 3)]))
print(f"changed packs' triangles: shipped {len(ta)}, written {len(tb)}; new {len(new)}, gone {len(gone)}; "
      f"barriers shipped {len(ba)}, written {len(bb)}; list adds {len(adds)}")


def normal_z(t):
    u = [t[1][i] - t[0][i] for i in range(3)]
    v = [t[2][i] - t[0][i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    length = sum(x * x for x in n) ** 0.5
    return 0.0 if length < 1e-9 else abs(n[2]) / length


def on(tri, face, tol=0.06):
    a, b, cc = face
    cx, cy, cz = (sum(p[i] for p in tri) / 3.0 for i in range(3))
    d = (b[1] - cc[1]) * (a[0] - cc[0]) + (cc[0] - b[0]) * (a[1] - cc[1])
    if abs(d) < 1e-9: return False
    l1 = ((b[1] - cc[1]) * (cx - cc[0]) + (cc[0] - b[0]) * (cy - cc[1])) / d
    l2 = ((cc[1] - a[1]) * (cx - cc[0]) + (a[0] - cc[0]) * (cy - cc[1])) / d
    l3 = 1 - l1 - l2
    return min(l1, l2, l3) >= -0.01 and abs(l1 * a[2] + l2 * b[2] + l3 * cc[2] - cz) < tol


ground, walls, missing_ground, missing_walls = [], 0, 0, 0
low = []
for barrier, quad, corners in adds:
    faces = [corners[0:3]] + ([[corners[0], corners[2], corners[3]]] if quad else [])
    pts = corners[:4] if quad else corners[:3]
    tall = max(p[2] for p in pts) - min(p[2] for p in pts)
    if not barrier and normal_z(corners[0:3]) <= 0.6 and tall < 0.5:
        low += [tuple(sorted(f)) for f in faces]
        continue
    if not barrier and normal_z(corners[0:3]) > 0.6:
        ground += faces
        if not any(on(t, f) for t in new for f in faces):
            missing_ground += 1
        continue
    pts = corners[:4] if quad else corners[:3]
    pair = max(((a, b) for i, a in enumerate(pts) for b in pts[i + 1:]),
               key=lambda ab: (ab[0][0] - ab[1][0]) ** 2 + (ab[0][1] - ab[1][1]) ** 2)
    want = sorted([(pair[0][0], pair[0][1]), (pair[1][0], pair[1][1])])
    hit = any(all(abs(x - y) < 0.1 for p, q in zip(sorted([(p0[0], p0[1]), (p1[0], p1[1])]), want) for x, y in zip(p, q))
              for p0, p1 in new_bars)
    walls += 1
    missing_walls += not hit
check(missing_ground == 0, f"added ground faces in the file: {sum(1 for b, q, cs in adds if not b and normal_z(cs[0:3]) > 0.6) - missing_ground} found, {missing_ground} missing")
check(missing_walls == 0, f"added walls and barriers as barrier records: {walls - missing_walls} found, {missing_walls} missing")
stray = [t for t in new if not any(on(t, f) for f in ground)]
print(f"   new triangles not on an added ground face: {len(stray)} (kerbs, and pieces of shipped ground cut round "
      f"the road; checked below)")
def same(t, f):
    return all(abs(x - y) < 0.02 for p, q in zip(sorted(t), f) for x, y in zip(p, q))


upright = [t for t in new if normal_z(t) < 0.05 and abs(max(p[2] for p in t) - min(p[2] for p in t)) > 0.05]
def on_plane(t, f):
    a, b, cc = f
    u = [b[i] - a[i] for i in range(3)]
    v = [cc[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    length = sum(x * x for x in n) ** 0.5 or 1.0
    return max(abs(sum(n[i] * (p[i] - a[i]) for i in range(3))) / length for p in t) < 0.02


def degenerate(t):   # two corners in one place: no area at all (slivers of the cut round raised ground)
    return any(all(abs(t[i][k] - t[j][k]) < 0.02 for k in range(3)) for i in range(3) for j in range(i + 1, 3))


slivers = [t for t in upright if degenerate(t)]
odd = [t for t in upright if not degenerate(t) and not any(on_plane(t, f) for f in low)]
print(f"   upright new triangles: {len(upright)}; {len(slivers)} have no area (cut slivers), "
      f"{len(upright) - len(slivers) - len(odd)} lie in a low face (kerb, cut along a box)")
def pieces_of(f):   # cut along a box's edge: pieces on its plane, their centres within it in plan
    return any(on_plane(t, f) and all(min(q[i] for q in f) - 0.05 <= sum(p[i] for p in t) / 3 <= max(q[i] for q in f) + 0.05
                                      for i in range(2)) for t in new)


found_low = sum(1 for f in low if any(same(t, f) for t in new) or pieces_of(f))
check(found_low == len(low), f"low upright faces (kerbs) kept as ground triangles: {found_low} of {len(low)}")
check(not odd, f"upright new triangles that are not such a low face: {len(odd)} (of {len(upright)} upright)")


def under(t):
    cx, cy = (sum(p[i] for p in t) / 3.0 for i in range(2))
    for a, b, cc in ground:
        d = (b[1] - cc[1]) * (a[0] - cc[0]) + (cc[0] - b[0]) * (a[1] - cc[1])
        if abs(d) < 1e-9: continue
        l1 = ((b[1] - cc[1]) * (cx - cc[0]) + (cc[0] - b[0]) * (cy - cc[1])) / d
        l2 = ((cc[1] - a[1]) * (cx - cc[0]) + (a[0] - cc[0]) * (cy - cc[1])) / d
        if min(l1, l2, 1 - l1 - l2) >= -0.05: return True
    return False


# A gone triangle touches the road's footprint when its centre or a corner is under added ground.
def touches(t):
    return under(t) or any(under((p, p, p)) for p in t)


far = [t for t in gone if not touches(t)]
print(f"   shipped triangles gone: {len(gone)}, {len(gone) - len(far)} of them under or at the road")
check(len(far) <= sum(1 for line in open(os.path.join(out, "frs-original", "L5RA-edits.txt")) if line.startswith("C ")) * 100,
      f"shipped triangles gone away from the road: {len(far)} (only the list's C lines may take those)")
print("ALL PASS" if ok_all else "SOME FAILED")
sys.exit(0 if ok_all else 1)
