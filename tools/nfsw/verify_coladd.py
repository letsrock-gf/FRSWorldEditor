"""Checks a collision save that ADDED polygons (scratch folder, FRS_WRITE_TRACKS) against the shipped file.

Usage: verify_coladd.py <scratch folder>

With the edit list's `C` (deleted) and `A` (added) lines as what was asked for:
  - the written STREAML5RA_25000.BUN parses as a chunk chain to its last byte, same ids in the same order, and
    grew by a multiple of 0x100 (or not at all);
  - every pack the list does not name is byte-identical;
  - a named pack keeps every shipped polygon the list does not delete, and holds each added triangle (within the
    1/128 the format stores) or it is reported as left out; added barriers are counted;
  - the kept copy is the shipped file;
  - the section table written beside it (L5RA.BUN) says the file's new size.
"""
import os
import struct
import sys
from collections import Counter

import col_lib as c

NAME = "STREAML5RA_25000.BUN"
fails = 0


def check(ok, text):
    global fails
    fails += not ok
    print(("ok    " if ok else "FAIL  ") + text)


def shapes(data):
    """Triangles (three game-space points, sorted) and barriers of a pack."""
    arts, insts = c.load(data)
    tris, bars = [], []
    for inst in insts:
        art = arts[inst.article]
        for tri in art.tris:
            tris.append(tuple(sorted(c.game(inst.to_world(p)) for p in tri[:3])))
        for p0, p1, surface, flags in art.barriers:
            bars.append((c.game(inst.to_world(p0)), c.game(inst.to_world(p1))))
    return tris, bars


def close(a, b, tol=0.02):
    return all(abs(x - y) <= tol for p, q in zip(a, b) for x, y in zip(p, q))


def main():
    folder = sys.argv[1]
    # The game's own copy, kept by the editor once Gui saved into it; else the live file.
    kept_shipped = os.path.join(c.TRACKS, "frs-original", NAME)
    shipped = open(kept_shipped if os.path.exists(kept_shipped) else os.path.join(c.TRACKS, NAME), "rb").read()
    written = open(os.path.join(folder, NAME), "rb").read()
    check(open(os.path.join(folder, "frs-original", NAME), "rb").read() == shipped, "kept copy is the shipped file")

    cuts, adds = {}, {}
    for line in open(os.path.join(folder, "frs-original", "L5RA-edits.txt")):
        w = line.split()
        if w[0] == "C":
            cuts.setdefault(int(w[1]), set()).update(int(x) for x in w[2:])
        elif w[0] == "A":
            f = [float(x) for x in w[11:23]]
            adds.setdefault(int(w[1]), []).append((int(w[2]), int(w[3]), int(w[5]),
                                                   [tuple(f[i:i + 3]) for i in range(0, 12, 3)]))
    print("edit list: deleted", {k: len(v) for k, v in cuts.items()}, "added", {k: len(v) for k, v in adds.items()})

    grown = len(written) - len(shipped)
    check(grown >= 0 and grown % 0x100 == 0, "file grew by %d bytes, a multiple of 0x100" % grown)
    chain_a, chain_b = list(c.chunks(shipped)), list(c.chunks(written))
    check([x[0] for x in chain_a] == [x[0] for x in chain_b], "same chunk ids in the same order (%d)" % len(chain_b))
    last = chain_b[-1]
    check(last[1] + last[2] == len(written), "the chunk chain ends on the file's last byte")
    check(all((off - 8) % 16 == 0 for cid, off, size in chain_b if cid == 0x3B801),
          "collision chunks still start on a multiple of 16")

    named = set(cuts) | set(adds)
    walls_total = [0]
    same = True
    for (ca, oa, sa), (cb, ob, sb) in zip(chain_a, chain_b):
        if ca == 0x3B802:
            # The collision grid: the save adds the instances that got polygons (nfs::AddToCollisionGrid).
            print("      grid 0x3B802: %s" % ("changed" if shipped[oa:oa + sa] != written[ob:ob + sb] else "unchanged"))
            continue
        if ca != 0x3B801:
            same = same and shipped[oa:oa + sa] == written[ob:ob + sb]
            continue
        a, b = shipped[oa:oa + sa], written[ob:ob + sb]
        number = struct.unpack_from("<I", a, 12)[0]
        if number not in named:
            same = same and a == b
            continue
        check(len(b) >= len(a) and (len(b) - len(a)) % 0x100 == 0 and
              struct.unpack_from("<I", b, 8)[0] == len(b) - 24,
              "pack %d: %d -> %d bytes, size field agrees" % (number, len(a), len(b)))
        ta, ba = shapes(a)
        tb, bb = shapes(b)
        # Shipped polygons the list keeps are all still there.
        arts, insts = c.load(a)
        order = []
        for inst in insts:
            art = arts[inst.article]
            order += [("T", tuple(sorted(c.game(inst.to_world(p)) for p in tri[:3]))) for tri in art.tris]
            order += [("B", (c.game(inst.to_world(p0)), c.game(inst.to_world(p1)))) for p0, p1, s, f in art.barriers]
        kept = Counter(x for i, x in enumerate(order) if i not in cuts.get(number, ()))
        have = Counter([("T", t) for t in tb] + [("B", x) for x in bb])
        check(not (kept - have), "pack %d: every shipped polygon the list keeps is there (%d)" % (number, sum(kept.values())))
        extra = have - kept
        new_tris = [x[1] for x in extra.elements() if x[0] == "T"]
        new_bars = [x[1] for x in extra.elements() if x[0] == "B"]
        found = missing = wanted_bars = 0
        walls_found = walls_missing = 0
        for barrier, quad, source, corners in adds.get(number, []):
            if barrier:
                wanted_bars += 1
                continue
            # A wall-like face (|normal z| <= 0.6) has to come out as a barrier record from its lowest to its
            # highest corner between the two corners furthest apart in plan (the game tests triangles in plan only).
            u = [corners[1][i] - corners[0][i] for i in range(3)]
            v = [corners[2][i] - corners[0][i] for i in range(3)]
            n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            length = sum(x * x for x in n) ** 0.5
            if length < 1e-9 or abs(n[2]) / length <= 0.6:
                pts = corners[:4] if quad else corners[:3]
                pair = max(((a, b) for i, a in enumerate(pts) for b in pts[i + 1:]),
                           key=lambda ab: (ab[0][0] - ab[1][0]) ** 2 + (ab[0][1] - ab[1][1]) ** 2)
                lo, hi = min(p[2] for p in pts), max(p[2] for p in pts)
                hit = False
                for p0, p1 in new_bars:
                    ends = sorted([(p0[0], p0[1]), (p1[0], p1[1])])
                    want = sorted([(pair[0][0], pair[0][1]), (pair[1][0], pair[1][1])])
                    if close(ends, want, 0.05) and abs(min(p0[2], p1[2]) - lo) < 0.05 and abs(max(p0[2], p1[2]) - hi) < 0.05:
                        hit = True
                walls_found += hit
                walls_missing += not hit
                continue
            for tri in ([corners[0:3]] + ([[corners[0], corners[2], corners[3]]] if quad else [])):
                want = tuple(sorted(tri))
                if any(close(want, got) for got in new_tris):
                    found += 1
                else:
                    missing += 1
        print("      pack %d: added triangles written %d, left out %d; new triangles in file %d; "
              "barrier records in file %d for %d added barriers" % (number, found, missing, len(new_tris),
                                                                    len(new_bars), wanted_bars))
        # Ground wider than one box is cut along the boxes (pieces of an added triangle): every new triangle has to
        # lie inside, and on the plane of, one of the added ground faces.
        planes = []
        for barrier, quad, source, corners in adds.get(number, []):
            if not barrier:
                planes += [corners[0:3]] + ([[corners[0], corners[2], corners[3]]] if quad else [])

        def inside(tri):
            cx, cy, cz = (sum(p[i] for p in tri) / 3.0 for i in range(3))
            for a, b, cc in planes:
                d = (b[1] - cc[1]) * (a[0] - cc[0]) + (cc[0] - b[0]) * (a[1] - cc[1])
                if abs(d) < 1e-9:
                    continue
                l1 = ((b[1] - cc[1]) * (cx - cc[0]) + (cc[0] - b[0]) * (cy - cc[1])) / d
                l2 = ((cc[1] - a[1]) * (cx - cc[0]) + (a[0] - cc[0]) * (cy - cc[1])) / d
                l3 = 1 - l1 - l2
                if min(l1, l2, l3) >= -0.01 and abs(l1 * a[2] + l2 * b[2] + l3 * cc[2] - cz) < 0.05:
                    return True
            return False
        stray = [t for t in new_tris if not inside(t)]
        check(not stray, "pack %d: every new triangle is (a piece of) an added ground face (%d new)" % (number, len(new_tris)))
        if walls_found or walls_missing:
            print("      pack %d: wall faces written as barriers %d, not found %d" % (number, walls_found, walls_missing))
            walls_total[0] += walls_found
        check(not any(abs(t[0][2] - t[2][2]) > 0.05 and
                      abs(((t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[1][1] - t[0][1]) * (t[2][0] - t[0][0]))) < 1e-3
                      for t in new_tris), "pack %d: no upright triangle written (the game cannot hit one)" % number)
    check(same, "everything outside the named packs and the grid is byte-identical")
    # The editor keeps a polygon near a pack's edge in both packs; a barrier needs one box of its pack, so a copy
    # whose pack has none is left out while the other copy is written.
    check(walls_total[0] > 0, "wall faces written as barrier records: %d" % walls_total[0])

    table = os.path.join(folder, "L5RA.BUN")
    if grown:
        check(os.path.exists(table), "L5RA.BUN written beside it (the file changed size)")
    if os.path.exists(table):
        buf = open(table, "rb").read()
        for cid, off, size in c.chunks(buf):
            if cid != 0x34110:
                continue
            for pos in range(off, off + size - 207, 208):
                if struct.unpack_from("<I", buf, pos + 8)[0] == 25000:
                    count = struct.unpack_from("<I", buf, pos + 0x5C)[0]
                    triples = [struct.unpack_from("<3I", buf, pos + 0x60 + 12 * i) for i in range(max(1, min(count, 8)))]
                    print("      table entry Z0: %d parts %s" % (count, triples))
                    check(triples[-1][0] == len(written), "table: the last part ends at the new file size")
    print("FAILED (%d)" % fails if fails else "PASSED")


main()
