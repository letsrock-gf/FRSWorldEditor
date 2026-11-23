"""Checks the streaming zones a road bake widened (L5RA.BUN chunk 0x34152 in 0x80034150; nfsw.exe 0x7471f0 loads it,
0x741ba0 looks the car up in it) in a written TRACKS folder, against the shipped file:
  - the top-level chunk chain ends on the file's last byte; 0x80034150 holds its children exactly; every top-level
    chunk that was 16-aligned still is;
  - L5RA.HOT (16-byte rows: id, size, offset, hash) names every top-level chunk where it is, with its size;
  - every 0x34152 record the list's Z lines do not name is byte for byte the shipped one; a widened one has the Z
    points in order (plus at most one copy of the last), at most 127 of them, min/max/centre of those points, and the
    rest of its header as shipped; its outline does not cross itself;
  - every corner of the collision the list adds (A lines the editor made) is inside a drivable zone (even-odd, as
    the game), or within 0.1 m of one.
Usage: verify_zones.py <written TRACKS folder> <game TRACKS>"""
import math
import os
import struct
import sys

out, tracks = sys.argv[1], sys.argv[2]
ok_all = True


def check(ok, text):
    global ok_all
    ok_all &= bool(ok)
    print(("   ok   " if ok else "   FAIL ") + text)


def shipped(name):
    p = os.path.join(tracks, "frs-original", name)
    return p if os.path.exists(p) else os.path.join(tracks, name)


def top(b):
    rows, p = [], 0
    while p + 8 <= len(b):
        k, n = struct.unpack_from("<II", b, p)
        rows.append((k, p, n))
        p += 8 + n
    return rows, p


def zones(b):
    rows, _ = top(b)
    k, p, n = next(r for r in rows if r[0] == 0x80034150)
    q, kids = p + 8, []
    while q + 8 <= p + 8 + n:
        kk, nn = struct.unpack_from("<II", b, q)
        kids.append((kk, q, nn))
        q += 8 + nn
    base = next(struct.unpack_from("<I", b, q2 + 8)[0] for kk, q2, nn in kids if kk == 0x34151)
    kk, q2, nn = next(c for c in kids if c[0] == 0x34152)
    recs, q = [], q2 + 8
    while q + 48 <= q2 + 8 + nn:
        sec, cnt = struct.unpack_from("<hB", b, q + 8)
        recs.append((sec, cnt, b[q:q + 48 + 8 * cnt]))
        q += 48 + 8 * cnt
    return kids, q == q2 + 8 + nn, base, recs, q - (p + 8 + n)


live = open(os.path.join(out, "L5RA.BUN"), "rb").read()
ship = open(shipped("L5RA.BUN"), "rb").read()
rows, end = top(live)
check(end == len(live), "top-level chunks end on the file's last byte")
srows, _ = top(ship)
same_ids = [r[0] for r in rows] == [r[0] for r in srows]
check(same_ids and all(p % 16 == 0 for (k, p, n), (ks, ps, ns) in zip(rows, srows) if ps % 16 == 0 and k != 0),
      "same top-level chunks in the same order; the 16-aligned ones still are")
kids, exact, base, recs, overrun = zones(live)
check(exact and overrun <= 0, "0x34152 is whole records; 0x80034150 holds its children")
hotp = os.path.join(out, "L5RA.HOT")
if os.path.exists(hotp):
    hot = open(hotp, "rb").read()
    at = {p: n for k, p, n in rows}
    bad = [r for r in range(16, len(hot), 16)
           if struct.unpack_from("<I", hot, r + 8)[0] not in at or
           at[struct.unpack_from("<I", hot, r + 8)[0]] != struct.unpack_from("<I", hot, r + 4)[0]]
    check(not bad, f"L5RA.HOT: {len(hot) // 16 - 1} rows, each at a chunk of that size ({len(bad)} not)")
else:
    print("   (no L5RA.HOT written)")

zlines = {}
manual = {}   # ZM: widened by hand; a Z outline of the same zone (the bake's) replaces it
alines = []
for line in open(os.path.join(out, "frs-original", "L5RA-edits.txt")):
    w = line.split()
    if w and w[0] == "Z":
        zlines.setdefault(int(w[1]), []).append((float(w[2]), float(w[3])))
    elif w and w[0] == "ZM":
        manual.setdefault(int(w[1]), []).append((float(w[2]), float(w[3])))
    elif w and w[0] == "A" and w[5] == "-1":
        f = [float(x) for x in w[11:23]]
        alines.append([(f[i], f[i + 1]) for i in range(0, 12, 3)])
for sec, pts in manual.items():
    zlines.setdefault(sec, pts)
_, _, _, srecs, _ = zones(ship)
sby = {sec: raw for sec, cnt, raw in srecs}
same = sum(1 for sec, cnt, raw in recs if sec not in zlines and sby.get(sec) == raw)
check(same == sum(1 for sec, cnt, raw in recs if sec not in zlines), f"{same} zones not widened are as shipped")
check(len(recs) == len(srecs), f"as many zone records as shipped ({len(recs)})")


def crosses(poly):
    n, hits = len(poly), 0
    def inter(a, b, c, d):
        def o(p, q, r): return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
        return o(a, b, c) * o(a, b, d) < 0 and o(c, d, a) * o(c, d, b) < 0
    for i in range(n):
        for j in range(i + 2, n):
            if i == 0 and j == n - 1: continue
            if inter(poly[i], poly[(i + 1) % n], poly[j], poly[(j + 1) % n]): hits += 1
    return hits


polys = {}
for sec, cnt, raw in recs:
    pts = [struct.unpack_from("<2f", raw, 48 + 8 * k) for k in range(cnt)]
    polys[sec] = pts
    if sec not in zlines: continue
    want = [(round(x, 2), round(y, 2)) for x, y in zlines[sec]]
    got = [(round(x, 2), round(y, 2)) for x, y in pts]
    check(got[:len(want)] == want and len(got) - len(want) in (0, 1) and (len(got) == len(want) or got[-1] == want[-1]),
          f"zone {sec}: the list's {len(want)} points, {len(got)} written")
    check(cnt <= 127, f"zone {sec}: {cnt} points, the game reads up to 127")
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    box = struct.unpack_from("<6f", raw, 24)
    check(all(abs(a - b) < 0.01 for a, b in zip(box, (min(xs), min(ys), max(xs), max(ys),
                                                       (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2))),
          f"zone {sec}: min, max and centre are the outline's")
    check(raw[:8] == sby[sec][:8] and raw[12:24] == sby[sec][12:24], f"zone {sec}: rest of the header as shipped")
    check(crosses(pts) == 0, f"zone {sec}: the outline does not cross itself ({crosses(pts)} crossings)")


def inside(pt, poly):
    c = False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[i - 1]
        if (y1 > pt[1]) != (y2 > pt[1]) and pt[0] < (x2 - x1) * (pt[1] - y1) / (y2 - y1) + x1: c = not c
    return c


def near(pt, poly):
    best = 1e30
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[i - 1]
        dx, dy = x2 - x1, y2 - y1
        t = max(0, min(1, ((pt[0] - x1) * dx + (pt[1] - y1) * dy) / (dx * dx + dy * dy or 1)))
        best = min(best, math.hypot(pt[0] - x1 - t * dx, pt[1] - y1 - t * dy))
    return best


drivable = {s: p for s, p in polys.items() if s != -1 and s // 1000 <= 0x13 and 1 <= s % 1000 < base}
corners = [c for poly in alines for c in poly]
outside = [c for c in corners if not any(inside(c, p) or near(c, p) <= 0.1 for p in drivable.values())]
check(not outside, f"collision the list adds: {len(corners) - len(outside)} of {len(corners)} corners in a drivable zone"
      + (f" (first outside: {outside[0][0]:.1f}, {outside[0][1]:.1f})" if outside else ""))
print("ALL PASS" if ok_all else "SOME FAILED")
sys.exit(0 if ok_all else 1)
