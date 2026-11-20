"""Tag table of 0x3B801 collision chunks. Usage: col_tags.py [index...] (no index = statistics)"""
import struct
import sys
from collections import Counter

from game_folder import COLLISION as PATH


def chunks(buf):
    pos = 0
    while pos + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, pos)
        yield cid, pos + 8, size
        pos += 8 + size


def tag_name(tag):
    raw = struct.pack(">I", tag)
    return "".join(chr(b) if 32 <= b < 127 else "\\%d" % b for b in raw)


def entries(data):
    """16-byte rows from 0x18 until the 0xAA padding."""
    pos = 0x18
    out = []
    while pos + 16 <= len(data) and data[pos:pos + 4] != b"\xAA\xAA\xAA\xAA":
        out.append((pos,) + struct.unpack_from("<4I", data, pos))
        pos += 16
    return out


def main():
    buf = open(PATH, "rb").read()
    cols = [(o, s) for c, o, s in chunks(buf) if c == 0x3B801]
    if len(sys.argv) > 1:
        for arg in sys.argv[1:]:
            off, size = cols[int(arg)]
            data = buf[off:off + size]
            print("chunk", arg, "size", size, "head", struct.unpack_from("<6I", data, 0))
            for pos, tag, a, b, c in entries(data):
                print("  %04X %-8s %08X %6d %06X" % (pos, tag_name(tag), a, b, c))
        return
    kinds = Counter()
    heads = Counter()
    for off, size in cols:
        data = buf[off:off + size]
        heads[struct.unpack_from("<6I", data, 0)[0:1] + struct.unpack_from("<6I", data, 0)[2:3] +
              struct.unpack_from("<6I", data, 0)[4:6]] += 1
        for pos, tag, a, b, c in entries(data):
            name = tag_name(tag)
            kinds[(name if not name.startswith("\\0\\0") else name[:8], "%08X" % a if tag >> 16 else "%04X...." % (a >> 16))] += 1
    for key, n in sorted(heads.items()):
        print("head", ["%X" % x for x in key], n)
    for key, n in sorted(kinds.items()):
        print(key, n)


main()
