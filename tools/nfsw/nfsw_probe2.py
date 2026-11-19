"""NFSW probe 2: table->file mapping, material + vertex samples per effect, texture pack tree."""
import os
import struct
import sys
from collections import Counter

T = sys.argv[1]


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


def hexdump(b, n=208, base=0):
    for i in range(0, min(n, len(b)), 16):
        row = b[i:i + 16]
        print(f"    {base + i:03X}  {row.hex(' ')}  {''.join(chr(c) if 32 <= c < 127 else '.' for c in row)}")


names = {f.upper(): os.path.getsize(os.path.join(T, f)) for f in os.listdir(T)}

# --- table -> file ----------------------------------------------------------
l5 = open(os.path.join(T, "L5RA.BUN"), "rb").read()
stat = Counter()
miss = []
for k, s, e, _ in walk(l5):
    if k != 0x34110:
        continue
    for i in range((e - s) // 208):
        en = l5[s + 208 * i: s + 208 * (i + 1)]
        tag = en[:8].split(b"\0")[0].decode()
        num, = struct.unpack_from("<I", en, 8)
        h18, h1c, f20, f24 = struct.unpack_from("<IIII", en, 0x18)
        cnt, = struct.unpack_from("<I", en, 0x5C)
        sz = struct.unpack_from("<III", en, 0x60)
        by_num = f"STREAML5RA_{num}.BUN"
        by_h = [f"STREAML5RA_0X{h:08X}.BUN" for h in (h18, h1c)]
        if by_num in names:
            stat["by number"] += 1
            stat[f"  size==+60:{names[by_num] == sz[0]} size==+68:{names[by_num] == sz[2]}"] += 1
            stat[f"  count={cnt}"] += 1
        elif by_h[0] in names:
            stat["by hash@18"] += 1
            stat[f"  h size==+60:{names[by_h[0]] == sz[0]} +68:{names[by_h[0]] == sz[2]} cnt={cnt}"] += 1
        elif by_h[1] in names:
            stat["by hash@1C"] += 1
        else:
            stat["no file"] += 1
            if len(miss) < 4:
                miss.append((tag, num, hex(h18), hex(h1c), f20, f24, cnt, sz))
for k, v in sorted(stat.items()):
    print(k, v)
print("missing samples", miss)

# --- materials + vertices ---------------------------------------------------
seen = set()
files = sorted(f for f in os.listdir(T) if f.upper().startswith("STREAML5RA_") and "_0x" not in f and f.upper().endswith(".BUN"))
mat_shown = 0
for f in files[:120]:
    b = open(os.path.join(T, f), "rb").read()
    for k, s, e, d in walk(b):
        if k != 0x80134100:
            continue
        mats, streams, idx = [], [], None
        for k2, s2, e2, _ in walk(b, s, e):
            if k2 == 0x134B02:
                q = skip11(b, s2, e2)
                mats = [b[r:r + 116] for r in range(q, e2 - 115, 116)]
            elif k2 == 0x134B01:
                q = skip11(b, s2, e2, 128)
                streams.append(b[q:e2])
            elif k2 == 0x134B03:
                q = skip11(b, s2, e2)
                idx = b[q:e2]
        if mat_shown < 2 and len(mats) >= 2:
            print(f"materials of an object in {f} ({len(mats)} mats, {len(streams)} streams, idx bytes {len(idx)})")
            for m in mats[:2]:
                hexdump(m, 116)
                print()
            mat_shown += 1
        si = -1
        prev = None
        for m in mats:
            eff = struct.unpack_from("<I", m, 8)[0]
            if eff != prev:
                si += 1
                prev = eff
                vc_run = sum(struct.unpack_from("<I", x, 0x40)[0] for x in mats if struct.unpack_from("<I", x, 8)[0] == eff)
                if eff not in seen and si < len(streams) and vc_run:
                    stride = len(streams[si]) // vc_run
                    seen.add(eff)
                    print(f"effect {eff:X} stride {stride} ({f}) first 2 vertices:")
                    v = streams[si]
                    for j in range(2):
                        raw = v[j * stride:(j + 1) * stride]
                        fl = struct.unpack_from(f"<{stride // 4}f", raw)
                        print("    hex", raw.hex(" ", 4))
                        print("    flt", " ".join(f"{x:.3f}" if abs(x) < 1e6 and (abs(x) > 1e-4 or x == 0) else "~" for x in fl))

# --- texture pack tree ------------------------------------------------------
tf = next(f for f in os.listdir(T) if "_0x" in f and os.path.getsize(os.path.join(T, f)) < 400000 and os.path.getsize(os.path.join(T, f)) > 60000)
b = open(os.path.join(T, tf), "rb").read()
print("texture file", tf, len(b))
for k, s, e, d in walk(b):
    if k == 0:
        continue
    print("  " * d + f"  {k:08X} {e - s}")
    if k in (0x33310004, 0x33310005, 0x33310002, 0x33310001, 0x33310021, 0x33310022, 0x33320001):
        hexdump(b[skip11(b, s, e):e], 176 if k == 0x33310004 else 64)
