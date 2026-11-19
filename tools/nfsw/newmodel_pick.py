"""Picks test inputs for adding models to one NFSW section (read only):
- the section's centre (table 0x34110 +0x28) and file;
- a texture a model the section loads draws with as its first slot, in an effect the editor writes (a donor);
- a model stored in ANOTHER numbered section, not loaded with this one, whose textures this section's models all
  draw with (must be copied in under a hash of its own), and one whose textures are not (the save must refuse).
"Loaded with" = stored in the section's file or named by any level of its scenery infos (as io/WorldMesh.cpp).
Usage: newmodel_pick.py <TRACKS folder> <section number>"""
import glob
import os
import struct
import sys

from mesh_probe import walk

T, NUMBER = sys.argv[1], int(sys.argv[2])
ORIG = os.path.join(T, "frs-original")
WRITABLE = {0x0, 0x1, 0xA, 0xC, 0xF, 0x2, 0x3, 0x7, 0xB, 0x4, 0x5}


def shipped(name):
    p = os.path.join(ORIG, name)
    return p if os.path.exists(p) else os.path.join(T, name)


def data(b, s, e):
    f = 0
    while s + f < e and b[s + f] == 0x11 and f < 16:
        f += 1
    return s + f


def models(path):
    """hash -> (name, texture list, [(slot0 texture, effect)])"""
    b = open(path, "rb").read()
    out, infos = {}, []
    for k, h, s, e in walk(b, 0, len(b)):
        if k == 0x80034100:
            for k1, h1, s1, e1 in walk(b, s, e):
                if k1 == 0x34102:
                    d = data(b, s1, e1)
                    for i in range((e1 - d) // 72):
                        infos += [x for x in struct.unpack_from("<4I", b, d + 72 * i + 0x18) if x]
        if k != 0x80134000:
            continue
        for k1, h1, s1, e1 in walk(b, s, e):
            if k1 != 0x80134010:
                continue
            name, hsh, tex, mats = "", 0, [], []
            for k2, h2, s2, e2 in walk(b, s1, e1):
                d = data(b, s2, e2)
                if k2 == 0x134011:
                    hsh = struct.unpack_from("<I", b, d + 0x10)[0]
                    name = b[d + 0xA0:e2].split(b"\0")[0].decode("latin1")
                if k2 == 0x134012:
                    tex = [struct.unpack_from("<I", b, s2 + 8 * i)[0] for i in range((e2 - s2) // 8)]
                if k2 == 0x80134100:
                    for k3, h3, s3, e3 in walk(b, s2, e2):
                        if k3 == 0x134B02:
                            d3 = data(b, s3, e3)
                            for m in range((e3 - d3) // 116):
                                slot = b[d3 + 116 * m + 0x28]
                                eff = struct.unpack_from("<I", b, d3 + 116 * m + 8)[0]
                                mats.append((tex[slot] if slot < len(tex) else 0, eff))
            out[hsh] = (name, tex, mats)
    return out, infos


table = open(shipped("L5RA.BUN"), "rb").read()
centre = None
p = 0
while p + 8 <= len(table):
    k, n = struct.unpack_from("<II", table, p)
    if k == 0x34110:
        for i in range(n // 208):
            at = p + 8 + 208 * i
            if struct.unpack_from("<I", table, at + 8)[0] == NUMBER:
                centre = struct.unpack_from("<3f", table, at + 0x28)
                tag = table[at:at + 8].split(b"\0")[0].decode()
    p += 8 + n
mine, infos = models(shipped(f"STREAML5RA_{NUMBER}.BUN"))
everything = {}
for f in sorted(glob.glob(os.path.join(T, "STREAML5RA_*.BUN"))):
    base = os.path.basename(f)
    if base.endswith("_25000.BUN"):
        continue
    ms, _ = models(shipped(base))
    for h, v in ms.items():
        everything[h] = (base,) + v
loaded = set(mine) | set(infos)
textures = set()
for h in loaded:
    if h in everything:
        textures |= set(everything[h][2])
print(f"section {tag} ({NUMBER}) centre {centre}")
print(f"models loaded with it: {len(loaded)} ({len(mine)} in its file), textures they draw with: {len(textures)}")
donors = [(t, e, everything[h][1]) for h in sorted(loaded) if h in everything for t, e in everything[h][3] if e in WRITABLE and t]
print("donor textures (texture, effect, model):", [f"{t:08X} e{e} {n}" for t, e, n in donors[:6]])
ok = bad = None
for h, (home, name, tex, mats) in sorted(everything.items()):
    if h in loaded or home.startswith("STREAML5RA_0x") or not tex:
        continue
    if set(tex) <= textures and ok is None and not name.startswith("TRN_"):
        ok = (h, name, home, len(tex))
    if not set(tex) <= textures and bad is None:
        bad = (h, name, home, [f"{t:08X}" for t in tex if t not in textures][:3])
    if ok and bad:
        break
print(f"copy candidate: {ok[0]:08X} {ok[1]} from {ok[2]} ({ok[3]} textures)" if ok else "copy candidate: none")
print(f"refuse candidate: {bad[0]:08X} {bad[1]} from {bad[2]}, textures not loaded here {bad[3]}" if bad else "refuse candidate: none")
