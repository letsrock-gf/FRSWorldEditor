"""Which scenery models no TRACKS file stores: for every placed record (0x34103) of every section file, the model its
info (0x34102, LOD hashes @0x18) names first, looked up among every solid object (0x134011, hash @0x10) of every
TRACKS file. Lists the infos whose model is stored nowhere, by records placing them, with an example section.
Usage: unresolved_models.py <TRACKS folder> [name filter regex]"""
import os
import re
import struct
import sys
from collections import Counter, defaultdict

T = sys.argv[1]
flt = re.compile(sys.argv[2], re.I) if len(sys.argv) > 2 else None


def walk(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000:
            yield from walk(b, p + 8, p + 8 + n)
        p += 8 + n


def kids(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            return
        yield k, p + 8, p + 8 + n
        p += 8 + n


files = [f for f in sorted(os.listdir(T)) if f.upper().endswith(".BUN") and f.upper().startswith("STREAM")]
stored = set()
data = {}
for f in files:
    kept = os.path.join(T, "frs-original", f)
    b = open(kept if os.path.exists(kept) else os.path.join(T, f), "rb").read()
    data[f] = b
    for k, s, e in walk(b, 0, len(b)):
        if k == 0x134011:
            s = (s + 15) & ~15   # the loader rounds this chunk's data up to 16 (0x11 filler)
            if e - s >= 0x14:
                stored.add(struct.unpack_from("<I", b, s + 0x10)[0])

missing = Counter()
where = defaultdict(set)
hashes = {}
total = 0
for f, b in data.items():
    for k, s, e in kids(b, 0, len(b)):
        if k != 0x80034100:
            continue
        ch = {kk: (ss, ee) for kk, ss, ee in kids(b, s, e)}
        if 0x34102 not in ch or 0x34103 not in ch:
            continue
        is_, ie = ch[0x34102]
        infos = [b[q:q + 72] for q in range(is_, ie - 71, 72)]
        rs, re_ = ch[0x34103]
        rs = (rs + 15) & ~15
        for r in range(rs, re_ - 95, 96):
            info = struct.unpack_from("<h", b, r + 0x54)[0]
            if not 0 <= info < len(infos):
                continue
            if b[r + 0x2C:r + 0x50] == bytes(36):
                continue   # removed (3x3 zeroed)
            total += 1
            raw = infos[info]
            lod = [h for h in struct.unpack_from("<4I", raw, 0x18) if h]
            if lod and not any(h in stored for h in lod):
                name = raw[:24].split(b"\0")[0].decode("latin1")
                missing[name] += 1
                where[name].add(f)
                hashes[name] = lod[0]
print(f"{total} placed records, {sum(missing.values())} whose model no TRACKS file stores ({len(missing)} infos)")
for name, n in missing.most_common(None if flt else 40):
    if flt and not flt.search(name):
        continue
    print(f"{n:6d}  {hashes[name]:08X}  {name:24s} e.g. {sorted(where[name])[0]}")
