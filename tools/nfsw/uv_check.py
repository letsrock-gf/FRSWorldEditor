"""Per effect: is the offset the editor reads UVs from the one that holds plausible texture coordinates?"""
import os, struct, sys
import numpy as np
T = sys.argv[1]
LAYOUT = {0: (36, 20), 1: (36, 20), 0xA: (36, 20), 0xC: (36, 20), 0xF: (36, 20), 2: (44, 20), 3: (44, 20), 7: (44, 20), 0xB: (44, 20), 4: (28, 20), 5: (36, 28)}
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
acc = {}
files = [f for f in sorted(os.listdir(T)) if f.upper().startswith("STREAML5RA_") and f.upper().endswith(".BUN")]
for f in files[::6]:
    b = open(os.path.join(T, f), "rb").read()
    for k, s, e in walk(b):
        if k != 0x80134100: continue
        mats, streams = [], []
        for k2, s2, e2 in walk(b, s, e):
            if k2 == 0x134B02: q = al(b, s2, e2); mats = [b[r:r + 116] for r in range(q, e2 - 115, 116)]
            elif k2 == 0x134B01: q = al(b, s2, e2); streams.append(b[q:e2])
        runs = []
        for m in mats:
            eff, = struct.unpack_from("<I", m, 8); vc, = struct.unpack_from("<I", m, 0x40)
            if runs and runs[-1][0] == eff: runs[-1][1] += vc
            else: runs.append([eff, vc])
        if len(runs) != len(streams): continue
        for (eff, vc), st in zip(runs, streams):
            if eff not in LAYOUT or not vc or len(st) != vc * LAYOUT[eff][0]: continue
            a = np.frombuffer(st, dtype="<f4").reshape(vc, LAYOUT[eff][0] // 4)
            acc.setdefault(eff, []).append(a[: min(vc, 400)])
for eff in sorted(acc):
    a = np.concatenate(acc[eff]); stride, uvo = LAYOUT[eff]
    out = []
    for off in range(12, stride - 4, 4):
        col = a[:, off // 4: off // 4 + 2]
        with np.errstate(all="ignore"):
            ok = np.isfinite(col).all(axis=1) & (np.abs(col) < 200).all(axis=1) & ((np.abs(col) > 1e-6).any(axis=1) | (col == 0).all(axis=1))
            frac = ok.mean(); spread = np.nanstd(np.where(ok[:, None], col, np.nan), axis=0).mean() if ok.any() else 0
        out.append(f"@{off}{'*' if off == uvo else ''}: {frac*100:5.1f}% plausible, spread {spread:8.3f}")
    print(f"effect {eff:X} ({len(a)} vertices)\n   " + "\n   ".join(out))
