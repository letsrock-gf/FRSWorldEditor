"""Checks section files written with NEW models (and models copied in), the way nfsw.exe reads them:
solid list 0x749C80 (count 0x134002 +0x0C, rows 0x134003 searched by 0x747700), object 0x74D550, mesh 0x64D5E0,
scenery 0x746060 / tree 0x73DB90, section table entry sizes.
Every chunk that is not a solid list or a scenery container must be the shipped one; every shipped object and record
must still be there unchanged; whatever is added must be accounted for by the edit list (N lines and placements).
Usage: verify_newmodel.py <written folder> <game TRACKS> [--shipped] [--same-as <other written folder>]
  --shipped  the folder must hold the section files as shipped again (everything taken back)"""
import os
import struct
import sys

out, tracks = sys.argv[1], sys.argv[2]
expect_shipped = "--shipped" in sys.argv
same_as = sys.argv[sys.argv.index("--same-as") + 1] if "--same-as" in sys.argv else None
LOADER_ALIGNED = {0x134011, 0x134900, 0x134B01, 0x134B02, 0x134B03, 0x134C05, 0x134C06, 0x13401A}   # (p+0x17)&~0xF
SCENERY_ALIGNED = {0x34101, 0x34103, 0x34105, 0x3410A, 0x3410E, 0x3410F, 0x34C03}
STRIDE = {0: 36, 1: 36, 0xA: 36, 0xC: 36, 0xF: 36, 2: 44, 3: 44, 7: 44, 0xB: 44, 4: 28, 5: 36}
ok_all = True


def check(ok, text):
    global ok_all
    ok_all &= bool(ok)
    print(("   ok   " if ok else "   FAIL ") + text)
    return ok


def shipped(name):
    p = os.path.join(tracks, "frs-original", name)
    return p if os.path.exists(p) else os.path.join(tracks, name)


def chunks(b, s, e):
    p, res = s, []
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            return res, False
        res.append((k, p, p + 8 + n))
        p += 8 + n
    return res, p == e


def data_of(b, k, s, e, aligned):
    if k in aligned:
        d = (s + 8 + 0xF) & ~0xF     # file offsets: the files are loaded 128-aligned
        return d, all(x == 0x11 for x in b[s + 8:d])
    return s + 8, True


def canon(b, k, s, e):
    """An object or container as the game reads it: ids and payloads, fillers dropped."""
    if k & 0x80000000:
        kids, _ = chunks(b, s + 8, e)
        return (k, tuple(canon(b, *c) for c in kids if c[0] != 0))
    d, _ = data_of(b, k, s, e, LOADER_ALIGNED)
    return (k, bytes(b[d:e]))


def bhash(name):
    h = 0xFFFFFFFF
    for c in name.encode("latin1"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def pack_objects(b, s, e):
    kids, whole = chunks(b, s + 8, e)
    objs, count, rows = [], None, None
    for k, cs, ce in kids:
        if k == 0x80134001:
            for k2, s2, e2 in chunks(b, cs + 8, ce)[0]:
                if k2 == 0x134002:
                    count = struct.unpack_from("<I", b, s2 + 8 + 0x0C)[0]
                if k2 == 0x134003:
                    rows = [struct.unpack_from("<I", b, s2 + 8 + 8 * i)[0] for i in range((e2 - s2 - 8) // 8)]
        if k == 0x80134010:
            h = None
            for k2, s2, e2 in chunks(b, cs + 8, ce)[0]:
                if k2 == 0x134011:
                    d, _ = data_of(b, k2, s2, e2, LOADER_ALIGNED)
                    h = struct.unpack_from("<I", b, d + 0x10)[0]
            objs.append((h, cs, ce))
    return objs, count, rows, whole


def check_object(b, s, e, label, strict_bounds=True):
    """The object as 0x74D550 and 0x64D5E0 walk it."""
    kids = {}
    order = []
    good = True
    for k, cs, ce in chunks(b, s + 8, e)[0]:
        d, fill = data_of(b, k, cs, ce, LOADER_ALIGNED)
        good &= fill and (k not in LOADER_ALIGNED or d % 16 == 0)
        kids.setdefault(k, []).append((d, ce))
        order.append(k)
    hd = kids[0x134011][0][0]
    name = b[hd + 0xA0:kids[0x134011][0][1]].split(b"\0")[0].decode("latin1")
    h = struct.unpack_from("<I", b, hd + 0x10)[0]
    ts, te = kids[0x134012][0][0], kids[0x134012][0][1]
    ntex = (te - ts) // 8
    tex = [struct.unpack_from("<I", b, ts + 8 * i)[0] for i in range(ntex)]
    check(h == bhash(name), f"{label}: header hash {h:08X} is the hash of its name '{name}'")
    check(b[hd + 0x19] == ntex and b[hd + 0x1A] == len(kids.get(0x134013, [])) and b[hd + 0x1B] == 0 or 0x13401A in kids,
          f"{label}: header counts: {ntex} textures (+0x19 = {b[hd + 0x19]}), 0x134013 entries +0x1A = {b[hd + 0x1A]}")
    check(good, f"{label}: aligned children start on 16 after 0x11 filler")
    mesh = {}
    morder = []
    ms, me = None, None
    for k, cs, ce in chunks(b, s + 8, e)[0]:
        if k == 0x80134100:
            ms, me = cs, ce
    for k, cs, ce in chunks(b, ms + 8, me)[0]:
        d, fill = data_of(b, k, cs, ce, LOADER_ALIGNED)
        good &= fill
        mesh.setdefault(k, []).append((d, ce))
        morder.append(k)
    dd = mesh[0x134900][0][0]
    nm, nbuf, nidx, flags = (struct.unpack_from("<I", b, dd + o)[0] for o in (0x10, 0x18, 0x2C, 0x0C))
    md, me2 = mesh[0x134B02][0]
    check((me2 - md) // 116 == nm, f"{label}: descriptor says {nm} materials, the list holds {(me2 - md) // 116}")
    check(len(mesh.get(0x134B01, [])) == nbuf, f"{label}: {nbuf} vertex buffers named, {len(mesh.get(0x134B01, []))} there")
    check(len(mesh.get(0x134C02, [])) == nm, f"{label}: one shader name (0x134C02) per material")
    orf = 0
    runs, last = [], None
    total_idx = 0
    idx_d, idx_e = mesh[0x134B03][0]
    idx = struct.unpack_from(f"<{(idx_e - idx_d) // 2}H", b, idx_d)
    lo, hi = [1e30] * 3, [-1e30] * 3
    for m in range(nm):
        at = md + 116 * m
        f0, eff = struct.unpack_from("<I", b, at)[0], struct.unpack_from("<I", b, at + 8)[0]
        orf |= f0
        nv, ni, nt, first = struct.unpack_from("<4I", b, at + 0x40)
        if eff != last:
            runs.append([eff, 0, []])
            last = eff
        runs[-1][2].append((runs[-1][1], nv, first, ni))
        runs[-1][1] += nv
        slots = b[at + 0x28:at + 0x2D]
        check(all(x < ntex or x == 0xFF for x in slots) and b[at + 0x2D] == 0xFF and ni == nt * 3,
              f"{label}: material {m}: effect {eff}, texture slots {slots.hex()} into {ntex} textures (diffuse {tex[slots[0]]:08X}), {nv} vertices, {nt} triangles")
        total_idx += ni
    check(flags == orf, f"{label}: descriptor flags {flags:#x} = OR of the materials' flags {orf:#x}")
    check(total_idx == nidx and nidx <= len(idx), f"{label}: {nidx} indices named, {total_idx} by the materials, {len(idx)} stored")
    for r, (eff, nv, mats) in enumerate(runs):
        bd, be = mesh[0x134B01][r]
        stride = STRIDE.get(eff)
        check(stride and be - bd == nv * stride, f"{label}: run {r} (effect {eff}): buffer {be - bd} bytes = {nv} x {stride}")
        for base, mv, first, ni in mats:
            inside = all(base <= idx[first + i] < base + mv for i in range(ni))
            check(inside, f"{label}: run {r}: the material's indices fall in its own vertices {base}..{base + mv - 1}")
        for v in range(nv):
            p = struct.unpack_from("<3f", b, bd + v * stride)
            lo = [min(lo[i], p[i]) for i in range(3)]
            hi = [max(hi[i], p[i]) for i in range(3)]
    hmin = struct.unpack_from("<3f", b, hd + 0x20)
    hmax = struct.unpack_from("<3f", b, hd + 0x30)
    check(not strict_bounds or all(abs(hmin[i] - lo[i]) < 1e-3 and abs(hmax[i] - hi[i]) < 1e-3 for i in range(3)),
          f"{label}: header bounds {[round(x, 2) for x in hmin]}..{[round(x, 2) for x in hmax]} = the vertices'")
    return h, name, nidx // 3


_sources = {}


def find_source(name, hashes):
    """The shipped object a copy `<name>_FRS<n>` was made from (one of the placed hashes)."""
    base = name.rsplit("_FRS", 1)[0]
    for h in hashes:
        if bhash(base) != h:
            continue
        if h not in _sources:
            import glob
            # A new model of the editor's is copied from where this save wrote it.
            written_files = sorted(glob.glob(os.path.join(out, "STREAML5RA_*.BUN")))
            for f in written_files + sorted(glob.glob(os.path.join(tracks, "STREAML5RA_*.BUN"))):
                b = open(f if f in written_files else shipped(os.path.basename(f)), "rb").read()
                for k, s, e in chunks(b, 0, len(b))[0]:
                    if k == 0x80134000:
                        for oh, os_, oe in pack_objects(b, s, e)[0]:
                            if oh == h:
                                _sources[h] = canon(b, 0x80134010, os_, oe)
                if h in _sources:
                    break
        return _sources.get(h)
    return None


def strip_header(c):
    """An object's canonical form with the header's hash and name left out."""
    out = []
    for k, v in c[1]:
        if k == 0x134011:
            v = v[:0x10] + v[0x14:0xA0]
        out.append((k, v))
    return tuple(out)


def src_name(c):
    if c is None:
        return "NOTHING FOUND"
    hd = [v for k, v in c[1] if k == 0x134011][0]
    return f"{struct.unpack_from('<I', hd, 0x10)[0]:08X} {hd[0xA0:].split(bytes(1))[0].decode('latin1')}"


def edit_list(folder):
    path = os.path.join(folder, "frs-original", "L5RA-edits.txt")
    lines = open(path).read().split("\n") if os.path.exists(path) else []
    news = {int(l.split()[2], 16): int(l.split()[1]) for l in lines if l.startswith("N ")}
    places = [(int(l.split()[1]), int(l.split()[3], 16)) for l in lines if l.startswith("P ")]
    return news, places, lines


news, places, lines = edit_list(out)
print(f"== edit list: {len(lines) - (1 if lines and not lines[-1] else 0)} lines, new models {[f'{h:08X}' for h in news]}, placements {[(s, f'{h:08X}') for s, h in places]}")

written = sorted(f for f in os.listdir(out) if f.upper().startswith("STREAML5RA_") and f.upper().endswith(".BUN"))
for f in written:
    new = open(os.path.join(out, f), "rb").read()
    old = open(shipped(f), "rb").read()
    print(f"== {f}: {len(old)} -> {len(new)} bytes")
    if expect_shipped:
        check(new == old, "the file is the shipped one again")
        check(not os.path.exists(os.path.join(out, "frs-original", f)), "no kept copy left beside it")
        continue
    kept = os.path.join(out, "frs-original", f)
    check(os.path.exists(kept) and open(kept, "rb").read() == old, "the kept copy is the shipped file")
    check((len(new) - len(old)) % 128 == 0, f"grew by {len(new) - len(old)}, a multiple of 128")
    told, endo = chunks(old, 0, len(old))
    tnew, endn = chunks(new, 0, len(new))
    check(endo and endn, "both parse as a chunk chain to their last byte")
    a = [c for c in told if c[0] != 0]
    z = [c for c in tnew if c[0] != 0]
    check([c[0] for c in a] == [c[0] for c in z], f"same {len(a)} top-level chunks in the same order (padding aside)")
    number = int(f[len("STREAML5RA_"):-4]) if not f.upper().startswith("STREAML5RA_0X") else None
    stored_new = set()
    for (ko, so, eo), (kn, sn, en) in zip(a, z):
        if ko == 0x80134000:
            oo, co, ro, _ = pack_objects(old, so, eo)
            on, cn, rn, whole = pack_objects(new, sn, en)
            hashes = [h for h, _, _ in on]
            check(whole and cn == len(on) == len(rn) and rn == hashes and hashes == sorted(set(hashes)),
                  f"solid list @{sn:#x}: count {cn}, {len(rn)} rows, {len(on)} objects, rows = objects in hash order")
            old_c = {h: canon(old, 0x80134010, s, e) for h, s, e in oo}
            new_c = {h: (canon(new, 0x80134010, s, e), s, e) for h, s, e in on}
            check(all(h in new_c and new_c[h][0] == c for h, c in old_c.items()),
                  f"its {len(old_c)} shipped objects are all there, unchanged")
            for h in set(new_c) - set(old_c):
                hh, name, tris = check_object(new, new_c[h][1], new_c[h][2], f"added {h:08X}", h in news)
                stored_new.add(h)
                if h not in news:
                    src = find_source(name, {ph for ps, ph in places if ps == number})
                    same = src is not None and strip_header(src) == strip_header(new_c[h][0])
                    check(same, f"added {h:08X}: a copy of {src_name(src)}, byte for byte but its hash and name")
        elif ko == 0x80034100:
            def kids(b, s, e):
                res = {}
                for k, cs, ce in chunks(b, s + 8, e)[0]:
                    d, fill = data_of(b, k, cs, ce, SCENERY_ALIGNED)
                    res[k] = (d, ce - d, fill)
                return res
            ro_, rn_ = kids(old, so, eo), kids(new, sn, en)
            nro, nrn = ro_[0x34103][1] // 96, rn_[0x34103][1] // 96
            check(all(v[2] for v in rn_.values()), "scenery: aligned children start after 0x11 filler")
            check(new[rn_[0x34103][0]:rn_[0x34103][0] + nro * 96] == old[ro_[0x34103][0]:ro_[0x34103][0] + nro * 96],
                  f"scenery: its {nro} shipped records unchanged")
            check(rn_[0x3410D][1] // 32 >= nrn, f"scenery: {nrn} records, {rn_[0x3410D][1] // 32} 0x3410D entries")
            ni_o, ni_n = ro_[0x34102][1] // 72, rn_[0x34102][1] // 72
            check(new[rn_[0x34102][0]:rn_[0x34102][0] + ni_o * 72] == old[ro_[0x34102][0]:ro_[0x34102][0] + ni_o * 72],
                  f"scenery: its {ni_o} shipped infos unchanged ({ni_n - ni_o} added)")
            nodes, nb = rn_[0x34105][0], rn_[0x34105][1]
            leaves, stack, seen = [], [0], set()
            while stack:
                n = stack.pop()
                if n in seen or n >= nb // 48:
                    continue
                seen.add(n)
                for c in range(struct.unpack_from("<h", new, nodes + 48 * n + 0x0C)[0]):
                    ch = struct.unpack_from("<h", new, nodes + 48 * n + 0x1C + 2 * c)[0]
                    (leaves.append(ch) if ch >= 0 else stack.append(-ch))
            for i in range(nro, nrn):
                r = rn_[0x34103][0] + 96 * i
                info = struct.unpack_from("<h", new, r + 0x54)[0]
                level0 = struct.unpack_from("<I", new, rn_[0x34102][0] + 72 * info + 0x18)[0] if 0 <= info < ni_n else 0
                pos = [round(x, 2) for x in struct.unpack_from("<3f", new, r + 0x20)]
                here = level0 in stored_new or any(h == level0 for h, _, _ in pack_objects(new, sn, en)[0])
                infos_old = {struct.unpack_from("<I", old, ro_[0x34102][0] + 72 * k + 0x18)[0] for k in range(ni_o)}
                placed = {h for s, h in places if s == number}
                how = ('added to this file' if level0 in stored_new else 'shipped info' if level0 in infos_old else
                       'named where it is stored, not copied (the log says why)' if level0 in placed else 'UNACCOUNTED')
                check(0 <= info < ni_n and leaves.count(i) == 1 and how != 'UNACCOUNTED',
                      f"scenery: new record {i} at {pos}: info {info} names {level0:08X} ({how}), reached once by the tree")
    # what was added is accounted for
    for h in stored_new:
        named = h in news or any(s == number for s, _ in places)
        check(named, f"added model {h:08X}: {'a new model of the list' if h in news else 'a copy for a placement of the list'}")
    for h, s in news.items():
        if s == number:
            check(h in stored_new, f"new model {h:08X} of the list is stored in this file")

# --- the section table ---------------------------------------------------------------
lay = os.path.join(out, "L5RA.BUN")
if os.path.exists(lay):
    a = open(shipped("L5RA.BUN"), "rb").read()
    b = open(lay, "rb").read()
    p = 0
    while p < len(a):
        k, n = struct.unpack_from("<II", a, p)
        if k == 0x34110:
            table, count = p + 8, n // 208
        p += 8 + n
    diff = [i for i in range(len(a)) if a[i] != b[i]] if len(a) == len(b) else None
    check(diff is not None, "L5RA.BUN: same size as shipped")
    entries = sorted({(i - table) // 208 for i in diff or []})
    if expect_shipped:
        check(not entries, "L5RA.BUN: the shipped table again")
    for e in entries:
        at = table + 208 * e
        tag = a[at:at + 8].split(b"\0")[0].decode()
        num = struct.unpack_from("<I", a, at + 8)[0]
        if struct.unpack_from("<I", a, at + 0x1C)[0]:
            continue   # a texture section (new textures): verify_newtex.py checks its five parts
        fn = f"STREAML5RA_{num}.BUN"
        was, now = struct.unpack_from("<3I", a, at + 0x60), struct.unpack_from("<3I", b, at + 0x60)
        only = all(at + 0x60 <= i < at + 0x6C for i in diff if (i - table) // 208 == e)
        size = os.path.getsize(os.path.join(out, fn)) if os.path.exists(os.path.join(out, fn)) else -1
        grow = size - os.path.getsize(shipped(fn))
        check(only and now[0] == now[1] == size and now[2] == was[2] + grow,
              f"L5RA.BUN entry {tag}: {was} -> {now}; only sizes changed, = file size {size}, third raised by {grow}")
    check(all(f"STREAML5RA_{struct.unpack_from('<I', a, table + 208 * e + 8)[0]}.BUN" in written or
              struct.unpack_from('<I', a, table + 208 * e + 0x1C)[0] for e in entries),
          "L5RA.BUN: only entries of written files changed")
elif written and not expect_shipped:
    check(False, "L5RA.BUN was not written")

if same_as:
    for f in sorted(os.listdir(same_as)):
        if f.upper().endswith(".BUN"):
            check(os.path.exists(os.path.join(out, f)) and open(os.path.join(out, f), "rb").read() ==
                  open(os.path.join(same_as, f), "rb").read(), f"{f} is byte for byte the one in {os.path.basename(same_as)}")
print("RESULT:", "all checks passed" if ok_all else "PROBLEMS FOUND")
