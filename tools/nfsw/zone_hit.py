"""Which section boundary polygons of L5RA.BUN chunk 0x34152 hold given points (game x y), and how far a point is
from the nearest one. Record: u32 11, u32 11, u16 section, u8 point count, u8 flag, 12 bytes, min xy, max xy,
centre xy, then the points (float x, y each; game axes, measured: extents match the map).
Usage: zone_hit.py x,y [x,y ...]"""
import os, struct, sys, math
from game_folder import TRACKS

p = os.path.join(TRACKS, "frs-original", "L5RA.BUN")
b = open(p if os.path.exists(p) else os.path.join(TRACKS, "L5RA.BUN"), "rb").read()


def walk(s, e):
    q = s
    while q + 8 <= e:
        k, n = struct.unpack_from("<II", b, q)
        yield k, q + 8, q + 8 + n
        if k & 0x80000000: yield from walk(q + 8, q + 8 + n)
        q += 8 + n


zones = []
for k, s, e in walk(0, len(b)):
    if k != 0x34152: continue
    q = s
    while q + 48 <= e:
        m1, m2, sec, n = struct.unpack_from("<IIHB", b, q)
        if m1 != 11: break
        pts = [struct.unpack_from("<2f", b, q + 48 + 8 * i) for i in range(n)]
        zones.append((sec, pts))
        q += 48 + 8 * n


def inside(pt, poly):
    x, y, c = pt[0], pt[1], False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[i - 1]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1: c = not c
    return c


def dist(pt, poly):
    best = 1e30
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[i - 1]
        dx, dy = x2 - x1, y2 - y1
        t = max(0, min(1, ((pt[0] - x1) * dx + (pt[1] - y1) * dy) / (dx * dx + dy * dy or 1)))
        best = min(best, math.hypot(pt[0] - x1 - t * dx, pt[1] - y1 - t * dy))
    return best


xs = [p[0] for _, pts in zones for p in pts]; ys = [p[1] for _, pts in zones for p in pts]
print(f"{len(zones)} zones, x {min(xs):.0f}..{max(xs):.0f}, y {min(ys):.0f}..{max(ys):.0f}")
for arg in sys.argv[1:]:
    pt = tuple(float(v) for v in arg.split(","))
    hold = [sec for sec, pts in zones if inside(pt, pts)]
    near = min((dist(pt, pts), sec) for sec, pts in zones)
    print(f"{arg}: inside {hold or 'none'}; nearest outline {near[1]} at {near[0]:.1f} m")
