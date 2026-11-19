"""Full scan of NFSW solids: effect -> stride, index base rule, vertex totals, header versions."""
import os
import struct
import sys
from collections import Counter

T = sys.argv[1]


def walk(b, s=0, e=None):
    e = len(b) if e is None else e
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000:
            yield from walk(b, p + 8, p + 8 + n)
        p += 8 + n


def skip11(b, s, e, align):
    end = min(e, (s + align - 1) & ~(align - 1))
    q = s
    while q < end and b[q] == 0x11:
        q += 1
    return q if q == end else s   # filler only when it is all 0x11


stride = {}
idxrule = Counter()
total_v = Counter()
ver = Counter()
mism = Counter()
slot = Counter()
sample = {}
texref_rem = Counter()
objs = 0
for f in sorted(os.listdir(T)):
    if not (f.upper().startswith("STREAML5RA_") and f.upper().endswith(".BUN")):
        continue
    b = open(os.path.join(T, f), "rb").read()
    ch = list(walk(b))
    for k, s, e in ch:
        if k == 0x134011:
            hp = skip11(b, s, e, 16)
            ver[struct.unpack_from("<I", b, hp + 0x0C)[0]] += 1
        elif k == 0x134012:
            texref_rem[(e - skip11(b, s, e, 8)) % 8] += 1
        elif k == 0x80134100:
            objs += 1
            mats, streams, idx = [], [], b""
            for k2, s2, e2 in walk(b, s, e):
                if k2 == 0x134B02:
                    q = skip11(b, s2, e2, 16)
                    mats = [b[r:r + 116] for r in range(q, e2 - 115, 116)]
                elif k2 == 0x134B01:
                    q = skip11(b, s2, e2, 16)
                    streams.append(b[q:e2])
                elif k2 == 0x134B03:
                    q = skip11(b, s2, e2, 16)
                    idx = b[q:e2]
            runs = []
            for m in mats:
                eff, = struct.unpack_from("<I", m, 8)
                vc, ic, tc, fi = struct.unpack_from("<IIII", m, 0x40)
                slot[m[0x28]] += 0
                if runs and runs[-1][0] == eff:
                    runs[-1][1].append((vc, ic, fi))
                else:
                    runs.append((eff, [(vc, ic, fi)]))
            if len(runs) != len(streams):
                mism[(len(runs), len(streams))] += 1
                continue
            tv = 0
            for (eff, ms), st in zip(runs, streams):
                vc = sum(m[0] for m in ms)
                tv += vc
                if not vc:
                    continue
                stride.setdefault(eff, Counter())[round(len(st) / vc, 3)] += 1
                sample.setdefault(eff, f)
                base = 0
                for mvc, ic, fi in ms:
                    if ic and 2 * (fi + ic) <= len(idx):
                        mx = max(struct.unpack_from(f"<{ic}H", idx, 2 * fi))
                        if len(ms) > 1 and base:
                            if mx < mvc:
                                idxrule["relative to material"] += 1
                            elif base <= mx < base + mvc:
                                idxrule["relative to stream"] += 1
                            else:
                                idxrule["other"] += 1
                    base += mvc
            total_v["<=65535" if tv <= 65535 else ">65535"] += 1

print("objects", objs)
print("header version", dict(ver))
print("texref chunk size mod 8", dict(texref_rem))
print("run/stream mismatches", dict(mism))
print("index rule", dict(idxrule))
print("total vertices per object", dict(total_v))
for eff, c in sorted(stride.items(), key=lambda kv: -sum(kv[1].values())):
    print(f"  effect {eff:2X}: {c.most_common(3)}  e.g. {sample[eff]}")
