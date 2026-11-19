"""Statistics of how texture data is laid out in the pack (0xB3300000) and in the larger-level packs (0xB3300011),
over every third section file.   python tex_layout_stats.py <TRACKS folder>"""
import os, struct, sys
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
def chain(w, h, mips, block):
    return sum(max(1, (w >> m) + 3 >> 2) * max(1, (h >> m) + 3 >> 2) * block for m in range(mips))
c = Counter(); tier = Counter(); ratio = Counter(); ex = {}
files = [f for f in sorted(os.listdir(T)) if f.startswith("STREAML5RA_") and f.endswith(".BUN")][::3]
for f in files:
    b = open(os.path.join(T, f), "rb").read()
    ent = []; fm = []; packdims = {}; level = None; rows = None; inpack = False
    for k, s, e in walk(b):
        if k == 0x33310004:
            p = s; ent = []
            while p + 0x91 <= e:
                ln = b[p + 0x90]; ent.append(p); p += 0x91 + ln
        if k == 0x33310005: fm = [b[q + 12:q + 16] for q in range(s, e, 32)]
        if k == 0xB3320000: inpack = True
        if k == 0x33310021: level = struct.unpack_from("<I", b, s + 4)[0]; inpack = False
        if k == 0x33310022: rows = (s, e)
        if k == 0x33320002 and inpack:
            inpack = False
            data = e - ((s + 127) & ~127); expect = 0
            for i, p in enumerate(ent):
                h, = struct.unpack_from("<I", b, p + 0x0C)
                size, base, w, hh, lw, lh = struct.unpack_from("<6I", b, p + 0x18)
                mips = b[p + 0x31]; sz2, off = struct.unpack_from("<II", b, p + 0x4C)
                packdims[h] = (w, hh)
                cc = fm[i] if i < len(fm) else b"?"
                block = 8 if cc in (b"DXT1", b"ATI1") else 16 if cc in (b"DXT3", b"DXT5", b"ATI2") else 0
                c["size==sz2" if size == sz2 else "size!=sz2"] += 1
                c["off==running" if off == expect else "off!=running"] += 1
                c["off%128==0" if off % 128 == 0 else "off%128!=0"] += 1
                if block:
                    ch = chain(w, hh, mips, block)
                    key = "size==chain" if size == ch else "size==chain up to 128" if size == (ch + 127 & ~127) else "size other"
                    c[key] += 1
                    if key == "size other" and len(ex.setdefault(key, [])) < 4: ex[key].append((f, w, hh, mips, cc, hex(size), hex(ch)))
                    c["base==level0" if base == chain(w, hh, 1, block) else "base other"] += 1
                c["log2 ok" if (1 << lw, 1 << lh) == (w, hh) else "log2 other"] += 1
                c[("mips full" if (max(w, hh) >> (mips - 1)) in (1, 2, 4) else "mips short")] += 1
                expect = off + size
            c["data==sum" if data == expect else "data>sum" if data > expect else "data<sum"] += 1
            if data != expect and len(ex.setdefault("data", [])) < 4: ex["data"].append((f, hex(data), hex(expect)))
        if k == 0x33320002 and rows is not None and level is not None:
            n = (rows[1] - rows[0]) // 32; data = e - ((s + 127) & ~127); end = 0
            for i in range(n):
                h, off, size, base, w, hh, lw, lh, mips = struct.unpack_from("<4I2H3I", b, rows[0] + 32 * i)
                tier[(level, max(w, hh))] += 1
                if h in packdims: ratio[(level, w // packdims[h][0], hh // packdims[h][1])] += 1
                else: ratio[(level, "no pack entry")] += 1
                end = max(end, off + base)
                c["row log2 ok" if (1 << lw, 1 << lh) == (w, hh) else "row log2 other"] += 1
            c["stream data==sum" if data == end else "stream data>sum" if data > end else "stream data<sum"] += 1
            if n == 0: c[f"empty level {level}"] += 1
            rows = None; level = None
print(len(files), "files"); print(sorted(c.items())); print("tier (level, max dim):", sorted(tier.items()))
print("ratio (level, w ratio, h ratio):", sorted(ratio.items(), key=str)); print(ex)
