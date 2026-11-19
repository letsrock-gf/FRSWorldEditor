"""Count chunk ids over every TRACKS file (recursing into 0x8xxxxxxx containers).
Usage: scan_ids.py <TRACKS dir> [hex id ...]   no ids = full histogram of ids not seen in L5RA.BUN"""
import glob
import os
import struct
import sys
from collections import Counter, defaultdict


def walk(buf, lo, hi, depth, out, where, name):
    pos = lo
    while pos + 8 <= hi:
        cid, size = struct.unpack_from("<II", buf, pos)
        if pos + 8 + size > hi:
            return
        if cid:
            out[cid] += 1
            where[cid].add(name)
            if cid & 0x80000000:
                walk(buf, pos + 8, pos + 8 + size, depth + 1, out, where, name)
        pos += 8 + size


tracks = sys.argv[1]
want = [int(a, 16) for a in sys.argv[2:]]
out, where = Counter(), defaultdict(set)
files = sorted(glob.glob(os.path.join(tracks, "*.BUN")))
for path in files:
    buf = open(path, "rb").read()
    walk(buf, 0, len(buf), 0, out, where, os.path.basename(path))
print(len(files), "files")
for cid in (want or sorted(out)):
    w = sorted(where[cid])
    print(f"{cid:08X} x{out[cid]} in {len(w)} files", w[:4])
