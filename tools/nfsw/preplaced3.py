import os, struct, sys, math
from collections import Counter
T = sys.argv[1]
def walk(b, s=0, e=None):
    e = len(b) if e is None else e; p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000: yield from walk(b, p + 8, p + 8 + n)
        p += 8 + n
def al(b, s, e, a=16):
    end = min(e, (s + a - 1) & ~(a - 1)); q = s
    while q < end and b[q] == 0x11: q += 1
    return q if q == end else s
models = {}; data = {}
for f in sorted(os.listdir(T)):
    if not (f.upper().startswith("STREAML5RA_") and f.upper().endswith(".BUN")): continue
    b = open(os.path.join(T, f), "rb").read()
    for k, s, e in walk(b):
        if k == 0x134011:
            hp = al(b, s, e); h, = struct.unpack_from("<I", b, hp + 0x10)
            models.setdefault(h, (struct.unpack_from("<3f", b, hp + 0x20), struct.unpack_from("<3f", b, hp + 0x30), b[hp + 0xA0:e].split(b"\0")[0].decode("latin1")))
    data[f] = b
stat = Counter(); err = {"pre as-is": [], "pre recentred": [], "local as-is": []}; perm = Counter(); shown = 0
for f, b in data.items():
    inst = infos = None
    for k, s, e in walk(b):
        if k == 0x34101: inst = infos = None
        elif k == 0x34103: inst = (al(b, s, e), e)
        elif k == 0x34102: q = al(b, s, e); infos = [struct.unpack_from("<I", b, r + 0x18)[0] for r in range(q, e - 71, 72)]
        elif k == 0x3410F and inst and infos:
            q = al(b, s, e); iq, ie = inst; n = (ie - iq) // 96
            rows = [struct.unpack_from("<3I", b, q + 32 * i + 16) for i in range(min(n, (e - q) // 32))]
            perm["rows are a permutation" if sorted(r[0] for r in rows) == list(range(n)) else "NOT permutation"] += 1
            perm["f0==f2" if all(r[0] == r[2] for r in rows) else "f0!=f2"] += 1
            for ii, fi, _ in rows:
                if ii >= n or fi >= len(infos) or infos[fi] not in models: stat["unresolved"] += 1; continue
                r = iq + 96 * ii
                imn = struct.unpack_from("<3f", b, r); imx = struct.unpack_from("<3f", b, r + 0x10)
                t = struct.unpack_from("<3f", b, r + 0x20); m = struct.unpack_from("<9f", b, r + 0x2C)
                mn, mx, name = models[infos[fi]]
                c = [(a + z) / 2 for a, z in zip(mn, mx)]; pre = math.hypot(*c) > math.dist(mn, mx)
                ic = [(a + z) / 2 for a, z in zip(imn, imx)]
                def xf(p): return [p[0] * m[0 + i] + p[1] * m[3 + i] + p[2] * m[6 + i] + t[i] for i in range(3)]
                tz = abs(t[0]) + abs(t[1]) + abs(t[2]) < 1e-3
                ident = all(abs(m[i] - (1.0 if i % 4 == 0 else 0.0)) < 1e-3 for i in range(9))
                stat[("pre-placed" if pre else "local", "t=0" if tz else "t!=0", "ident" if ident else "rot")] += 1
                if pre:
                    err["pre as-is"].append(math.dist(xf(c), ic)); err["pre recentred"].append(math.dist(xf([0, 0, 0]), ic))
                    if shown < 3: shown += 1; print(name, "t", [round(x, 1) for x in t], "model c", [round(x) for x in c], "inst c", [round(x) for x in ic])
                else: err["local as-is"].append(math.dist(xf(c), ic))
            inst = infos = None
print(dict(perm)); print(dict(stat))
for k, v in err.items():
    v.sort(); print(k, "n", len(v), "median centre error", round(v[len(v) // 2], 2), "p90", round(v[int(len(v) * .9)], 2), "p99", round(v[int(len(v) * .99)], 2))
