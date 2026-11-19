"""How 0x3410E boxes and 0x34105 nodes relate to the 0x34103 placement records."""
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
def al(b, s, e, a=16):
    end = min(e, (s + a - 1) & ~(a - 1)); q = s
    while q < end and b[q] == 0x11: q += 1
    return q if q == end else s
stat = Counter(); shown = 0
files = [f for f in sorted(os.listdir(T)) if f.upper().startswith("STREAML5RA_") and "_0x" not in f and f.upper().endswith(".BUN")]
for f in files:
    b = open(os.path.join(T, f), "rb").read()
    inst = None
    for k, s, e in walk(b):
        if k == 0x34101: inst = links = None; hdr = b[s:e]
        elif k == 0x34103: inst = (al(b, s, e), e)
        elif k == 0x3410E and inst:
            q = al(b, s, e); iq, ie = inst; n = (ie - iq) // 96; rows = (e - q) // 32
            same = 0
            for i in range(min(n, rows)):
                if b[iq + 96 * i: iq + 96 * i + 12] == b[q + 32 * i: q + 32 * i + 12] and b[iq + 96 * i + 16: iq + 96 * i + 28] == b[q + 32 * i + 16: q + 32 * i + 28]: same += 1
            stat["boxes: row i == record i for all" if same == n else "boxes: MISMATCH"] += 1
            stat[f"rows/instances ratio {rows / n:.2f}" if n else "n=0"] += 0
            ratio = rows - n
            # are the rows past n zero?
            tail = b[q + 32 * n: e]
            stat["box rows past n all zero" if not any(tail) else "box rows past n NOT zero"] += 1
            if shown < 2: print(f, "inst", n, "rows", rows, "hdr", hdr[:64].hex(" ", 4))
        elif k == 0x34105 and inst:
            q = al(b, s, e); n = (inst[1] - inst[0]) // 96
            size = e - q
            for cand in (32, 36, 40, 44, 48, 64):
                stat[f"34105 size % {cand} == 0"] += (size % cand == 0)
            stat["34105 chunks"] += 1
            if shown < 2:
                shown += 1
                print("  34105 bytes", size, "per instance", round(size / n, 2))
                for i in range(0, 160, 16):
                    row = b[q + i:q + i + 16]; fl = struct.unpack("<4f", row)
                    print("   ", row.hex(" ", 4), [round(x, 1) if abs(x) < 1e5 and abs(x) > 1e-3 else 0 for x in fl], struct.unpack("<8h", row)[6:])
for k, v in sorted(stat.items()): print(v, k)
