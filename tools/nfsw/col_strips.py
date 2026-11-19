"""Raw view of the strip block of one article. Usage: col_strips.py <chunk> <article> | col_strips.py find <strips>"""
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
    out = []
    for pos in range(0x38, 0x38 + 16 * struct.unpack_from("<I", data, 0x30)[0], 16):
        tag, info, count, off = struct.unpack_from("<4I", data, pos)
        out.append((tag, info >> 8, info & 0xFF, count, pos + off))
    return out


def main():
    buf = open(PATH, "rb").read()
    cols = [(o, s) for c, o, s in chunks(buf) if c == 0x3B801]
    if sys.argv[1] == "find":
        want = int(sys.argv[2])
        hits = []
        for ci, (off, size) in enumerate(cols):
            data = buf[off:off + size]
            for tag, size, kind, count, pos in pack_entries(data):
                if tag >> 16 == 0x6361:
                    strips, strip_bytes, edges = struct.unpack_from("<3H", data, pos)
                    if strips == want:
                        hits.append((strip_bytes, ci, tag & 0xFFFF, edges))
        for hit in sorted(hits)[:12]:
            print("strip bytes %d chunk %d article %d edges %d" % hit)
        return
    off, size = cols[int(sys.argv[1])]
    data = buf[off:off + size]
    for tag, size, kind, count, pos in pack_entries(data):
        if tag >> 16 == 0x6361 and (tag & 0xFFFF) == int(sys.argv[2]):
            strips, strip_bytes, edges, edge_bytes, b8, surfs, surf_bytes, flags = struct.unpack_from(
                "<4H2BHI", data, pos)
            print("strips", strips, "bytes", strip_bytes, "edges", edges, "surfs", surfs, "flags %X" % flags)
            for i in range(pos + 16, pos + 16 + strip_bytes, 8):
                h = struct.unpack_from("<4h", data, i)
                b = data[i:i + 8]
                f = struct.unpack_from("<2f", data, i)
                print("%04X  %s  %6d %6d %6d %6d  %10.4g %10.4g" % ((i - pos - 16, b.hex()) + h + f))
            end = pos + 16 + strip_bytes + edge_bytes
            print("surfaces", ["%08X" % x for x in struct.unpack_from("<%dI" % surfs, data, end)])


main()
