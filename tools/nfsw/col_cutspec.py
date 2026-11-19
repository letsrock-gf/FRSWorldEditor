"""Prints a FRS_TEST_COLLISION_CUT spec that exercises every case of the collision writer.

Polygons are numbered the way the editor's parser lists a pack: per instance, the article's strip triangles in
order, then its barriers. Cases: one triangle in the middle of a strip (the strip is cut in two, the second half
starting on an odd triangle), the first and the last triangle of a strip, a whole strip, some barriers, a strip
whose winding byte is 1 cut on an odd triangle, and every polygon of one small article.
"""
import col_lib as c


def polygons(arts, insts):
    """(source index, article, 'T'/'B', strip, triangle in strip, vertices in strip, winding) in parser order."""
    out = []
    for inst in insts:
        art = arts[inst.article]
        per_strip = {}
        for strip, k, winding in art.tri_meta:
            per_strip[strip] = per_strip.get(strip, 0) + 1
        for strip, k, winding in art.tri_meta:
            out.append((len(out), inst.article, "T", strip, k, per_strip[strip], winding))
        for b in range(len(art.barriers)):
            out.append((len(out), inst.article, "B", b, 0, 0, 0))
    return out


def main():
    buf = open(c.PACKS, "rb").read()
    packs = dict(c.packs(buf))
    spec = []

    arts, insts = c.load(packs[100])
    listed = polygons(arts, insts)
    cuts = set()
    want = {"middle": None, "ends": None, "whole": None, "flipped": None}
    for src, article, kind, strip, k, count, winding in listed:
        if kind != "T":
            continue
        key = (article, strip)
        if want["middle"] is None and count >= 9 and winding == 0 and k == 3:
            want["middle"] = key
            cuts.add(src)
        elif want["flipped"] is None and count >= 9 and winding == 1 and k == 2 and key != want["middle"]:
            want["flipped"] = key
            cuts.add(src)
        elif want["ends"] is None and count >= 6 and k == 0 and key not in want.values():
            want["ends"] = key
            cuts.add(src)
            cuts.add(src + count - 1)
        elif want["whole"] is None and 3 <= count <= 5 and k == 0 and key not in want.values():
            want["whole"] = key
            cuts.update(range(src, src + count))
    spec.append("100:" + ",".join(str(x) for x in sorted(cuts)))

    arts, insts = c.load(packs[3061])   # one article of six barriers
    listed = polygons(arts, insts)
    spec.append("3061:" + ",".join(str(src) for src, a, kind, *_ in listed if kind == "B" and src % 2 == 0))

    # Every polygon of one small article, in the first other pack that has one: it is left empty.
    for number in sorted(packs):
        if number in (100, 3061):
            continue
        arts, insts = c.load(packs[number])
        small = [a for a in arts if 0 < len(arts[a].tris) <= 3 and not arts[a].barriers]
        if small:
            spec.append("%d:" % number + ",".join(str(src) for src, a, *_ in polygons(arts, insts) if a == small[0]))
            break
    print(";".join(spec))


main()
