"""Statistics of strip headers and packed vertices over every collision article."""
import struct
from collections import Counter

from game_folder import COLLISION as PATH


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
    seqs = Counter()
    for off, size in cols:
        data = buf[off:off + size]
        for tag, size, kind, count, pos in pack_entries(data):
            if tag >> 16 != 0x6361:
                continue
            strips, strip_bytes, edges, edge_bytes, b8, surfs, surf_bytes, flags = struct.unpack_from(
                "<4H2BHI", data, pos)
            base = pos + 16
            offs = [struct.unpack_from("<H", data, base + 16 * i + 14)[0] for i in range(strips)]
            stat[("first offset == 16n", (offs[0] == 16 * strips) if strips else None)] += 1
            stat[("offsets ascending", offs == sorted(offs))] += 1
            for i, o in enumerate(offs):
                end = offs[i + 1] if i + 1 < strips else strip_bytes
                n = (end - o) // 8
                verts = [struct.unpack_from("<3h2B", data, base + o + 8 * k) for k in range(n)]
                # the block is padded to 16 bytes with a zero vertex
                if i + 1 == strips and n and verts[-1] == (0, 0, 0, 0, 0) and n % 2 == 0:
                    verts.pop()
                    stat["tail pad"] += 1
                stat[("verts", min(len(verts), 12))] += 1
                for k, v in enumerate(verts):
                    stat[("flag", v[3], "first" if k == 0 else "second" if k == 1 else "third" if k == 2 else "rest")] += 1
                    stat[("surface < count", v[4] < surfs)] += 1
                seqs[tuple(v[3] for v in verts[:8])] += 1
            for i in range(edges):
                w0, = struct.unpack_from("<I", data, base + strip_bytes + 32 * i + 12)
                stat[("edge w lo byte", w0 & 0xFF)] += 1
                stat[("edge w byte1 < surfs", ((w0 >> 8) & 0xFF) < surfs)] += 1
    for key, n in sorted(stat.items(), key=lambda kv: str(kv[0])):
        print(key, n)
    print("-- flag sequences")
    for key, n in seqs.most_common(25):
        print(key, n)


main()
