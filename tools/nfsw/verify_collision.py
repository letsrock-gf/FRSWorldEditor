"""Checks a collision save made into a scratch folder (FRS_WRITE_TRACKS) against the shipped file.

Usage: verify_collision.py <scratch folder> [--restored]

With the edit list's `C` lines as what was asked for:
  - the written STREAML5RA_25000.BUN has the shipped size and chunk chain (ids and sizes in order);
  - every byte outside the packs named by the list is the shipped byte;
  - every named pack parses, keeps its size, its rows and its instances byte for byte, and its polygons are the
    shipped ones less the listed ones: same vertices, surface hash and flags, same winding;
  - articles of a named pack that lost nothing are byte-identical;
  - the kept copy in frs-original is the shipped file, and L5RA.BUN was not written.
--restored: after undoing everything and saving again the folder must hold the shipped file again (or none) and
no kept copy, no edit list.
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


def oriented(tri, winding):
    """The triangle as the game winds it, rotated to start at its smallest vertex; unordered when unknown."""
    a, b, cc = tri[:3]
    if winding == 2:
        return ("both",) + tuple(sorted((a, b, cc)))
    if winding == 1:
        a, b = b, a
    turn = min(range(3), key=lambda i: (a, b, cc)[i])
    pts = (a, b, cc)
    return ("one",) + tuple(pts[(turn + i) % 3] for i in range(3))


def listed(data):
    """Every polygon of a pack in the parser's order, as something comparable."""
    arts, insts = c.load(data)
    out = []
    for inst in insts:
        art = arts[inst.article]
        for tri, (strip, k, winding) in zip(art.tris, art.tri_meta):
            shape = oriented([inst.to_world(p) for p in tri[:3]], winding)
            out.append(("T", shape, art.surfaces[tri[3]] if tri[3] < len(art.surfaces) else None, tri[4]))
        for p0, p1, surface, flags in art.barriers:
            out.append(("B", inst.to_world(p0), inst.to_world(p1),
                        art.surfaces[surface] if surface < len(art.surfaces) else None, flags))
    return out


def main():
    folder = sys.argv[1]
    # The game's kept copy once the live file holds edits of its own.
    original = os.path.join(c.TRACKS, "frs-original", NAME)
    shipped = open(original if os.path.exists(original) else os.path.join(c.TRACKS, NAME), "rb").read()
    written_path = os.path.join(folder, NAME)
    kept_path = os.path.join(folder, "frs-original", NAME)
    list_path = os.path.join(folder, "frs-original", "L5RA-edits.txt")

    if "--restored" in sys.argv:
        check(not os.path.exists(kept_path), "no kept copy left")
        check(not os.path.exists(list_path), "no edit list left")
        check(not os.path.exists(written_path) or open(written_path, "rb").read() == shipped,
              "the section file is the shipped one again")
        print("FAILED" if fails else "PASSED")
        return

    cuts = {}
    for line in open(list_path):
        if line.startswith("C "):
            words = line.split()
            cuts.setdefault(int(words[1]), set()).update(int(w) for w in words[2:])
    print("edit list: %d packs, %d polygons" % (len(cuts), sum(len(v) for v in cuts.values())))
    check(bool(cuts), "the edit list names deleted collision polygons")

    written = open(written_path, "rb").read()
    check(len(written) == len(shipped), "file size unchanged (%d)" % len(written))
    check(open(kept_path, "rb").read() == shipped, "kept copy is the shipped file")
    # The section is streamed in parts, so the table is written with where each ends: the last has to be the
    # file's size, which did not change.
    table = os.path.join(folder, "L5RA.BUN")
    if os.path.exists(table):
        buf = open(table, "rb").read()
        for cid, off, size in c.chunks(buf):
            if cid == 0x34110:
                for pos in range(off, off + size - 207, 208):
                    if struct.unpack_from("<I", buf, pos + 8)[0] == 25000:
                        count = max(1, min(struct.unpack_from("<I", buf, pos + 0x5C)[0], 8))
                        check(struct.unpack_from("<I", buf, pos + 0x60 + 12 * (count - 1))[0] == len(written),
                              "section table: the last part of Z0 ends at the file size")

    chain_a = [(cid, off, size) for cid, off, size in c.chunks(shipped)]
    chain_b = [(cid, off, size) for cid, off, size in c.chunks(written)]
    check(chain_a == chain_b, "chunk chain identical (%d chunks)" % len(chain_a))

    touched = []      # (offset, size) of the packs the list names
    outside = True
    cursor = 0
    stats = Counter()
    for cid, off, size in chain_a:
        if cid != 0x3B801:
            continue
        a, b = shipped[off:off + size], written[off:off + size]
        number = struct.unpack_from("<I", a, 12)[0]
        if number not in cuts:
            continue
        touched.append((off, size))
        outside = outside and shipped[cursor:off] == written[cursor:off]
        cursor = off + size

        check(a[:0x38] == b[:0x38], "pack %d: header and group rows unchanged" % number)
        rows_a, rows_b = c.rows(a), c.rows(b)
        check([(t, k, n) for t, s, k, n, p in rows_a] == [(t, k, n) for t, s, k, n, p in rows_b],
              "pack %d: same rows (tags, kinds, counts)" % number)
        inst_a = next((p, s) for t, s, k, n, p in rows_a if t >> 16 == 0x6369)
        inst_b = next((p, s) for t, s, k, n, p in rows_b if t >> 16 == 0x6369)
        check(a[inst_a[0]:inst_a[0] + inst_a[1]] == b[inst_b[0]:inst_b[0] + inst_b[1]],
              "pack %d: instances byte-identical" % number)
        end = 0
        for (t, s, k, n, p) in rows_b:
            check(p % 16 == 8 and p + s <= len(b), "pack %d: row %08X body aligned and inside the pack" % (number, t)) \
                if p % 16 != 8 or p + s > len(b) else None
            end = max(end, p + ((s + 15) & ~15))
        check(all(x == 0xAA for x in b[end:]), "pack %d: 0xAA after the last body" % number)

        before, after = listed(a), listed(b)
        expected = [p for i, p in enumerate(before) if i not in cuts[number]]
        check(max(cuts[number]) < len(before), "pack %d: listed indices exist (%d polygons shipped)" % (number, len(before)))
        check(Counter(expected) == Counter(after),
              "pack %d: polygons = shipped less the %d listed (%d left)" % (number, len(cuts[number]), len(after)))
        stats["winding unknown (2) among kept"] += sum(1 for p in after if p[0] == "T" and p[1][0] == "both")

        # Articles: unchanged ones byte for byte, changed ones by their counts.
        arts_a = {t & 0xFFFF: a[p:p + s] for t, s, k, n, p in rows_a if t >> 16 == 0x6361}
        arts_b = {t & 0xFFFF: b[p:p + s] for t, s, k, n, p in rows_b if t >> 16 == 0x6361}
        changed = [i for i in arts_a if arts_a[i] != arts_b.get(i)]
        la, ia = c.load(a)
        first = {}
        n = 0
        for inst in ia:
            first[inst.article] = n
            n += len(la[inst.article].tris) + len(la[inst.article].barriers)
        hit = set()
        for inst in ia:
            size_ = len(la[inst.article].tris) + len(la[inst.article].barriers)
            if any(first[inst.article] <= x < first[inst.article] + size_ for x in cuts[number]):
                hit.add(inst.article)
        check(set(changed) == hit, "pack %d: only the %d articles that lost polygons changed" % (number, len(hit)))
        for i in sorted(hit):
            art = arts_b[i]
            strips, sb, edges, eb, b8, surfs, sfb, fl = struct.unpack_from("<4H2BHI", art, 0)
            used = 16 + sb + eb + sfb
            ok = (art[8:16] == arts_a[i][8:16] and eb == 32 * edges and len(art) == (used + 63) & ~63
                  and not any(art[used:]) and sb % 16 == 0)
            stats["articles rewritten"] += 1
            stats["articles left empty"] += strips == 0 and edges == 0
            stats["bytes %+d" % (len(art) - len(arts_a[i]))] += 1
            check(ok, "pack %d article %d: header tail kept, sizes consistent (%d strips, %d barriers, %d bytes)"
                  % (number, i, strips, edges, len(art)))
    outside = outside and shipped[cursor:] == written[cursor:]
    check(outside, "every byte outside the %d named packs is the shipped byte" % len(touched))
    for key, n in sorted(stats.items()):
        print("      %s: %d" % (key, n))
    print("FAILED (%d)" % fails if fails else "PASSED")


main()
