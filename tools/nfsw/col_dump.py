"""Dump the head of 0x3B801 collision chunks. Usage: col_dump.py [index] [words]"""
import struct
import sys

from game_folder import COLLISION as PATH


def chunks(buf):
    pos = 0
    while pos + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, pos)
        yield cid, pos + 8, size
        pos += 8 + size


def main():
    buf = open(PATH, "rb").read()
    cols = [(o, s) for c, o, s in chunks(buf) if c == 0x3B801]
    print("count", len(cols), "sizes", min(s for _, s in cols), max(s for _, s in cols))
    idx = int(sys.argv[1]) if len(sys.argv) > 1 else min(range(len(cols)), key=lambda i: cols[i][1])
    words = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    off, size = cols[idx]
    print("chunk", idx, "file off", hex(off), "size", size)
    data = buf[off:off + size]
    for i in range(0, min(size, words * 4), 16):
        row = data[i:i + 16]
        u = struct.unpack_from("<%dI" % (len(row) // 4), row)
        f = struct.unpack_from("<%df" % (len(row) // 4), row)
        txt = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        print("%05X  %s  %s  %s" % (i, " ".join("%08X" % x for x in u),
                                     " ".join("%11.4g" % x for x in f), txt))


main()
