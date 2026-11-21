"""Checks a Roads > Bake written to NFS World (test_roadbake.ps1), at the edit list's level; the files themselves are
checked by verify_newmodel.py, verify_coladd.py, verify_grid.py and verify_table.py.
  baked:  the list names new models (N) whose kept meshes are road pieces (o ROAD_*) and places each (P); it adds
          collision (A) whose four attribute bytes are an NFS World surface hash (asphalt, concrete, grass), asphalt
          among them; it makes no new texture (X): every road texture is one the map already has.
  --undone: (Ctrl+Z) no N, A or X line is left, and every section file in the folder is byte for byte the shipped one.
  --unbaked: (Roads > Remove baked roads) no N, X or made A line is left; the ground fitted under the road stays,
          so the collision file and L5RA.BUN may differ, every other section file is the shipped one.
Usage: verify_roadbake.py <written TRACKS folder> <game TRACKS> [--undone | --unbaked]"""
import os
import sys

out, tracks = sys.argv[1], sys.argv[2]
undone = "--undone" in sys.argv
unbaked = "--unbaked" in sys.argv
SURFACES = {0x19DB2F1E: "asphalt", 0x6CA26F9B: "concrete", 0x772FB736: "grass", 0xC1C577D6: "gravel"}
ok_all = True


def check(ok, text):
    global ok_all
    ok_all &= bool(ok)
    print(("   ok   " if ok else "   FAIL ") + text)


def shipped(name):
    p = os.path.join(tracks, "frs-original", name)
    return p if os.path.exists(p) else os.path.join(tracks, name)


path = os.path.join(out, "frs-original", "L5RA-edits.txt")
lines = open(path).read().splitlines() if os.path.exists(path) else []
kinds = {}
for line in lines:
    kinds.setdefault(line[:1], []).append(line.split())
n, p, a, x = (kinds.get(k, []) for k in "NPAX")
print(f"list: {len(lines)} lines, N {len(n)}, P {len(p)}, A {len(a)}, X {len(x)}")

if undone or unbaked:
    made = [f for f in a if f[5] == "-1"]
    if undone:
        check(not n and not a and not x, "no new model, collision or texture left in the list")
    else:
        # Remove baked roads keeps the ground fitted under the road (as on Underground 2): shipped polygons
        # moved (A with a source) and cut (C) may stay, nothing the bake made.
        check(not n and not made and not x, f"no new model, made collision or texture left in the list "
              f"({len(a) - len(made)} moved shipped polygons and {len(kinds.get('C', []))} C lines kept: ground fit)")
    files = [f for f in os.listdir(out) if f.upper().endswith(".BUN")]
    if unbaked and (a or kinds.get("C")):
        files = [f for f in files if f.upper() not in ("STREAML5RA_25000.BUN", "L5RA.BUN")]
    same = [f for f in files if open(os.path.join(out, f), "rb").read() == open(shipped(f), "rb").read()]
    check(len(same) == len(files), f"{len(same)} of {len(files)} written .BUN files are the shipped ones")
else:
    check(len(n) > 0, f"{len(n)} new models")
    placed = {f[3].upper() for f in p if len(f) > 3}
    names = []
    for f in n:
        kept = os.path.join(out, "frs-original", "meshes", f[3]) if len(f) > 3 else ""
        name = ""
        if os.path.exists(kept):
            name = next((l.split()[1] for l in open(kept) if l.startswith("o ")), "")
        names.append(name)
        check(name.upper().startswith(("ROAD_", "FRS_ROAD_")), f"N {f[2]} kept as {f[3] if len(f) > 3 else '?'}: o {name}")
        check(f[2].upper() in placed, f"N {f[2]} is placed")
    surfaces = {}
    moved = [f for f in a if f[5] != "-1"]   # shipped ground moved under the road keeps its own surface
    a = [f for f in a if f[5] == "-1"]
    print(f"   {len(moved)} shipped polygons moved, {len(a)} made")
    for f in a:
        b = [int(v) for v in f[6:10]]
        h = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24
        surfaces[SURFACES.get(h, "%08X" % h)] = surfaces.get(SURFACES.get(h, "%08X" % h), 0) + 1
    print("   collision surfaces:", surfaces)
    check(len(a) > 0, f"{len(a)} collision polygons added")
    check(set(surfaces) <= set(SURFACES.values()), "every added polygon names an NFS World surface")
    check(surfaces.get("asphalt", 0) > 0, "the road itself is asphalt")
    check(not x, "no new texture: every road texture is the map's own")
print("ALL PASS" if ok_all else "SOME FAILED")
sys.exit(0 if ok_all else 1)
