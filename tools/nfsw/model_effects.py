"""Material effects (0x134B02 +0x08) of solid objects whose name matches a regex, in every TRACKS file:
python model_effects.py <TRACKS folder> <name regex>"""
import os
import re
import struct
import sys
from collections import Counter

T, pat = sys.argv[1], re.compile(sys.argv[2], re.I)


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


seen = {}
for f in sorted(os.listdir(T)):
    if not (f.upper().startswith("STREAM") and f.upper().endswith(".BUN")):
        continue
    kept = os.path.join(T, "frs-original", f)
    b = open(kept if os.path.exists(kept) else os.path.join(T, f), "rb").read()
    for k, s, e in walk(b, 0, len(b)):
        if k != 0x80134010:
            continue
        name, effects = None, []
        for kk, ss, ee in walk(b, s, e):
            if kk == 0x134011:
                ss = (ss + 15) & ~15
                name = b[ss + 0xA0:ee].split(b"\0")[0].decode("latin1")
                h = struct.unpack_from("<I", b, ss + 0x10)[0]
            elif kk == 0x134B02:
                ss = (ss + 15) & ~15
                effects += [struct.unpack_from("<I", b, q + 8)[0] for q in range(ss, ee - 115, 116)]
        if name and pat.search(name) and name not in seen:
            seen[name] = (h, Counter(effects), f)
for name, (h, eff, f) in sorted(seen.items()):
    print(f"{h:08X} {name:40s} effects {dict(eff)}  ({f})")
