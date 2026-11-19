"""Checks a save made by test_surface.ps1 against the installed collision (same edit list, older writer).
- every pack without added polygons is byte for byte the installed one;
- every pack keeps the same triangles and barriers (corners), only surfaces may differ;
- a triangle that is also in the shipped pack keeps its shipped surface;
- no added triangle is left on the null surface (hash 0), which makes nfsw.exe reset the player car.
Usage: verify_surface.py <out folder> [installed TRACKS folder] [--extra: the list had more lines, skip the corner check]"""
import sys
from collections import Counter

import col_lib as L

out = sys.argv[1]
extra = "--extra" in sys.argv
args = [a for a in sys.argv[1:] if a != "--extra"]
tracks = args[1] if len(args) > 1 else L.TRACKS
added_packs = {int(line.split()[1]) for line in open(tracks + r"\frs-original\L5RA-edits.txt") if line.startswith("A ")}


def polygons(data):
    """Counter of (kind, corners, surface hash) of one pack, corners in world axes."""
    arts, insts = L.load(data)
    c = Counter()
    for ins in insts:
        a = arts.get(ins.article)
        if not a:
            continue
        for t in a.tris:
            corners = tuple(sorted(tuple(round(v, 4) for v in ins.to_world(p)) for p in t[:3]))
            c[("tri", corners, a.surfaces[t[3]])] += 1
        for b in a.barriers:
            corners = tuple(sorted(tuple(round(v, 4) for v in ins.to_world(p)) for p in b[:2]))
            c[("bar", corners, a.surfaces[b[2]])] += 1
    return c


def by_number(path):
    return dict(L.packs(open(path, "rb").read()))


new = by_number(out + r"\STREAML5RA_25000.BUN")
old = by_number(tracks + r"\STREAML5RA_25000.BUN")
shipped = by_number(tracks + r"\frs-original\STREAML5RA_25000.BUN")
fail = []
if set(new) != set(old):
    fail.append("pack numbers differ")
same = sum(1 for n in old if n not in added_packs and new.get(n) == old[n])
if same != sum(1 for n in old if n not in added_packs):
    fail.append("a pack without additions changed")
for n in sorted(added_packs):
    pn, po, ps = polygons(new[n]), polygons(old[n]), polygons(shipped[n])
    strip = lambda c: Counter({k[:2]: 0 for k in c}) + Counter(k[:2] for k in c.elements())
    if strip(pn) != strip(po) and not extra:
        fail.append("pack %d: corners differ" % n)
    shipped_surface = {k[:2]: k[2] for k in ps}
    moved = [k for k in pn if k[:2] in shipped_surface and shipped_surface[k[:2]] != k[2]]
    added_null = [k for k in pn.elements() if k[0] == "tri" and k[:2] not in shipped_surface and k[2] == 0]
    old_null = [k for k in po.elements() if k[0] == "tri" and k[:2] not in shipped_surface and k[2] == 0]
    shipped_null = sum(v for k, v in pn.items() if k[0] == "tri" and k[:2] in shipped_surface and k[2] == 0)
    hashes = Counter("%08x" % k[2] for k in pn.elements() if k[0] == "tri" and k[:2] not in shipped_surface)
    print("pack %d: added triangles on null surface %d (installed %d); shipped triangles still null %d; "
          "shipped surfaces changed %d; added by surface %s" % (n, len(added_null), len(old_null), shipped_null,
                                                              len(moved), dict(hashes)))
    if added_null:
        fail.append("pack %d: %d added triangles still null" % (n, len(added_null)))
    if moved:
        fail.append("pack %d: shipped triangle surface changed" % n)
print("packs unchanged:", same)
print("FAILED: " + "; ".join(fail) if fail else "PASSED")
