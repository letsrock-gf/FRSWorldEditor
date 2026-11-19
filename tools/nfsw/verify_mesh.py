"""Checks the NFSW mesh edits an isolated save wrote (FRS_WRITE_TRACKS folder) against the shipped files.
Usage: verify_mesh.py <written TRACKS folder> <shipped TRACKS folder> [<OBJ saved by Blender> (manifest beside it)]

For every `O <hash> <file>` line of frs-original/L5RA-edits.txt:
  - the written section file walks cleanly to its end, grew by a multiple of 128 (L5RA.BUN sizes raised by as much),
    and every top-level chunk keeps its offset modulo 128 and its bytes (scenery: only boxes and radii may differ);
  - the edited object is consistent (counts, runs, buffer sizes, index ranges, bounds, area) and its vertices and
    triangles are the kept model-space OBJ's;
  - with the Blender OBJ: that OBJ taken back to model space is the same set of vertices as the written object;
  - every other object in the file is byte-identical to the shipped one (or identical but for 0x11 filler when the
    edited object before it changed size by other than a multiple of 16).
Exit code 0 only when every check passes."""
import json
import math
import os
import re
import struct
import sys

LAYOUT = {}
for e in (0x0, 0x1, 0xA, 0xC, 0xF): LAYOUT[e] = (36, 12, 28, 20)
for e in (0x2, 0x3, 0x7, 0xB): LAYOUT[e] = (44, 12, 28, 20)
LAYOUT[0x4] = (28, -1, 12, 20)
LAYOUT[0x5] = (36, 12, 20, 28)
ALIGN = {0x134B01: 16, 0x134012: 8}

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures.append(what)
    return ok


def walk(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            return
        yield k, p, p + 8, p + 8 + n
        p += 8 + n


def data_start(b, s, e, cid):
    if cid & 0x80000000:
        return s
    a = ALIGN.get(cid, 16)
    f = (a - s % a) % a
    return s + f if f <= e - s and b[s:s + f] == b"\x11" * f else s


def objects(b):
    out = []
    for k, h, s, e in walk(b, 0, len(b)):
        if k != 0x80134000:
            continue
        for k1, h1, s1, e1 in walk(b, s, e):
            if k1 != 0x80134010:
                continue
            hd = next((data_start(b, s2, e2, k2) for k2, h2, s2, e2 in walk(b, s1, e1) if k2 == 0x134011), None)
            if hd is not None:
                out.append((struct.unpack_from("<I", b, hd + 0x10)[0], h1, e1))
    return out


def stripped(b, h, e):
    """An object's leaves without their filler, in order: what stays when only alignment moved."""
    out = []

    def rec(s, e2):
        for k, hh, ss, ee in walk(b, s, e2):
            if k & 0x80000000:
                out.append((k, None))
                rec(ss, ee)
            else:
                out.append((k, b[data_start(b, ss, ee, k):ee]))
    rec(h + 8, e)
    return out


def parse_object(b, h, e):
    o = {"bufs": []}
    for k, hh, s, e2 in walk(b, h + 8, e):
        if k == 0x134011:
            o["hdr"] = data_start(b, s, e2, k)
        if k == 0x80134100:
            for k3, h3, s3, e3 in walk(b, s, e2):
                d = data_start(b, s3, e3, k3)
                if k3 == 0x134900: o["desc"] = d
                if k3 == 0x134B02: o["mats"] = [b[d + 116 * i:d + 116 * (i + 1)] for i in range((e3 - d) // 116)]
                if k3 == 0x134B03: o["idx"] = (d, e3)
                if k3 == 0x134B01: o["bufs"].append(b[d:e3])
    return o


def decode(b, o):
    """Per material: vertices (pos, normal, uv, colour) and triangles as vertex numbers into that list."""
    mats = o["mats"]
    runs = []
    for i, m in enumerate(mats):
        eff = struct.unpack_from("<I", m, 8)[0]
        if not runs or runs[-1][0] != eff:
            runs.append([eff, []])
        runs[-1][1].append(i)
    per = {}
    problems = []
    if len(runs) != len(o["bufs"]):
        problems.append(f"{len(o['bufs'])} buffers for {len(runs)} runs")
        return per, problems
    first_expected = 0
    for (eff, ms), buf in zip(runs, o["bufs"]):
        stride, nrm, col, uv = LAYOUT[eff]
        total = sum(struct.unpack_from("<I", mats[i], 0x40)[0] for i in ms)
        if len(buf) != total * stride:
            problems.append(f"effect {eff}: buffer {len(buf)} bytes for {total} vertices of {stride}")
            continue
        base = 0
        for i in ms:
            vc, ic, tri, first = struct.unpack_from("<4I", mats[i], 0x40)
            if tri * 3 != ic: problems.append(f"material {i}: {tri} triangles for {ic} indices")
            if first != first_expected: problems.append(f"material {i}: first index {first}, expected {first_expected}")
            first_expected += ic
            verts = []
            for v in range(vc):
                a = (base + v) * stride
                pos = struct.unpack_from("<3f", buf, a)
                n = tuple(x / 32767 for x in struct.unpack_from("<3h", buf, a + nrm)) if nrm >= 0 else None
                verts.append((pos, n, struct.unpack_from("<2f", buf, a + uv), struct.unpack_from("<I", buf, a + col)[0]))
            ids = struct.unpack_from(f"<{ic}H", b, o["idx"][0] + 2 * first) if ic else ()
            if any(not (base <= x < base + vc) for x in ids):
                problems.append(f"material {i}: an index outside its vertices")
            tris = [tuple(x - base for x in ids[t:t + 3]) for t in range(0, len(ids), 3)]
            lo = [min(p[0][c] for p in verts) for c in range(3)] if verts else None
            hi = [max(p[0][c] for p in verts) for c in range(3)] if verts else None
            mlo, mhi = struct.unpack_from("<3f", mats[i], 0x10), struct.unpack_from("<3f", mats[i], 0x1C)
            if verts and any(abs(lo[c] - mlo[c]) > 1e-3 or abs(hi[c] - mhi[c]) > 1e-3 for c in range(3)):
                problems.append(f"material {i}: bounds {mlo}..{mhi}, vertices span {lo}..{hi}")
            per[i] = (verts, tris)
            base += vc
    return per, problems


def read_kept(path):
    v, vn, vt, mats, cur, same = [], [], [], {}, None, False
    for line in open(path, encoding="utf-8"):
        p = line.split()
        if not p: continue
        if p[0] == "#" and "frs-topology" in line: same = "same" in line
        elif p[0] == "v": v.append(tuple(map(float, p[1:4])))
        elif p[0] == "vn": vn.append(tuple(map(float, p[1:4])))
        elif p[0] == "vt": vt.append(tuple(map(float, p[1:3])))
        elif p[0] == "usemtl": cur = int(p[1][1:]); mats.setdefault(cur, [])
        elif p[0] == "f": mats[cur].append(tuple(int(c.split("/")[0]) - 1 for c in p[1:4]))
    return v, vn, vt, mats, same


def key(p, q=1e-3):
    return tuple(round(c / q) for c in p)


def tri_set(points, tris):
    out = []
    for t in tris:
        ks = [key(points[i]) for i in t]
        j = ks.index(min(ks))
        out.append(tuple(ks[j:] + ks[:j]))
    return sorted(out)


def section_entry(l5, name):
    """The L5RA.BUN table entry (offset) for a section file name."""
    m = re.match(r"STREAML5RA_(0x[0-9A-F]+|\d+)\.BUN", name, re.I)
    for k, h, s, e in walk(l5, 0, len(l5)):
        if k != 0x34110: continue
        for i in range((e - s) // 208):
            a = s + 208 * i
            num, = struct.unpack_from("<I", l5, a + 8)
            h1c, = struct.unpack_from("<I", l5, a + 0x1C)
            if (m.group(1).lower().startswith("0x") and h1c == int(m.group(1), 16)) or \
               (not m.group(1).lower().startswith("0x") and num == int(m.group(1))):
                return a
    return None


def main():
    written_dir, shipped_dir = sys.argv[1], sys.argv[2]
    blender_obj = sys.argv[3] if len(sys.argv) > 3 else None
    kept_dir = os.path.join(written_dir, "frs-original")
    edits = [l.split() for l in open(os.path.join(kept_dir, "L5RA-edits.txt"), encoding="utf-8") if l.startswith("O ")]
    check(bool(edits), f"edit list names {len(edits)} edited meshes")
    hashes = {int(h, 16): f for _, h, f in edits}

    written_files = [f for f in os.listdir(written_dir) if f.upper().startswith("STREAML5RA_")]
    found = {}
    l5_written = os.path.join(written_dir, "L5RA.BUN")
    l5_shipped = open(os.path.join(shipped_dir, "L5RA.BUN"), "rb").read()
    for name in sorted(written_files):
        b = open(os.path.join(written_dir, name), "rb").read()
        s = open(os.path.join(kept_dir, name), "rb").read() if os.path.exists(os.path.join(kept_dir, name)) \
            else open(os.path.join(shipped_dir, name), "rb").read()
        ship_live = open(os.path.join(shipped_dir, name), "rb").read()
        check(s == ship_live, f"{name}: the kept copy is the shipped file")
        objs_w, objs_s = objects(b), objects(s)
        edited_here = [hsh for hsh, _, _ in objs_w if hsh in hashes]
        print(f"== {name}: {len(b)} bytes (shipped {len(s)}), edited objects here: {[f'{x:08X}' for x in edited_here]}")

        # --- the file as a whole ---------------------------------------------------------------------
        top_w, top_s = list(walk(b, 0, len(b))), list(walk(s, 0, len(s)))
        check(top_w and top_w[-1][3] == len(b), f"{name}: top-level chunks walk to the end of the file")
        delta = len(b) - len(s)
        check(delta % 128 == 0, f"{name}: grew by {delta} bytes (a multiple of 128)")
        nonpad_w = [c for c in top_w if c[0] != 0]
        nonpad_s = [c for c in top_s if c[0] != 0]
        check(len(nonpad_w) == len(nonpad_s), f"{name}: {len(nonpad_w)} top-level chunks (shipped {len(nonpad_s)})")
        for cw, cs in zip(nonpad_w, nonpad_s):
            if cw[0] != cs[0]:
                check(False, f"{name}: chunk {cw[0]:08X} where {cs[0]:08X} shipped")
                continue
            if cw[1] % 128 != cs[1] % 128:
                check(False, f"{name}: chunk {cw[0]:08X} moved from offset {cs[1]:#x} to {cw[1]:#x} (not by 128s)")
            if cw[0] == 0x80134000:
                continue  # the packs: object by object below
            same = b[cw[1]:cw[3]] == s[cs[1]:cs[3]]
            if cw[0] == 0x80034100:
                diff = sum(1 for x, y in zip(b[cw[1]:cw[3]], s[cs[1]:cs[3]]) if x != y)
                check(cw[3] - cw[1] == cs[3] - cs[1], f"{name}: scenery {cw[1]:#x} same size, {diff} bytes differ (boxes, radii)")
            elif not same:
                check(False, f"{name}: chunk {cw[0]:08X} at {cw[1]:#x} differs from the shipped one")
        if delta:
            if os.path.exists(l5_written):
                lw = open(l5_written, "rb").read()
                at = section_entry(l5_shipped, name)
                ok = at is not None and all(struct.unpack_from("<I", lw, at + f)[0] - struct.unpack_from("<I", l5_shipped, at + f)[0] == delta
                                            for f in (0x60, 0x64, 0x68))
                check(ok, f"{name}: L5RA.BUN sizes +0x60/+0x64/+0x68 raised by {delta}")
            else:
                check(False, f"{name}: grew but no L5RA.BUN was written")

        # --- untouched objects ---------------------------------------------------------------------------
        shipped_by_hash = {hsh: (h, e) for hsh, h, e in objs_s}
        identical = filler_only = 0
        for hsh, h, e in objs_w:
            if hsh in hashes or hsh not in shipped_by_hash:
                continue
            sh, se = shipped_by_hash[hsh]
            if b[h:e] == s[sh:se]:
                identical += 1
            elif stripped(b, h, e) == stripped(s, sh, se):
                filler_only += 1
            else:
                check(False, f"{name}: untouched object {hsh:08X} changed")
        check(identical > 0 or len(objs_w) == len(edited_here),
              f"{name}: {identical} untouched objects byte-identical, {filler_only} identical but for 0x11 filler")
        check(len(objs_w) == len(objs_s), f"{name}: {len(objs_w)} objects (shipped {len(objs_s)})")

        # --- the edited objects ------------------------------------------------------------------------
        for hsh, h, e in objs_w:
            if hsh not in hashes:
                continue
            o = parse_object(b, h, e)
            per, problems = decode(b, o)
            for p in problems:
                check(False, f"{hsh:08X}: {p}")
            mats = o["mats"]
            ic_total = sum(struct.unpack_from("<I", m, 0x44)[0] for m in mats)
            hd, de = o["hdr"], o["desc"]
            runs = 1 + sum(1 for i in range(1, len(mats)) if mats[i][8:12] != mats[i - 1][8:12])
            check(struct.unpack_from("<I", b, hd + 0x14)[0] * 3 == ic_total, f"{hsh:08X}: header triangles {ic_total // 3}")
            check(struct.unpack_from("<I", b, de + 0x10)[0] == len(mats), f"{hsh:08X}: descriptor materials {len(mats)}")
            check(struct.unpack_from("<I", b, de + 0x18)[0] == runs == len(o["bufs"]), f"{hsh:08X}: descriptor buffers {runs}")
            check(struct.unpack_from("<I", b, de + 0x2C)[0] == ic_total, f"{hsh:08X}: descriptor indices {ic_total}")
            check((o["idx"][1] - o["idx"][0]) == (2 * ic_total + 3) // 4 * 4,
                  f"{hsh:08X}: index buffer holds {ic_total} indices (padded to 4 bytes, as shipped)")
            pts = [v[0] for verts, _ in per.values() for v in verts]
            lo, hi = struct.unpack_from("<3f", b, hd + 0x20), struct.unpack_from("<3f", b, hd + 0x30)
            check(all(lo[c] - 1e-3 <= p[c] <= hi[c] + 1e-3 for p in pts for c in range(3)) and
                  all(abs(min(p[c] for p in pts) - lo[c]) < 1e-3 and abs(max(p[c] for p in pts) - hi[c]) < 1e-3 for c in range(3)),
                  f"{hsh:08X}: header bounds {tuple(round(x, 2) for x in lo)}..{tuple(round(x, 2) for x in hi)} fit the vertices")
            area = sum(0.5 * math.dist((0, 0, 0), (
                (b2[1] - a2[1]) * (c2[2] - a2[2]) - (b2[2] - a2[2]) * (c2[1] - a2[1]),
                (b2[2] - a2[2]) * (c2[0] - a2[0]) - (b2[0] - a2[0]) * (c2[2] - a2[2]),
                (b2[0] - a2[0]) * (c2[1] - a2[1]) - (b2[1] - a2[1]) * (c2[0] - a2[0])))
                for verts, tris in per.values() for t in tris for a2, b2, c2 in [[verts[i][0] for i in t]])
            got_area, density = struct.unpack_from("<2f", b, hd + 0x98)
            check(abs(got_area - area) <= 1e-3 * max(1, area) and abs(density - (ic_total / 3) / area) <= 1e-3 * max(1, density),
                  f"{hsh:08X}: header area {got_area:.2f} (computed {area:.2f}), density {density:.5f}")

            kv, kn, kt, kmats, same = read_kept(os.path.join(kept_dir, "meshes", hashes[hsh]))
            # Each written vertex is matched to the kept vertex of its material at the same place (within 1e-3)
            # and with the same UV; then the triangles must be the kept ones, vertex for vertex.
            ok = True
            unmatched = 0
            worst_uv = 0.0
            for m, tris in kmats.items():
                if m not in per:
                    ok = check(False, f"{hsh:08X}: material {m} of the kept mesh is not written") and ok
                    continue
                verts, wt = per[m]
                used = sorted({i for t in tris for i in t})
                cells = {}
                for i in used:
                    cells.setdefault(key(kv[i], 1e-2), []).append(i)

                def match(pos, uv, n):
                    k = key(pos, 1e-2)
                    best = None
                    for dx in (-1, 0, 1):
                        for dy in (-1, 0, 1):
                            for dz in (-1, 0, 1):
                                for i in cells.get((k[0] + dx, k[1] + dy, k[2] + dz), ()):
                                    d = math.dist(pos, kv[i]) + abs(uv[0] - kt[i][0]) + abs(uv[1] - kt[i][1])
                                    if n is not None and i < len(kn):   # double-sided cards: same place, other normal
                                        d += 0.01 * math.dist(n, kn[i])
                                    if best is None or d < best[0]:
                                        best = (d, i)
                    return best
                mapped = []
                for pos, n, uv, col in verts:
                    best = match(pos, uv, n)
                    if best is None or math.dist(pos, kv[best[1]]) > 1e-3:
                        unmatched += 1
                        mapped.append(-1)
                        continue
                    mapped.append(best[1])
                    worst_uv = max(worst_uv, abs(uv[0] - kt[best[1]][0]), abs(uv[1] - kt[best[1]][1]))

                # Triangles as the kept vertices' values: shipped meshes repeat vertices exactly, so indices
                # alone cannot be paired; values taken from the kept side on both sides cannot round apart.
                def canon(t):
                    t = [(key(kv[i]), key(kt[i], 1e-5)) if i >= 0 else None for i in t]
                    if None in t:
                        return ()
                    j = t.index(min(t))
                    return tuple(t[j:] + t[:j])
                if sorted(canon(list(t)) for t in tris) != sorted(canon([mapped[i] for i in t]) for t in wt):
                    ok = check(False, f"{hsh:08X}: material {m} triangles differ from the kept mesh") and ok
            check(ok and unmatched == 0,
                  f"{hsh:08X}: {sum(len(t) for t in kmats.values())} triangles over {len(kv)} kept vertices = written "
                  f"({sum(len(v) for v, _ in per.values())} vertices, {unmatched} unmatched) [{'same layout' if same else 'rebuilt'}]")
            check(worst_uv < 1e-4, f"{hsh:08X}: UVs match the kept mesh (largest difference {worst_uv:.6f})")
            found[hsh] = (b, o, per)

    for hsh in hashes:
        check(hsh in found, f"{hsh:08X}: written into a section file")

    # --- the OBJ Blender saved, taken back to model space, against what was written ------------------------
    if blender_obj:
        man = json.load(open(os.path.splitext(blender_obj)[0] + ".json", encoding="utf-8"))
        origin = man["origin"]
        objs, name, vs, ranges = {}, None, [], {}
        for line in open(blender_obj, encoding="utf-8"):
            p = line.split()
            if not p: continue
            if p[0] == "o": name = p[1]; ranges[name] = [len(vs), len(vs)]
            elif p[0] == "v": vs.append(tuple(map(float, p[1:4]))); ranges[name][1] = len(vs)
        for entry in man["objects"]:
            hsh = int(entry["hash"], 16)
            if hsh not in found:
                continue
            nm = next((n for n in ranges if n == entry["name"] or n.split(".")[0] == entry["name"]), None)
            t = entry["transform"]
            r = [t[0:3], t[3:6], t[6:9]]
            tr = t[9:12]
            local = []
            for x, y, z in vs[ranges[nm][0]:ranges[nm][1]]:
                w = (x + origin[0], -z + origin[1], y + origin[2])
                d = [w[c] - tr[c] for c in range(3)]
                # row vectors: world = local * R + T, so local_i = d . r_i / |r_i|^2 (rows orthogonal)
                local.append(tuple(sum(d[j] * r[i][j] for j in range(3)) / sum(x * x for x in r[i]) for i in range(3)))
            b, o, per = found[hsh]
            written = [v[0] for verts, _ in per.values() for v in verts]
            tol = 2e-3   # the OBJ carries 4 decimals

            def near(points, q):
                cells = {}
                for p in points:
                    cells.setdefault(key(p, tol), []).append(p)
                def has(p):
                    k = key(p, tol)
                    return any(math.dist(p, x) <= tol for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)
                               for x in cells.get((k[0] + dx, k[1] + dy, k[2] + dz), ()))
                return sum(1 for p in q if not has(p))
            missing, extra = near(written, local), near(local, written)
            check(missing == 0 and extra == 0, f"{hsh:08X}: the Blender OBJ in model space has the written positions "
                                               f"({len(local)} OBJ vertices, {len(written)} written; {missing} OBJ vertices "
                                               f"not written, {extra} written not in the OBJ)")

    print("PASS" if not failures else f"FAIL ({len(failures)})")
    sys.exit(0 if not failures else 1)


main()
