"""Which shipped objects draw with given textures as the FIRST slot of a material, and how: the material's effect
(0x134B02 +0x08), its five texture slots (+0x28, indices into the object's 0x134012 list, 0xFF = none) by name,
and how many objects share that combination. For choosing a material a new model can copy.
Usage: texture_users.py HASH|NAME [...]"""
import glob, os, struct, sys
from collections import Counter, defaultdict
from game_folder import TRACKS


def walk(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000: yield from walk(b, p + 8, p + 8 + n)
        p += 8 + n


def body(b, s, e):
    while s < e and b[s] == 0x11 and s % 16: s += 1
    while s < e and b[s] == 0x11: s += 1
    return s


def bstr(name):
    h = 0xFFFFFFFF
    for ch in name.encode(): h = (h * 33 + ch) & 0xFFFFFFFF
    return h


wanted = {int(a, 16) if len(a) == 8 and all(c in "0123456789abcdefABCDEF" for c in a) else bstr(a.upper()) for a in sys.argv[1:]}
names, combos, examples = {}, Counter(), defaultdict(list)
files = glob.glob(os.path.join(TRACKS, "STREAML5RA_*.BUN"))
for path in files:
    b = open(path, "rb").read()
    for k, s, e in walk(b, 0, len(b)):
        if k == 0x33310004:
            p = s
            while p + 0x91 <= e:
                ln = b[p + 0x90]
                names[struct.unpack_from("<I", b, p + 0x0C)[0]] = b[p + 0x91:p + 0x91 + ln].split(b"\0")[0].decode("latin1")
                p += 0x91 + ln
        if k != 0x80134010: continue
        name, texlist, mats = "?", [], None
        for k2, s2, e2 in walk(b, s, e):
            d = body(b, s2, e2)
            if k2 == 0x134011 and e2 - d >= 0xA0:
                name = b[d + 0xA0:d + 0xC0].split(b"\0")[0].decode("latin1", "replace")
            elif k2 == 0x134012:
                texlist = [struct.unpack_from("<I", b, q)[0] for q in range(s2, e2 - 7, 8)]
            elif k2 == 0x134B02:
                mats = (d, e2)
        if not mats or not texlist: continue
        d, e2 = mats
        for m in range((e2 - d) // 116):
            a = d + m * 116
            slots = list(b[a + 0x28:a + 0x2D])
            first = texlist[slots[0]] if slots[0] != 0xFF and slots[0] < len(texlist) else None
            if first not in wanted: continue
            effect = struct.unpack_from("<I", b, a + 0x08)[0]
            key = (first, effect, tuple(texlist[x] if x != 0xFF and x < len(texlist) else 0 for x in slots))
            combos[key] += 1
            if len(examples[key]) < 3: examples[key].append(f"{name} ({os.path.basename(path)})")
for (first, effect, slots), n in combos.most_common(30):
    print(f"{n:4} effect {effect:X} slots " + ", ".join(names.get(t, '%08X' % t) if t else '-' for t in slots))
    print("       e.g. " + "; ".join(examples[(first, effect, slots)]))
