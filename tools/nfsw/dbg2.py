import os, struct, sys
T = sys.argv[1]
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
b = open(os.path.join(T, sys.argv[2]), "rb").read()
for k, s, e in walk(b):
    if k == 0x34103: ni = (e - al(b, s, e)) // 96
    if k == 0x34102: nf = (e - al(b, s, e)) // 72
    if k == 0x3410F:
        q = al(b, s, e); n = (e - q) // 32
        rows = [struct.unpack_from("<4I", b, q + 32 * i + 16) for i in range(n)]
        print("instances", ni, "infos", nf, "3410F entries", n, "pad", q - s)
        for c in range(4):
            col = [r[c] for r in rows]; print(f"  field {c}: min {min(col)} max {max(col)} distinct {len(set(col))}")
        print("  first rows", rows[:12]); print("  last rows", rows[-4:])
        hw = [struct.unpack_from("<8H", b, q + 32 * i + 16) for i in range(6)]; print("  as u16", hw)
    if k == 0x34105:
        q = al(b, s, e); print("34105", (e - q), "bytes;", [b[q + 32 * i: q + 32 * i + 32].hex(" ", 4) for i in range(3)])
