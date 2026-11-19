import os, struct, sys
from collections import Counter
T = sys.argv[1]
names = {f.upper() for f in os.listdir(T)}
b = open(os.path.join(T, "L5RA.BUN"), "rb").read()
p = 0
while p < len(b):
    k, n = struct.unpack_from("<II", b, p)
    if k == 0x34110: s, e = p + 8, p + 8 + n
    p += 8 + n
c = Counter(); ex = {}
for i in range((e - s) // 208):
    en = b[s + 208 * i: s + 208 * (i + 1)]
    tag = en[:8].split(b"\0")[0].decode(); num = struct.unpack_from("<I", en, 8)[0]
    h1c, f20, f24 = struct.unpack_from("<III", en, 0x1C)
    pos = struct.unpack_from("<3f", en, 0x28); cnt = struct.unpack_from("<I", en, 0x5C)[0]
    kind = "num" if f"STREAML5RA_{num}.BUN" in names else "hash"
    key = (kind, tag[0], "pos" if pos[0] or pos[1] else "nopos", f"f20={f20}", f"cnt={cnt}", f"f24-num={f24 - num}")
    c[key] += 1; ex.setdefault(key, (tag, num, hex(h1c)))
for k, v in sorted(c.items()): print(v, k, ex[k])
