"""Field statistics over NFSW solid objects and scenery infos, to pin the fields a mesh writer must keep true.
Usage: mesh_stats.py <TRACKS folder> [max files]"""
import glob
import os
import struct
import sys
from collections import Counter

from mesh_probe import walk

T = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 400
files = sorted(glob.glob(os.path.join(T, "STREAML5RA_*.BUN")))
files = [f for f in files if not f.endswith("_25000.BUN")][:: max(1, len(files) // limit)]


def data(b, s, e):
    f = 0
    while s + f < e and b[s + f] == 0x11 and f < 16:
        f += 1
    return s + f


st = Counter()
lods = Counter()
for path in files:
    b = open(path, "rb").read()
    for k, h, s, e in walk(b, 0, len(b)):
        if k == 0x80134000:
            hashes = None
            objs = []
            for k1, h1, s1, e1 in walk(b, s, e):
                if k1 == 0x80134001:
                    for k2, h2, s2, e2 in walk(b, s1, e1):
                        if k2 == 0x134003:
                            d = data(b, s2, e2)
                            hashes = [struct.unpack_from("<II", b, d + 8 * i) for i in range((e2 - d) // 8)]
                if k1 == 0x80134010:
                    objs.append((h1, s1, e1))
            if hashes is not None and objs:
                st[f"hashes rows==objs {len(hashes) == len(objs)}"] += 1
                if len(hashes) == len(objs) and len(objs) > 1:
                    # what the offset counts from
                    o0 = hashes[0][1]; o1 = hashes[1][1]
                    st[f"hash offset diff == object distance {o1 - o0 == objs[1][0] - objs[0][0]}"] += 1
                    st[f"hash off0 - (obj0 - pack data) = {o0 - (objs[0][0] - s)}"] += 1
            for h1, s1, e1 in objs:
                hd = mat = desc = None
                bufs = []
                for k2, h2, s2, e2 in walk(b, s1, e1):
                    if k2 == 0x134011:
                        hd = data(b, s2, e2)
                    if k2 == 0x80134100:
                        for k3, h3, s3, e3 in walk(b, s2, e2):
                            if k3 == 0x134900: desc = data(b, s3, e3)
                            if k3 == 0x134B02: mat = (data(b, s3, e3), e3)
                            if k3 == 0x134B01: bufs.append(e3 - data(b, s3, e3))
                if hd is None or mat is None or desc is None:
                    st["object without header/mat/desc"] += 1
                    continue
                n = (mat[1] - mat[0]) // 116
                m = [struct.unpack_from("<29I", b, mat[0] + 116 * i) for i in range(n)]
                tris = sum(x[18] for x in m); idx = sum(x[17] for x in m); verts = sum(x[16] for x in m)
                st[f"hdr+14 == tris {struct.unpack_from('<I', b, hd + 0x14)[0] == tris}"] += 1
                st[f"mat tris == idx/3 {all(x[18] * 3 == x[17] for x in m)}"] += 1
                st[f"desc+2C == idx {struct.unpack_from('<I', b, desc + 0x2C)[0] == idx}"] += 1
                st[f"desc+10 == mats {struct.unpack_from('<I', b, desc + 0x10)[0] == n}"] += 1
                runs = 1 + sum(1 for i in range(1, n) if m[i][2] != m[i - 1][2])
                st[f"desc+18 == runs {struct.unpack_from('<I', b, desc + 0x18)[0] == runs}"] += 1
                st[f"bufs == runs {len(bufs) == runs}"] += 1
                firsts_ok = all(m[i][19] == sum(x[17] for x in m[:i]) for i in range(n))
                st[f"first index = running sum {firsts_ok}"] += 1
                h18 = struct.unpack_from("<I", b, hd + 0x18)[0]
                st[f"hdr+18 {h18:#x} (verts {'<' if verts < h18 else '>='})"] += 1
                area = struct.unpack_from("<ff", b, hd + 0x98)
                st[f"hdr+9C ~ tris/area {abs(area[1] - tris / area[0]) < 1e-3 * max(1, area[1]) if area[0] else 'area0'}"] += 1
                st[f"mat+00 {m[0][0]:#x}"] += 1
                st[f"desc+0C {struct.unpack_from('<I', b, desc + 0x0C)[0]:#x}"] += 1
        if k == 0x80034100:
            for k1, h1, s1, e1 in walk(b, s, e):
                if k1 == 0x34102:
                    d = data(b, s1, e1)
                    for i in range((e1 - d) // 72):
                        hs = struct.unpack_from("<4I", b, d + 72 * i + 0x18)
                        lods[f"distinct nonzero {len(set(x for x in hs if x))} pattern {''.join('A' if x == hs[0] else ('0' if not x else 'B' if x == hs[1] else 'C' if x == hs[2] else 'D') for x in hs)}"] += 1
for k, v in sorted(st.items()):
    print(f"{v:7} {k}")
print("--- info LOD slots")
for k, v in lods.most_common():
    print(f"{v:7} {k}")
