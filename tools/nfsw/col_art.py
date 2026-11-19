"""Hex/float dump of one collision article or the instances. Usage: col_art.py <chunk> <article|ci> [max bytes]"""
import struct
import sys

PATH = r"D:\NFS World\Data2015\TRACKS\STREAML5RA_25000.BUN"


def chunks(buf):
    pos = 0
    while pos + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, pos)
        yield cid, pos + 8, size
        pos += 8 + size


def pack_entries(data):
    """(tag, size, type, count, absolute offset) for every data row of the pack."""
    out = []
    for pos in range(0x38, 0x38 + 16 * struct.unpack_from("<I", data, 0x30)[0], 16):
        tag, info, count, off = struct.unpack_from("<4I", data, pos)
        out.append((tag, info >> 8, info & 0xFF, count, pos + off))
    return out


def dump(data, lo, hi):
    for i in range(lo, hi, 16):
        row = data[i:i + 16]
        n = len(row) // 4
        u = struct.unpack_from("<%dI" % n, row)
        f = struct.unpack_from("<%df" % n, row)
        h = struct.unpack_from("<%dh" % (n * 2), row)
        print("%05X  %s  %s  %s" % (i - lo, " ".join("%08X" % x for x in u),
                                     " ".join("%10.4g" % x for x in f), " ".join("%6d" % x for x in h)))


def main():
    buf = open(PATH, "rb").read()
    cols = [(o, s) for c, o, s in chunks(buf) if c == 0x3B801]
    off, size = cols[int(sys.argv[1])]
    data = buf[off:off + size]
    want = sys.argv[2]
    limit = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x200
    for tag, size, kind, count, pos in pack_entries(data):
        if want == "ci" and tag >> 16 == 0x6369:
            print("instances", count, "at", hex(pos))
            dump(data, pos, pos + min(size, limit))
        elif want != "ci" and tag >> 16 == 0x6361 and (tag & 0xFFFF) == int(want):
            print("article", want, "size", hex(size), "at", hex(pos))
            dump(data, pos, pos + min(size, limit))


main()
