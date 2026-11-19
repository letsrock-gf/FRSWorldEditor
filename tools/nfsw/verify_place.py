"""Checks sections written with newly placed objects, the way nfsw.exe reads them (loader 0x746060, tree walk 0x73DB90).
Usage: verify_place.py <written folder> <TRACKS>"""
import os
import struct
import sys

out, tracks = sys.argv[1], sys.argv[2]
ALIGNED = {0x34101, 0x34103, 0x34105, 0x3410A, 0x3410E, 0x3410F, 0x34C03}


def top(b):
    p, res = 0, []
    while p + 8 <= len(b):
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > len(b):
            break
        res.append((k, p, n))
        p += 8 + n
    return res, p


def container(b, start, size):
    """Children as the loader sees them: id -> (data offset, data size), with the alignment the loader applies."""
    kids, p, end, ok = {}, start + 8, start + 8 + size, True
    while p + 8 <= end:
        k, n = struct.unpack_from("<II", b, p)
        data = p + 8
        if k in ALIGNED:
            data = (p + 0x17) & ~0xF                      # lea eax,[edi+17h]; and eax,0FFFFFFF0h
            if any(x != 0x11 for x in b[p + 8:data]):
                ok = False
        kids[k] = (data, p + 8 + n - data)
        p += 8 + n
    return kids, ok and p == end


def boxes_contain(outer_min, outer_max, inner_min, inner_max, eps=1e-3):
    return all(outer_min[i] <= inner_min[i] + eps and outer_max[i] >= inner_max[i] - eps for i in range(3))


all_ok = True
for f in sorted(os.listdir(out)):
    if not (f.upper().startswith("STREAML5RA_") and f.upper().endswith(".BUN")):
        continue
    new = open(os.path.join(out, f), "rb").read()
    old = open(os.path.join(tracks, f), "rb").read()
    kept = open(os.path.join(out, "frs-original", f), "rb").read()
    delta = len(new) - len(old)
    print(f"== {f}: {len(old)} -> {len(new)} (+{delta}); multiple of 128: {delta % 128 == 0}; kept copy is the shipped file: {kept == old}")
    all_ok &= delta % 128 == 0 and kept == old

    told, endo = top(old)
    tnew, endn = top(new)
    print(f"   top-level chunks: {len(told)} -> {len(tnew)}; both parse to the end: {endo == len(old) and endn == len(new)}")
    all_ok &= len(told) == len(tnew) and endn == len(new)
    for (ko, po, no), (kn, pn, nn) in zip(told, tnew):
        if ko != 0x80034100 or no == nn:
            same = old[po:po + 8 + no] == new[pn:pn + 8 + nn]
            if not same:
                print(f"   chunk {ko:08X} at {po:#x} CHANGED"); all_ok = False
            continue
        ro, oko = container(old, po, no)
        rn, okn = container(new, pn, nn)
        print(f"   scenery container at {po:#x}: {no} -> {nn} bytes; fillers valid: {okn}")
        all_ok &= okn
        n_old = ro[0x34103][1] // 96
        n_new = rn[0x34103][1] // 96
        infos_old, infos_new = ro[0x34102][1] // 72, rn[0x34102][1] // 72
        extra_new = rn[0x3410D][1] >> 5
        print(f"   records {n_old} -> {n_new}; infos {infos_old} -> {infos_new}; 0x3410D entries {extra_new} (must cover every record: {extra_new >= n_new})")
        all_ok &= extra_new >= n_new
        # old records untouched
        same_old = new[rn[0x34103][0]: rn[0x34103][0] + n_old * 96] == old[ro[0x34103][0]: ro[0x34103][0] + n_old * 96]
        print(f"   shipped records unchanged: {same_old}; boxes and links tables unchanged: {new[rn[0x3410E][0]:rn[0x3410E][0] + rn[0x3410E][1]] == old[ro[0x3410E][0]:ro[0x3410E][0] + ro[0x3410E][1]] and new[rn[0x3410F][0]:rn[0x3410F][0] + rn[0x3410F][1]] == old[ro[0x3410F][0]:ro[0x3410F][0] + ro[0x3410F][1]]}")
        all_ok &= same_old

        # the tree, walked as 0x73DB90 does
        nodes, nbytes = rn[0x34105]
        ncount = nbytes // 48
        leaves, parent, stack, seen = [], {}, [0], set()
        while stack:
            n = stack.pop()
            if n in seen or n >= ncount:
                all_ok = False; print("   TREE: bad node", n); continue
            seen.add(n)
            cnt = struct.unpack_from("<h", new, nodes + 48 * n + 0x0C)[0]
            for c in range(cnt):
                child = struct.unpack_from("<h", new, nodes + 48 * n + 0x1C + 2 * c)[0]
                if child >= 0:
                    leaves.append(child); parent[("leaf", child)] = n
                else:
                    stack.append(-child); parent[("node", -child)] = n
        reach = sorted(leaves) == list(range(n_new)) if n_old == len([x for x in leaves if x < n_old]) else None
        print(f"   tree: {ncount} nodes, {len(leaves)} records reached from the root; every new record reached once: {all(leaves.count(i) == 1 for i in range(n_old, n_new))}; shipped ones still reached: {sorted(x for x in leaves if x < n_old) == sorted(set(x for x in leaves if x < n_old))}")
        all_ok &= all(leaves.count(i) == 1 for i in range(n_old, n_new))

        rq = rn[0x34103][0]
        iq = rn[0x34102][0]
        for i in range(n_old, n_new):
            r = rq + 96 * i
            mn = struct.unpack_from("<3f", new, r); mx = struct.unpack_from("<3f", new, r + 0x10)
            flags, = struct.unpack_from("<I", new, r + 0x0C)
            vis, = struct.unpack_from("<h", new, r + 0x1C)
            pos = struct.unpack_from("<3f", new, r + 0x20)
            info, = struct.unpack_from("<h", new, r + 0x54)
            name = new[iq + 72 * info: iq + 72 * info + 24].split(b"\0")[0].decode() if 0 <= info < infos_new else "?"
            # every node above must hold the box
            n = parent.get(("leaf", i)); chain, held = [], True
            while n is not None:
                nmn = struct.unpack_from("<3f", new, nodes + 48 * n); nmx = struct.unpack_from("<3f", new, nodes + 48 * n + 0x10)
                held &= boxes_contain(nmn, nmx, mn, mx); chain.append(n)
                n = parent.get(("node", n))
            vis_rows = rn.get(0x34107, (0, 0))[1] >> 7
            good = 0 <= info < infos_new and held and chain and chain[-1] == 0 and 0 <= vis < max(vis_rows, 1) and boxes_contain(mn, mx, pos, pos, 50)
            all_ok &= bool(good)
            print(f"   new record {i}: model '{name}' (info {info}), at {[round(x, 1) for x in pos]}, flags {flags:08X}, visibility row {vis} of {vis_rows}, nodes above {chain}, boxes hold it: {held} -> {'ok' if good else 'PROBLEM'}")

# --- the section table ---------------------------------------------------------------
lay_new = os.path.join(out, "L5RA.BUN")
if os.path.exists(lay_new):
    a = open(os.path.join(tracks, "L5RA.BUN"), "rb").read()
    b = open(lay_new, "rb").read()
    kept = open(os.path.join(out, "frs-original", "L5RA.BUN"), "rb").read()
    diff = [i for i in range(len(a)) if a[i] != b[i]] if len(a) == len(b) else None
    p = 0
    while p < len(a):
        k, n = struct.unpack_from("<II", a, p)
        if k == 0x34110:
            table = p + 8
        p += 8 + n
    entries = sorted({(i - table) // 208 for i in diff}) if diff else []
    print(f"== L5RA.BUN: same size {len(a) == len(b)}; kept copy is the shipped file: {kept == a}; entries changed: {entries}")
    for e in entries:
        at = table + 208 * e
        tag = a[at:at + 8].split(b"\0")[0].decode()
        was = struct.unpack_from("<3I", a, at + 0x60); now = struct.unpack_from("<3I", b, at + 0x60)
        only = all(table + 208 * e + 0x60 <= i < table + 208 * e + 0x6C for i in diff if (i - table) // 208 == e)
        fsize = os.path.getsize(os.path.join(out, f"STREAML5RA_{struct.unpack_from('<I', a, at + 8)[0]}.BUN"))
        print(f"   {tag}: sizes {was} -> {now}; only the size fields changed: {only}; first equals the written file's size: {now[0] == fsize}")
        all_ok &= only and now[0] == fsize and kept == a
else:
    print("== L5RA.BUN not written"); all_ok = False
print("RESULT:", "all checks passed" if all_ok else "PROBLEMS FOUND")
