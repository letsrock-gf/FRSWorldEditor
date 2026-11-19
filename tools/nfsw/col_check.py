"""Checks the decoded collision of all packs: article links, local bounds against the instance box, the two
candidate world transforms against the section centres, triangle winding."""
import math
from collections import Counter

import col_lib as c


def main():
    buf = open(c.PACKS, "rb").read()
    centres = c.section_centres()
    stat = Counter()
    dist = {"R^T*local - t": [], "R^T*(local - t)": []}
    worst = []
    for number, data in c.packs(buf):
        arts, insts = c.load(data)
        stat["packs"] += 1
        stat[("section in table", number in centres)] += 1
        stat[("articles == rows", len(arts) == max(arts) + 1 if arts else True)] += 1
        used = set()
        for inst in insts:
            stat["instances"] += 1
            stat[("article index valid", inst.article in arts)] += 1
            stat[("pad", inst.pad)] += 1
            stat[("group", inst.group)] += 1
            stat[("flags %04X" % inst.flags)] += 0  # listed below only when few
            used.add(inst.article)
            art = arts.get(inst.article)
            if not art:
                continue
            pts = list(art.points())
            if pts:
                ext = [max(abs(p[i]) for p in pts) for i in range(3)]
                box = (inst.width, inst.height, inst.length)
                ok = all(abs(ext[i] - box[i]) <= 0.02 * max(1.0, box[i]) + 0.07 for i in range(3))
                stat[("local extent == width/height/length", ok)] += 1
                if not ok and len(worst) < 5:
                    worst.append((number, inst.article, ext, box))
            r0, r1, r2 = inst.row0, inst.row1(), inst.row2
            stat[("rows unit", abs(math.dist(r0, (0, 0, 0)) - 1) < 0.01 and abs(math.dist(r2, (0, 0, 0)) - 1) < 0.01)] += 1
            if number in centres:
                a = c.game(inst.to_world((0, 0, 0)))
                t = inst.inv_pos
                alt = c.game(tuple(-(r0[i] * t[0] + r1[i] * t[1] + r2[i] * t[2]) for i in range(3)))
                rotated = abs(r0[0] - 1) > 0.01
                if rotated:
                    dist["R^T*local - t"].append(math.dist(a[:2], centres[number][:2]))
                    dist["R^T*(local - t)"].append(math.dist(alt[:2], centres[number][:2]))
                stat[("rotated", rotated)] += 1
            for tri in art.tris:
                (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri[:3]
                ny = (bz - az) * (cx - ax) - (bx - ax) * (cz - az)
                stat[("tri normal up", ny > 0)] += 1
        stat[("articles unused", len(set(arts) - used))] += 1
    for key, n in sorted(stat.items(), key=lambda kv: str(kv[0])):
        if n:
            print(key, n)
    for name, values in dist.items():
        values.sort()
        print("distance to section centre, %s: median %.1f p90 %.1f max %.1f (%d)" % (
            name, values[len(values) // 2], values[len(values) * 9 // 10], values[-1], len(values)))
    for item in worst:
        print("extent mismatch", item)


main()
