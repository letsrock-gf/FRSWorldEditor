"""Histogram of EAGL chunk ids in BUN files. Usage: chunks.py <file> [file...]"""
import struct
import sys
from collections import Counter


def walk(buf, lo, hi, depth, out):
    pos = lo
    while pos + 8 <= hi:
        cid, size = struct.unpack_from("<II", buf, pos)
        if pos + 8 + size > hi:
            out[("BAD", depth)] += 1
            return
        if cid != 0:  # 0 = padding chunk
            out[(cid, depth)] += 1
            out[("bytes", cid)] += size
            if cid & 0x80000000:
                walk(buf, pos + 8, pos + 8 + size, depth + 1, out)
        pos += 8 + size


for path in sys.argv[1:]:
    buf = open(path, "rb").read()
    out = Counter()
    walk(buf, 0, len(buf), 0, out)
    print("==", path.replace("\\", "/").split("/")[-1], len(buf), "head", buf[:8].hex())
    for key, n in sorted((k, v) for k, v in out.items() if k[0] != "bytes"):
        cid, depth = key
        if cid == "BAD":
            print("  " * depth + f"  BAD x{n}")
        else:
            print("  " * depth + f"  {cid:08X} x{n} bytes={out[('bytes', cid)]}")
