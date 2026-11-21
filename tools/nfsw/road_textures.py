"""Texture names of the map's packs that look like road surface, lines, pavement or kerb, with how many
packs hold each and the size: the candidates for what the road bake draws with on NFS World.
Usage: road_textures.py [regex]  (default: road-like words)"""
import glob, os, re, struct, sys
from collections import Counter
from game_folder import TRACKS

pattern = re.compile(sys.argv[1] if len(sys.argv) > 1 else
                     r"ROAD|ASPH|LANE|STRIPE|LINE|SIDEWALK|CURB|KERB|CROSSWALK|PAVE|CONCRETE|TARMAC|MEDIAN|INTERSECT|GRAVEL|DIRT|GRASS")
seen, info = Counter(), {}


def walk(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000: yield from walk(b, p + 8, p + 8 + n)
        p += 8 + n


for path in glob.glob(os.path.join(TRACKS, "STREAML5RA_*.BUN")) + [os.path.join(TRACKS, "L5RA.BUN")]:
    b = open(path, "rb").read()
    for k, s, e in walk(b, 0, len(b)):
        if k != 0x33310004: continue
        p = s
        while p + 0x91 <= e:
            ln = b[p + 0x90]
            name = b[p + 0x91:p + 0x91 + ln].split(b"\0")[0].decode("latin1")
            h, = struct.unpack_from("<I", b, p + 0x0C)
            w, hh = struct.unpack_from("<II", b, p + 0x20)
            if pattern.search(name):
                seen[name] += 1
                info[name] = (h, w, hh)
            p += 0x91 + ln
for name, n in sorted(seen.items(), key=lambda x: (-x[1], x[0])):
    h, w, hh = info[name]
    print(f"{n:4} {h:08X} {w}x{hh} {name}")
