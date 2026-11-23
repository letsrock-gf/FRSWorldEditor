"""Reads the road network NFS World's traffic, AI and GPS follow: L5RA.BUN chunk 0x3B800 (a 'CARP' row table, the
same row layout as the collision packs: {tag, size << 8 | kind, count, offset}, kind bit 1 = offset from the row).
nfsw.exe 0x6F5E30 loads the chunk; 0x6F1790 (WRoadNetwork init) reads RNhd and binds RNpf, RNnd, RNsg, RNrd, RNlm.
Usage: road_network.py <L5RA.BUN> [--dump N]"""
import struct
import sys

STRIDE = {b"RNpf": 64, b"RNnd": 32, b"RNsg": 24, b"RNrd": 8, b"RNlm": 32}


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def carp(region):
    at = 0
    while at + 8 <= len(region):
        cid, size = u32(region, at), u32(region, at + 4)
        if cid == 0x3B800:
            return region[(at + 8 + 15) & ~15:at + 8 + size], (at + 8 + 15) & ~15
        at += 8 + size
    raise SystemExit("no chunk 0x3B800")


def rows(p):
    out, at = {}, 0
    while at + 16 <= len(p):
        tag = p[at:at + 4][::-1]
        word, count, offset = struct.unpack_from("<III", p, at + 4)
        if tag == b"\xaa\xaa\xaa\xaa":
            break
        kind, size = word & 0xFF, word >> 8
        start = at + offset if kind & 2 else offset
        out[tag] = (kind, size, count, start, at)
        at += 16
    return out


def main():
    region = open(sys.argv[1], "rb").read()
    p, base = carp(region)
    r = rows(p)
    for tag, (kind, size, count, start, row) in r.items():
        print("%s kind %02x size %7d count %5d data +0x%06x%s" % (
            tag.decode(), kind, size, count, start,
            "  stride %d" % (size // count) if count and kind & 2 and size % count == 0 else ""))
    hd = struct.unpack_from("<7H", p, r[b"RNhd"][3])
    print("RNhd", hd, "(profiles, nodes, segments, x188, roads, ?, lanes)")
    for tag, n in ((b"RNpf", hd[0]), (b"RNnd", hd[1]), (b"RNsg", hd[2]), (b"RNrd", hd[4]), (b"RNlm", hd[6])):
        kind, size, count, start, row = r[tag]
        print("  %s header count %d, row count %d, bytes %d = %d x %d: %s" % (
            tag.decode(), n, count, size, n, STRIDE[tag], "ok" if count == n and size == n * STRIDE[tag] else "MISMATCH"))
    if "--dump" in sys.argv:
        k = int(sys.argv[sys.argv.index("--dump") + 1])
        for tag in STRIDE:
            kind, size, count, start, row = r[tag]
            print("--", tag.decode())
            for i in range(min(k, count)):
                e = p[start + i * STRIDE[tag]:start + (i + 1) * STRIDE[tag]]
                print("  %4d" % i, e.hex(" "))


if __name__ == "__main__":
    main()
