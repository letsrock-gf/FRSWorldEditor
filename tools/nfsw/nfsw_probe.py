"""Measure NFS World track layouts. Usage: nfsw_probe.py <TRACKS dir> [max section files]"""
import os
import struct
import sys
from collections import Counter

T = sys.argv[1]
LIMIT = int(sys.argv[2]) if len(sys.argv) > 2 else 200


def walk(b, s=0, e=None, depth=0):
    e = len(b) if e is None else e
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            return
        yield k, p + 8, p + 8 + n, depth
        if k & 0x80000000:
            yield from walk(b, p + 8, p + 8 + n, depth + 1)
        p += 8 + n


def skip11(b, s, e, align=16):
    end = min(e, (s + align - 1) & ~(align - 1))
    while s < end and b[s] == 0x11:
        s += 1
    return s


def hexdump(b, n=208):
    for i in range(0, min(n, len(b)), 16):
        row = b[i:i + 16]
        print(f"    {i:03X}  {row.hex(' ')}  {''.join(chr(c) if 32 <= c < 127 else '.' for c in row)}")


# --- section table ---------------------------------------------------------
l5 = open(os.path.join(T, "L5RA.BUN"), "rb").read()
for k, s, e, _ in walk(l5):
    if k == 0x34110:
        n = (e - s) // 208
        print(f"section table: {n} entries")
        for i in (0, 1, 500, 3000):
            print(f"  entry {i}")
            hexdump(l5[s + 208 * i: s + 208 * (i + 1)])
    if k == 0x34112:
        print("bounds chunk bytes", e - s)

# --- solids ----------------------------------------------------------------
files = sorted(f for f in os.listdir(T) if f.upper().startswith("STREAML5RA_") and "_0x" not in f and f.upper().endswith(".BUN"))
stride_by_effect = {}
mat_rem = Counter()
hdr_done = False
scn = Counter()
desc_done = 0
for f in files[:LIMIT]:
    b = open(os.path.join(T, f), "rb").read()
    ch = list(walk(b))
    for i, (k, s, e, d) in enumerate(ch):
        if k == 0x134011 and not hdr_done:
            hp = skip11(b, s, e)
            print(f"object header in {f}: payload {e - hp} bytes (pad {hp - s})")
            hexdump(b[hp:e], 256)
            hdr_done = True
        if k == 0x134900 and desc_done < 2:
            hp = skip11(b, s, e)
            print(f"descriptor {e - hp} bytes"); hexdump(b[hp:e], 160); desc_done += 1
        if k == 0x80134100:
            mats, streams = [], []
            for k2, s2, e2, _ in walk(b, s, e):
                if k2 == 0x134B02:
                    q = skip11(b, s2, e2)
                    mat_rem[(e2 - q) % 116] += 1
                    mats = [b[r:r + 116] for r in range(q, e2 - 115, 116)]
                elif k2 == 0x134B01:
                    q = skip11(b, s2, e2, 128)
                    streams.append(e2 - q)
            runs = []
            for m in mats:
                eff = struct.unpack_from("<I", m, 8)[0]
                vc = struct.unpack_from("<I", m, 0x40)[0]
                if runs and runs[-1][0] == eff:
                    runs[-1][1] += vc
                else:
                    runs.append([eff, vc])
            if len(runs) == len(streams):
                for (eff, vc), nb in zip(runs, streams):
                    if vc:
                        stride_by_effect.setdefault(eff, Counter())[round(nb / vc, 2)] += 1
            else:
                stride_by_effect.setdefault("run/stream mismatch", Counter())[(len(runs), len(streams))] += 1
        if k in (0x34101, 0x34102, 0x34103, 0x34105, 0x34107, 0x3410D, 0x3410E, 0x3410F):
            scn[(k, e - s)] += 1
    # scenery header of first file
    if f == files[0]:
        for k, s, e, d in ch:
            if k in (0x34101, 0x34102, 0x34103):
                print(f"scenery {k:05X} in {f}: {e - s} bytes")
                hexdump(b[s:e], 160)

print("material chunk remainder mod 116:", dict(mat_rem))
print("stride by effect id:")
for eff, c in sorted(stride_by_effect.items(), key=lambda kv: -sum(kv[1].values())):
    name = f"{eff:08X}" if isinstance(eff, int) else eff
    print(f"  {name}: {c.most_common(4)}")
