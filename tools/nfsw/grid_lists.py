"""What the four lists of each collision-grid cell hold (chunk 0x3B802 'CGcn', see src/nfs/WorldCollision.cpp
AddToCollisionGrid). Prints per list: cells using it, entries, value range, and for u16 lists whether they stay below
the road network's segment count (tools/nfsw/road_network.py).
Usage: grid_lists.py <section file with chunk 0x3B802>..."""
import struct
import sys


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def chunks(b, want):
    at = 0
    while at + 8 <= len(b):
        cid, size = u32(b, at), u32(b, at + 4)
        if cid == want:
            yield b[at + 8:at + 8 + size]
        if cid & 0x80000000:
            yield from chunks(b[at + 8:at + 8 + size], want)
        at += 8 + size


stats = [dict(cells=0, entries=0, lo=None, hi=None, sample=[]) for _ in range(4)]
for path in sys.argv[1:]:
    for raw in chunks(open(path, "rb").read(), 0x3B802):
        f = 0
        while f < 16 and raw[f] == 0x11:
            f += 1
        d = raw[f:]
        cell_row = 48
        at = cell_row + u32(d, cell_row + 12)
        count = u32(d, cell_row + 8)
        for _ in range(count):
            counts = d[at + 8:at + 12]
            offs = struct.unpack_from("<4H", d, at + 12)
            for k in range(4):
                n = counts[k]
                if not n:
                    continue
                s = stats[k]
                s["cells"] += 1
                s["entries"] += n
                vals = [u32(d, at + 0x14 + offs[k] + 4 * i) for i in range(n)]
                s["lo"] = min(vals + ([s["lo"]] if s["lo"] is not None else []))
                s["hi"] = max(vals + ([s["hi"]] if s["hi"] is not None else []))
                if len(s["sample"]) < 12:
                    s["sample"] += ["%08x" % v for v in vals[:3]]
            at += 0x14 + 4 * sum(counts)
for k, s in enumerate(stats):
    print("list %d: %d cells, %d entries, range %s..%s, sample %s" % (
        k, s["cells"], s["entries"], s["lo"] and hex(s["lo"]), s["hi"] and hex(s["hi"]), " ".join(s["sample"][:12])))
