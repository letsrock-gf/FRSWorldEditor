"""What a NEW model needs in an NFSW section file, measured on the shipped files.
Questions: the solid list header (0x134002 fields, 0x134003 rows vs objects and their order); the order of an
object's children; where the models a section's scenery infos name are stored (same file, a library U..Y file,
another numbered file); where an object's textures live (same file's pack, elsewhere).
Usage: newmodel_probe.py <TRACKS folder> [max files]"""
import glob
import os
import struct
import sys
from collections import Counter, defaultdict

from mesh_probe import walk

T = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
ORIG = os.path.join(T, "frs-original")


def shipped(name):
    p = os.path.join(ORIG, name)
    return p if os.path.exists(p) else os.path.join(T, name)


def data(b, s, e):
    f = 0
    while s + f < e and b[s + f] == 0x11 and f < 16:
        f += 1
    return s + f


names = sorted(os.path.basename(f) for f in glob.glob(os.path.join(T, "STREAML5RA_*.BUN")))
names = [n for n in names if not n.endswith("_25000.BUN")]
names = names[:: max(1, len(names) // limit)]

st = Counter()
hdr_words = defaultdict(Counter)       # 0x134002 word index -> Counter of (value relation)
model_home = {}                         # hash -> file
tex_home = defaultdict(set)             # texture hash -> files
infos_by_file = {}                      # file -> list of info hashes (4 levels)
obj_tex = {}                            # model hash -> texture hashes
child_orders = Counter()
top_orders = Counter()
samples = []
for name in names:
    b = open(shipped(name), "rb").read()
    numbered = not name.startswith("STREAML5RA_0x")
    kind = "num" if numbered else "lib"
    top = [k for k, h, s, e in walk(b, 0, len(b)) if k != 0]
    top_orders[(kind, tuple(dict.fromkeys(top)))] += 1
    packs = 0
    for k, h, s, e in walk(b, 0, len(b)):
        if k == 0xB3300000:
            for k1, h1, s1, e1 in walk(b, s, e):
                if k1 == 0xB3310000:
                    for k2, h2, s2, e2 in walk(b, s1, e1):
                        if k2 == 0x33310002:     # hash list
                            d = data(b, s2, e2)
                            for i in range((e2 - d) // 8):
                                tex_home[struct.unpack_from("<I", b, d + 8 * i)[0]].add(name)
        if k == 0x80034100:
            for k1, h1, s1, e1 in walk(b, s, e):
                if k1 == 0x34102:
                    d = data(b, s1, e1)
                    infos_by_file.setdefault(name, [])
                    for i in range((e1 - d) // 72):
                        infos_by_file[name].append(struct.unpack_from("<4I", b, d + 72 * i + 0x18))
        if k != 0x80134000:
            continue
        packs += 1
        objs, rows, head = [], None, None
        for k1, h1, s1, e1 in walk(b, s, e):
            if k1 == 0x80134001:
                for k2, h2, s2, e2 in walk(b, s1, e1):
                    if k2 == 0x134002:
                        head = b[s2:e2]
                    if k2 == 0x134003:
                        d = data(b, s2, e2)
                        rows = [struct.unpack_from("<II", b, d + 8 * i) for i in range((e2 - d) // 8)]
            if k1 == 0x80134010:
                oh = None
                kids = []
                texs = []
                for k2, h2, s2, e2 in walk(b, s1, e1):
                    kids.append(k2)
                    if k2 == 0x134011:
                        oh = struct.unpack_from("<I", b, data(b, s2, e2) + 0x10)[0]
                    if k2 == 0x134012:
                        d = data(b, s2, e2)
                        texs = [struct.unpack_from("<I", b, d + 8 * i)[0] for i in range((e2 - d) // 8)]
                child_orders[(kind, tuple(kids))] += 1
                objs.append(oh)
                if oh is not None:
                    model_home.setdefault(oh, name)
                    obj_tex[oh] = texs
        st[f"{kind} pack 134003 rows==objects {rows is not None and len(rows) == len(objs)}"] += 1
        if rows and len(rows) == len(objs):
            st[f"{kind} rows in object order {[r[0] for r in rows] == objs}"] += 1
            st[f"{kind} rows sorted by hash {[r[0] for r in rows] == sorted(objs)}"] += 1
            st[f"{kind} row second word all 0 {all(r[1] == 0 for r in rows)}"] += 1
        if head is not None:
            st[f"{kind} 134002 size {len(head)}"] += 1
            d = 0
            while d < len(head) and head[d] == 0x11:
                d += 1
            words = struct.unpack_from(f"<{(len(head) - d) // 4}I", head, d)
            for i, w in enumerate(words[:12]):
                rel = "=objs" if w == len(objs) else ("0" if w == 0 else "other")
                hdr_words[(kind, i)][rel] += 1
            if len(samples) < 4 and len(objs) > 2:
                samples.append((name, len(objs), head.hex()))
    st[f"{kind} files with {packs} solid packs"] += 1

for k, v in sorted(st.items()):
    print(f"{v:6}  {k}")
print("-- 0x134002 words (after 0x11 filler): relation to object count")
for (kind, i), c in sorted(hdr_words.items()):
    print(f"  {kind} word {i}: {dict(c)}")
print("-- samples of 0x134002")
for s in samples:
    print("  ", s)
print("-- object child orders")
for k, v in child_orders.most_common(8):
    print(f"{v:6}  {k[0]} " + " ".join(f"{x:08X}" for x in k[1]))
print("-- top-level chunk orders (first seen order of ids)")
for k, v in top_orders.most_common(10):
    print(f"{v:6}  {k[0]} " + " ".join(f"{x:08X}" for x in k[1]))

# Where the models named by a numbered section's infos live
where = Counter()
tex_where = Counter()
for name, infos in infos_by_file.items():
    if name.startswith("STREAML5RA_0x"):
        continue
    for levels in infos:
        h = levels[0]
        if h == 0:
            where["level0 hash 0"] += 1
            continue
        home = model_home.get(h)
        if home is None:
            where["not found in the sampled files"] += 1
        elif home == name:
            where["same file"] += 1
        elif home.startswith("STREAML5RA_0x"):
            where["library (0x) file"] += 1
        else:
            where["another numbered file"] += 1
        for t in obj_tex.get(h, []):
            th = tex_home.get(t, set())
            if name in th and home == name:
                tex_where["model here, texture in same file"] += 1
            elif home and home in th:
                tex_where["texture in model's file"] += 1
            elif any(x.startswith("STREAML5RA_0x") for x in th):
                tex_where["texture in a library file"] += 1
            elif th:
                tex_where["texture in another numbered file"] += 1
            else:
                tex_where["texture not found"] += 1
print("-- numbered sections: where the level-0 model of each info is stored")
for k, v in where.most_common():
    print(f"{v:8}  {k}")
print("-- their textures")
for k, v in tex_where.most_common():
    print(f"{v:8}  {k}")

# Is the level-0 model of a numbered section's info stored in that same file, and are hashes stored twice?
own = defaultdict(set)
for name in names:
    b = open(shipped(name), "rb").read()
    for k, h, s, e in walk(b, 0, len(b)):
        if k == 0x80134000:
            for k1, h1, s1, e1 in walk(b, s, e):
                if k1 == 0x80134010:
                    for k2, h2, s2, e2 in walk(b, s1, e1):
                        if k2 == 0x134011:
                            own[name].add(struct.unpack_from("<I", b, data(b, s2, e2) + 0x10)[0])
files_of = defaultdict(set)
for name, hs in own.items():
    for x in hs:
        files_of[x].add(name)
stored = Counter()
for name, infos in infos_by_file.items():
    if name.startswith("STREAML5RA_0x"):
        continue
    for levels in infos:
        if levels[0]:
            stored["in own file" if levels[0] in own[name] else
                   ("only in library files" if files_of.get(levels[0]) and all(f.startswith("STREAML5RA_0x") for f in files_of[levels[0]])
                    else ("in another numbered file" if files_of.get(levels[0]) else "nowhere"))] += 1
print("-- numbered infos, level 0 model:", dict(stored))
dup = Counter()
for x, fs in files_of.items():
    if len(fs) > 1:
        dup["num+lib" if any(f.startswith("STREAML5RA_0x") for f in fs) and any(not f.startswith("STREAML5RA_0x") for f in fs)
            else ("lib only" if all(f.startswith("STREAML5RA_0x") for f in fs) else "num only")] += 1
print("-- model hashes stored in more than one file:", dict(dup), "of", len(files_of))
