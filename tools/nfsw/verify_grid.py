"""Checks the collision grid (chunk 0x3B802) of a save against the shipped one.
- records parse by the game's rule (nfsw.exe 0x6d8d20: 0x14 + 4 * the four counts), offsets = 4 * counts before;
- every shipped cell is still there with all its shipped entries, in order, and its other lists untouched;
- for every added ground triangle, the cell the game looks a point up in (0x6cff40: round((x - origin) / 96))
  lists the instance holding it, at the triangle's centre and corners.
Usage: verify_grid.py <out folder>"""
import struct
import sys
from collections import Counter

import col_lib as L

ORIGIN_X, ORIGIN_Z, CELL, COLUMNS, ROWS = -5088.0, 0.0, 96.0, 80, 115


def grid(path):
    b = open(path, "rb").read()
    off, size = next((o, s) for cid, o, s in L.chunks(b) if cid == 0x3B802)
    d = b[off:off + size]
    tag, info, count, rel = struct.unpack_from("<4I", d, 56)
    assert tag == 0x4347636E
    p, end, cells = 56 + rel, 56 + rel + (info >> 8), {}
    for _ in range(count):
        counts = d[p + 8:p + 12]
        offsets = struct.unpack_from("<4H", d, p + 12)
        assert list(offsets) == [4 * sum(counts[:k]) for k in range(4)], "offsets"
        index = struct.unpack_from("<H", d, p + 4)[0]
        lists = [list(struct.unpack_from("<%dI" % counts[k], d, p + 0x14 + offsets[k])) for k in range(4)]
        cells[index] = lists
        p += 0x14 + 4 * sum(counts)
    assert p == end, "records end %d, row %d" % (p, end)
    name_rel = struct.unpack_from("<I", d, 72 + 12)[0]
    assert d[72 + name_rel:72 + name_rel + 6] == b"I25000", "Name row moved wrong"
    assert struct.unpack_from("<I", d, 8)[0] == size - 24, "chunk size field"
    return cells


out = sys.argv[1]
new = grid(out + r"\STREAML5RA_25000.BUN")
old = grid(L.TRACKS + r"\frs-original\STREAML5RA_25000.BUN")
fail = []
for index, lists in old.items():
    n = new.get(index)
    if n is None or n[0][:len(lists[0])] != lists[0] or n[1:] != lists[1:]:
        fail.append("cell %d lost shipped entries" % index)
print("cells shipped %d, now %d; entries shipped %d, now %d" % (
    len(old), len(new), sum(len(v[0]) for v in old.values()), sum(len(v[0]) for v in new.values())))

shipped = {}
for n, data in L.packs(open(L.TRACKS + r"\frs-original\STREAML5RA_25000.BUN", "rb").read()):
    arts, insts = L.load(data)
    shipped[n] = Counter(tuple(sorted(tuple(round(v, 3) for v in ins.to_world(p)) for p in t[:3]))
                         for ins in insts if ins.article in arts for t in arts[ins.article].tris)
checked = missing = 0
for n, data in L.packs(open(out + r"\STREAML5RA_25000.BUN", "rb").read()):
    arts, insts = L.load(data)
    for k, ins in enumerate(insts):
        a = arts.get(ins.article)
        if not a:
            continue
        for t in a.tris:
            w = [ins.to_world(p) for p in t[:3]]
            if tuple(sorted(tuple(round(v, 3) for v in p) for p in w)) in shipped.get(n, ()):
                continue
            centre = tuple(sum(p[i] for p in w) / 3 for i in range(3))
            for p in w + [centre]:   # collision axes: x, up, z
                cell = round((p[2] - ORIGIN_Z) / CELL) * COLUMNS + round((p[0] - ORIGIN_X) / CELL)
                checked += 1
                if (n << 16 | k) not in new.get(cell, [[]])[0]:
                    missing += 1
                    if missing <= 5:
                        print("  not listed: pack %d instance %d at cell %d" % (n, k, cell))
print("added triangle points checked %d, cell without its instance %d" % (checked, missing))
if missing:
    fail.append("%d points in cells that do not list their instance" % missing)
print("FAILED: " + "; ".join(fail[:5]) if fail else "PASSED")
