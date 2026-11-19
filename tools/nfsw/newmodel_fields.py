"""Fields a NEW solid object and its scenery info must carry, measured on the shipped NFSW files.
- every solid pack header 0x134002 of one numbered file (what tells its packs apart);
- object header 0x134011 words that are the same in every object, and the matrix;
- mesh container children order; descriptor 0x134900 words;
- material bytes 0x20..0x3F against the texture list length (which bytes are texture slots);
- scenery info words 0x38..0x47 and the LOD pattern; record flags.
Usage: newmodel_fields.py <TRACKS folder> <numbered section file name> [max files]"""
import glob
import os
import struct
import sys
from collections import Counter, defaultdict

from mesh_probe import walk

T = sys.argv[1]
ONE = sys.argv[2]
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 600
ORIG = os.path.join(T, "frs-original")


def shipped(name):
    p = os.path.join(ORIG, name)
    return p if os.path.exists(p) else os.path.join(T, name)


def data(b, s, e):
    f = 0
    while s + f < e and b[s + f] == 0x11 and f < 16:
        f += 1
    return s + f


b = open(shipped(ONE), "rb").read()
print("== solid pack headers of", ONE)
for k, h, s, e in walk(b, 0, len(b)):
    if k != 0x80134000:
        continue
    n = sum(1 for k1, *_ in walk(b, s, e) if k1 == 0x80134010)
    for k1, h1, s1, e1 in walk(b, s, e):
        if k1 == 0x80134001:
            for k2, h2, s2, e2 in walk(b, s1, e1):
                d = data(b, s2, e2)
                if k2 == 0x134002:
                    w = struct.unpack_from("<4I", b, d)
                    names = [x.decode("latin1") for x in b[d + 16:e2].split(b"\0") if x]
                    print(f"  pack @{h:#x} objs {n} words {w} strings {names} tail {b[e2 - 32:e2].hex()}")
                else:
                    print(f"    child {k2:08X} size {e2 - s2} filler {d - s2}")

names = sorted(os.path.basename(f) for f in glob.glob(os.path.join(T, "STREAML5RA_*.BUN")))
names = [n for n in names if not n.endswith("_25000.BUN")]
names = names[:: max(1, len(names) // limit)]

hdr_const = None
hdr_vals = defaultdict(Counter)
mesh_orders = Counter()
desc_vals = defaultdict(Counter)
slot = defaultdict(Counter)   # byte offset -> relation to texture count
mat_vals = defaultdict(Counter)
info_vals = defaultdict(Counter)
lod = Counter()
rec_vals = defaultdict(Counter)
objects = 0
for name in names:
    b = open(shipped(name), "rb").read()
    for k, h, s, e in walk(b, 0, len(b)):
        if k == 0x80034100:
            for k1, h1, s1, e1 in walk(b, s, e):
                d = data(b, s1, e1)
                if k1 == 0x34102:
                    for i in range((e1 - d) // 72):
                        at = d + 72 * i
                        L = struct.unpack_from("<4I", b, at + 0x18)
                        pat = "".join("A" if x == L[0] else ("0" if x == 0 else "x") for x in L)
                        lod[pat] += 1
                        w = struct.unpack_from("<4I", b, at + 0x38)
                        info_vals["0x3C"][w[1] if w[1] < 64 else "big"] += 1
                        info_vals["0x40==L0"][w[2] == L[0]] += 1
                        info_vals["0x40==0"][w[2] == 0] += 1
                        info_vals["0x44"][w[3] if w[3] < 64 else "big"] += 1
                        info_vals["0x28..0x34 zero"][b[at + 0x28:at + 0x38] == bytes(16)] += 1
                if k1 == 0x34103:
                    for i in range((e1 - d) // 96):
                        at = d + 96 * i
                        rec_vals["flags 0x0C"][hex(struct.unpack_from("<I", b, at + 0x0C)[0])] += 1
                        rec_vals["0x1C"][struct.unpack_from("<H", b, at + 0x1E)[0] == 0xFFFF] += 1
                        rec_vals["0x58"][hex(struct.unpack_from("<I", b, at + 0x58)[0])] += 1
        if k != 0x80134000:
            continue
        for k1, h1, s1, e1 in walk(b, s, e):
            if k1 != 0x80134010:
                continue
            objects += 1
            ntex = 0
            hd = None
            for k2, h2, s2, e2 in walk(b, s1, e1):
                d = data(b, s2, e2)
                if k2 == 0x134011:
                    hd = b[d:d + 0xA0]
                    words = struct.unpack_from("<40I", hd)
                    hdr_const = list(words) if hdr_const is None else [
                        c if c == wv else None for c, wv in zip(hdr_const, words)]
                    for i in (0, 1, 2, 3, 6, 7, 11, 15) + tuple(range(16, 38)):
                        hdr_vals[i][hex(words[i]) if i not in range(16, 32) else round(struct.unpack_from("<f", hd, 4 * i)[0], 3)] += 1
                if k2 == 0x134012:
                    ntex = (e2 - d) // 8
                if k2 == 0x80134100:
                    order = []
                    for k3, h3, s3, e3 in walk(b, s2, e2):
                        order.append(k3)
                        d3 = data(b, s3, e3)
                        if k3 == 0x134900:
                            w = struct.unpack_from(f"<{(e3 - d3) // 4}I", b, d3)
                            for i, v in enumerate(w):
                                if i not in (4, 6, 11):
                                    desc_vals[i][hex(v)] += 1
                            desc_vals["size"][e3 - d3] += 1
                        if k3 == 0x134B02:
                            for m in range((e3 - d3) // 116):
                                at = d3 + 116 * m
                                for o in range(0x20, 0x40):
                                    v = b[at + o]
                                    rel = "255" if v == 255 else ("<ntex" if v < ntex else (">=ntex"))
                                    slot[o][rel] += 1
                                for o in (0x00, 0x04, 0x0C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70):
                                    mat_vals[o][hex(struct.unpack_from("<I", b, at + o)[0])] += 1
                    mesh_orders[tuple(dict.fromkeys(order))] += 1

print(f"== {objects} objects in {len(names)} files")
print("header words equal in all:", {f"{i * 4:#x}": hex(v) for i, v in enumerate(hdr_const) if v is not None})
for i, c in sorted(hdr_vals.items()):
    print(f"  hdr +{i * 4:#04x}: {c.most_common(4)}")
print("mesh container child orders:")
for k, v in mesh_orders.most_common(6):
    print(f"  {v:6} " + " ".join(f"{x:08X}" for x in k))
print("descriptor words:")
for i, c in desc_vals.items():
    print(f"  {i}: {c.most_common(4)}")
print("material bytes vs texture count (slots are always < ntex or 255):")
for o, c in sorted(slot.items()):
    print(f"  +{o:#04x}: {dict(c)}")
for o, c in sorted(mat_vals.items()):
    print(f"  mat +{o:#04x}: {c.most_common(4)}")
print("info LOD pattern (A = level 0 hash):", lod.most_common(8))
for k, c in info_vals.items():
    print(f"  info {k}: {c.most_common(5)}")
for k, c in rec_vals.items():
    print(f"  record {k}: {c.most_common(6)}")
