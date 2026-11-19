"""Header statistics over every collision article of every 0x3B801 chunk."""
import struct
from collections import Counter

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
    stat = Counter()
    smallest = None
    for ci, (off, size) in enumerate(cols):
        data = buf[off:off + size]
        for tag, size, kind, count, pos in pack_entries(data):
            if tag >> 16 != 0x6361:
                stat[("tag", tag >> 16 if tag >> 16 in (0x6369,) else tag, kind)] += 1
                continue
            strips, strip_bytes, edges, edge_bytes, b8, surfs, surf_bytes, flags = struct.unpack_from(
                "<4H2BHI", data, pos)
            stat["articles"] += 1
            stat[("edge bytes == 32n", edge_bytes == 32 * edges)] += 1
            stat[("surf bytes == 4n", surf_bytes == 4 * surfs)] += 1
            stat[("b8", b8)] += 1
            stat[("surfs", surfs)] += 1
            used = 16 + strip_bytes + edge_bytes + surf_bytes
            stat[("slack", size - used)] += 1
            stat[("has strips", strips > 0)] += 1
            stat[("flags hi", flags >> 16)] += 1
            if strips and (smallest is None or size < smallest[0]):
                smallest = (size, ci, tag & 0xFFFF, strips, strip_bytes, edges)
    for key, n in sorted(stat.items(), key=lambda kv: str(kv[0])):
        print(key, n)
    print("smallest with strips (size, chunk, article, strips, strip bytes, edges):", smallest)


main()
