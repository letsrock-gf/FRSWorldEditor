"""Relations a new object's derived fields must keep, measured on shipped NFSW objects:
header +0x18 vs texture count, header +0x0C bits vs 0x134C05/C06/0x134013/0x13401A presence,
descriptor word 3 vs material flags, material +0x04 vs the texture its slot +0x28 names, one 0x134C02 per material,
texture list second word. Usage: newmodel_rel.py <TRACKS folder> [max files]"""
import glob, os, struct, sys
from collections import Counter
from mesh_probe import walk

T = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 800
names = sorted(glob.glob(os.path.join(T, "STREAML5RA_*.BUN")))
names = [n for n in names if not n.endswith("_25000.BUN")][:: max(1, len(names) // limit)]


def data(b, s, e):
    f = 0
    while s + f < e and b[s + f] == 0x11 and f < 16:
        f += 1
    return s + f


st = Counter()
for path in names:
    b = open(path, "rb").read()
    for k, h, s, e in walk(b, 0, len(b)):
        if k != 0x80134000:
            continue
        for k1, h1, s1, e1 in walk(b, s, e):
            if k1 != 0x80134010:
                continue
            kids = {}
            for k2, h2, s2, e2 in walk(b, s1, e1):
                kids.setdefault(k2, []).append((data(b, s2, e2), e2))
            if 0x134011 not in kids or 0x80134100 not in kids:
                continue
            hd = kids[0x134011][0][0]
            tl = kids.get(0x134012, [(0, 0)])[0]
            tex = [struct.unpack_from("<II", b, tl[0] + 8 * i) for i in range((tl[1] - tl[0]) // 8)]
            st[f"texlist second word all 0 {all(t[1] == 0 for t in tex)}"] += 1
            h18 = struct.unpack_from("<I", b, hd + 0x18)[0]
            st[f"hdr+0x18 == ntex<<8 {h18 == len(tex) << 8}"] += 1
            if h18 != len(tex) << 8: st[f"hdr+0x18 {h18:#x} ntex {len(tex)}"] += 1
            h0c = struct.unpack_from("<I", b, hd + 0x0C)[0]
            ms, mk = kids[0x80134100][0][0], kids[0x80134100][0][1]
            mkids = {}
            for k3, h3, s3, e3 in walk(b, ms - (ms - kids[0x80134100][0][0]), mk):
                mkids.setdefault(k3, []).append((data(b, s3, e3), e3))
            has = (0x134C05 in mkids, 0x134C06 in mkids, 0x134013 in kids, 0x13401A in kids)
            st[f"hdr+0x0C {h0c:#x} C05,C06,13,1A {has}"] += 1
            if 0x134B02 not in mkids or 0x134900 not in mkids:
                continue
            md, me = mkids[0x134B02][0]
            nm = (me - md) // 116
            flags = [struct.unpack_from("<I", b, md + 116 * m)[0] for m in range(nm)]
            orf = 0
            for f in flags:
                orf |= f
            d3 = struct.unpack_from("<I", b, mkids[0x134900][0][0] + 12)[0]
            st[f"desc w3 == OR of material flags {d3 == orf}; == first {d3 == flags[0]}"] += 1
            st[f"one 0x134C02 per material {len(mkids.get(0x134C02, [])) == nm}"] += 1
            for m in range(nm):
                at = md + 116 * m
                diff, eff = struct.unpack_from("<II", b, at + 4)
                s0 = b[at + 0x28]
                st[f"mat +0x04 == texlist[slot +0x28] {s0 < len(tex) and tex[s0][0] == diff}"] += 1
                hs = [t[0] for t in tex]
                which = [i for i in range(5) if b[at + 0x28 + i] < len(tex) and hs[b[at + 0x28 + i]] == diff]
                st[f"mat +0x04 in texlist {diff in hs}; slot positions naming it {which}"] += 1
                st[f"slots {bytes(b[at+0x28:at+0x2E]).hex()} ntex {len(tex)}"] += 1
                st[f"mat bytes 0x2E..0x3F zero {b[at + 0x2E:at + 0x40] == bytes(18)}"] += 1
                st[f"mat +0x0C zero {b[at + 0x0C:at + 0x10] == bytes(4)}"] += 1
for k, v in sorted(st.items()):
    print(f"{v:7}  {k}")
