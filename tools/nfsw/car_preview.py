"""Renders the object a `FRSWorldEditor --cars-selftest` run wrote (paint chunk, pad, object) from the front-left,
the side and the top, painted triangles in the paint colour: car_preview.py <file.bin> <out.png>."""
import struct, sys
import numpy as np
from PIL import Image, ImageDraw

U = lambda b, p: struct.unpack_from('<I', b, p)[0]


def walk(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from('<II', b, p)
        yield k, p, p + 8, p + 8 + n
        if k & 0x80000000: yield from walk(b, p + 8, p + 8 + n)
        p += 8 + n


def body(b, s):
    q = s
    while q < ((s + 15) & ~15) and b[q] == 0x11: q += 1
    return q


b = open(sys.argv[1], 'rb').read()
paint = U(b, 8 + 0xC); rgb = struct.unpack_from('<3f', b, 8 + 0x34 + 14 * 4)
obj = [x for x in walk(b, 0, len(b)) if x[0] == 0x80134010][0]
ch = list(walk(b, obj[2], obj[3]))
lights = [U(b, q) for k, p, s, e in ch if k == 0x134013 for q in range(s, e, 8)]
mk, mp, ms, me = [x for x in ch if x[0] == 0x134B02][0]; ms = body(b, ms)
ik, ip, iS, ie = [x for x in ch if x[0] == 0x134B03][0]; iS = body(b, iS)
bufs = [(body(b, s), e) for k, p, s, e in ch if k == 0x134B01]
idx = np.frombuffer(b[iS:ie], '<u2')
tris, cols = [], []
run, prev = -1, None
for q in range(ms, me - 115, 116):
    eff = U(b, q + 8)
    if eff != prev: run += 1; prev = eff
    s, e = bufs[run]
    stride = 32 if eff in (8, 9) else 36
    raw = b[s:s + (e - s) // stride * stride]
    if eff in (8, 9):
        P = np.frombuffer(raw, '<i2').reshape(-1, 16)[:, :3].astype(float) * 8 / 32767
    else:
        P = np.frombuffer(raw, '<f4').reshape(-1, stride // 4)[:, :3].astype(float)
    first, n = U(b, q + 0x4C), U(b, q + 0x44)
    f = idx[first:first + n].reshape(-1, 3)
    f = f[(f < len(P)).all(1)]
    painted = b[q + 0x2D] != 0xFF and lights[b[q + 0x2D]] == paint
    tris += list(P[f]); cols += [rgb if painted else (0.6, 0.6, 0.65)] * len(f)
tris = np.array(tris); cols = np.array(cols)
views = [('front-left', np.radians(-40), np.radians(20)), ('side', 0.0, 0.0), ('top', 0.0, np.radians(89))]
W = 420; img = Image.new('RGB', (W * 3, W), (30, 30, 34)); d = ImageDraw.Draw(img)
light = np.array([0.4, 0.5, 0.8]); light /= np.linalg.norm(light)
for v, (title, yaw, pitch) in enumerate(views):
    cy, sy, cp, sp = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)
    R = np.array([[cy, sy, 0], [-sy * sp, cy * sp, cp], [sy * cp, -cy * cp, sp]])   # rows: screen x, screen y, depth
    T = tris @ R.T
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0); scale = (W - 40) / max(hi[0] - lo[0], hi[1] - lo[1])
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]); n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    shade = 0.35 + 0.65 * np.abs(n @ light)
    for t in np.argsort(T[:, :, 2].mean(1)):
        pts = [(v * W + 20 + (x - lo[0]) * scale, W - 20 - (y - lo[1]) * scale) for x, y, _ in T[t]]
        c = tuple(int(255 * min(1, k * shade[t])) for k in cols[t]); d.polygon(pts, fill=c)
    d.text((v * W + 8, 6), title, fill=(220, 220, 220))
d.text((8, W - 16), '%s: %d triangles, paint %08X rgb %.2f %.2f %.2f' % (sys.argv[1].split(chr(92))[-1], len(tris), paint, *rgb), fill=(220, 220, 220))
img.save(sys.argv[2])
print(len(tris), 'triangles; box', tris.reshape(-1, 3).min(0).round(2), tris.reshape(-1, 3).max(0).round(2))
