import os, struct, sys, re
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
combo = Counter(); ex = {}; fmts = Counter(); shown = 0; bad = 0
files = [f for f in sorted(os.listdir(T)) if "_0x" in f][:1500]
for f in files:
    b = open(os.path.join(T, f), "rb").read()
    hdr = fmt = None
    for k, s, e in walk(b):
        if k == 0x33310004: hdr = (s, e)
        if k == 0x33310005: fmt = (s, e)
        if k == 0xB3320000 and hdr and fmt:
            p, i = hdr[0], 0
            while p + 0x91 <= hdr[1]:
                ln = b[p + 0x90]; name = b[p + 0x91:p + 0x91 + ln].split(b"\0")[0].decode("latin1")
                cc = b[fmt[0] + 32 * i + 12: fmt[0] + 32 * i + 16]
                fmts[cc] += 1
                w, h = struct.unpack_from("<II", b, p + 0x20)
                if not (0 < w <= 4096 and 0 < h <= 4096): bad += 1
                key = (b[p + 0x30:p + 0x34].hex(), b[p + 0x38:p + 0x4C].hex())
                combo[key] += 1; ex.setdefault(key, []).append(name)
                p += 0x91 + ln; i += 1
            hdr = fmt = None
print("formats", fmts, "bad dims", bad)
for key, n in combo.most_common(40):
    print(n, key[0], key[1], ex[key][:5])
