"""Measures, over every texture section (STREAML5RA_0x*.BUN), what a texture added to a section's pack must agree
with: the data header's texture count, the name hash, the word at +0x10, mips against size, and textures that have
no larger level in any 0xB3300011 pack.   python newtex_fields.py <TRACKS folder> [limit]"""
import os, struct, sys
from collections import Counter
T = sys.argv[1]; LIMIT = int(sys.argv[2]) if len(sys.argv) > 2 else 10 ** 9


def walk(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000: yield from walk(b, p + 8, p + 8 + n)
        p += 8 + n


def bhash(s, h=0xFFFFFFFF):
    for c in s.encode(): h = (h * 33 + c) & 0xFFFFFFFF
    return h


c = Counter(); w10 = Counter(); w14 = Counter(); b30 = Counter(); ex = {}
files = [f for f in sorted(os.listdir(T)) if f.startswith("STREAML5RA_0x") and f.endswith(".BUN")][:LIMIT]
for f in files:
    b = open(os.path.join(T, f), "rb").read()
    ents = []; rows = set(); dcount = None; npacks = 0; fmt = []
    for k, s, e in walk(b, 0, len(b)):
        if k == 0xB3300000: npacks += 1
        if k == 0x33310004:
            p = s
            while p + 0x91 <= e:
                ln = b[p + 0x90]; ents.append((p, b[p + 0x91:p + 0x91 + ln].split(b"\0")[0].decode("latin1"))); p += 0x91 + ln
        if k == 0x33310005: fmt += [b[q + 12:q + 16] for q in range(s, e, 32)]
        if k == 0x33320001 and dcount is None: dcount = struct.unpack_from("<I", b, s + 8)[0]
        if k == 0x33310022:
            for q in range(s, e, 32): rows.add(struct.unpack_from("<I", b, q)[0])
    c[f"packs={npacks}"] += 1
    if not ents: c["no textures"] += 1; continue
    c["datacount==n" if dcount == len(ents) else f"datacount!=n"] += 1
    for i, (p, name) in enumerate(ents):
        h, h10, h14 = struct.unpack_from("<3I", b, p + 0x0C)
        wd, ht = struct.unpack_from("<II", b, p + 0x20); mips = b[p + 0x31]
        c["hash==bhash(name)" if h == bhash(name) else "hash!=bhash(name)"] += 1
        c["hash==bhash(upper)" if h == bhash(name.upper()) else "hash!=bhash(upper)"] += 1
        c["name upper" if name == name.upper() else "name not upper"] += 1
        w10[h10 == 0x001A93CF and "1A93CF" or ("other")] += 1
        w14[f"{h14:08X}"] += 1
        b30[f"{b[p+0x30]:02X} {b[p+0x32]:02X} {b[p+0x33]:02X} {fmt[i] if i < len(fmt) else b'?'}"] += 1
        full = 0; m = max(wd, ht)
        c[f"maxdim {m}"] += 1
        c["has row" if h in rows else "no row"] += 1
        if h not in rows: ex.setdefault("norow", (f, name, wd, ht, mips))
        chainmin = min(wd, ht) >> (mips - 1) if mips else 0
        c[f"smallest side {chainmin}"] += 1
for k in sorted(c): print(f"{c[k]:7d}  {k}")
print("w10", w10.most_common(5)); print("w14", w14.most_common(8)); print("b30/b32/b33/fmt", b30.most_common(16)); print(ex)
