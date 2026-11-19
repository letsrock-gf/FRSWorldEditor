"""Per effect: what kind of texture each of a material's five slot bytes (+0x28..+0x2C) names."""
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
files = [f for f in sorted(os.listdir(T)) if f.upper().startswith("STREAML5RA_") and f.upper().endswith(".BUN")]
fmt = {}; name = {}
for f in files:
    if "_0x" not in f and "25000" not in f and "2500" not in f: continue
    b = open(os.path.join(T, f), "rb").read(); hdr = fm = None
    for k, s, e in walk(b):
        if k == 0x33310004: hdr = (s, e)
        elif k == 0x33310005: fm = (s, e)
        elif k == 0xB3320000 and hdr and fm:
            p, i = hdr[0], 0
            while p + 0x91 <= hdr[1]:
                ln = b[p + 0x90]; h, = struct.unpack_from("<I", b, p + 0x0C)
                fmt[h] = b[fm[0] + 32 * i + 12: fm[0] + 32 * i + 16].decode("latin1"); name[h] = b[p + 0x91:p + 0x91 + ln].split(b"\0")[0].decode("latin1")
                p += 0x91 + ln; i += 1
            hdr = fm = None
print("textures known", len(fmt))
stat = {}; ex = {}
for f in files[::5]:
    b = open(os.path.join(T, f), "rb").read(); tex = []
    for k, s, e in walk(b):
        if k == 0x134012: q = al(b, s, e, 8); tex = [struct.unpack_from("<I", b, r)[0] for r in range(q, e - 7, 8)]
        elif k == 0x134B02:
            q = al(b, s, e)
            for r in range(q, e - 115, 116):
                eff, = struct.unpack_from("<I", b, r + 8)
                for slot in range(5):
                    idx = b[r + 0x28 + slot]
                    kind = "none(FF)" if idx == 0xFF else ("out of range" if idx >= len(tex) else fmt.get(tex[idx], "unknown hash"))
                    stat.setdefault(eff, [Counter() for _ in range(5)])[slot][kind] += 1
                    if slot == 0 and kind in ("ATI2", "ATI1") and eff not in ex: ex[eff] = (f, name.get(tex[idx]))
for eff in sorted(stat):
    print(f"effect {eff:X}")
    for slot in range(5): print(f"   slot {slot}: {dict(stat[eff][slot].most_common(5))}")
print("slot 0 naming a normal/spec map, examples:", ex)
