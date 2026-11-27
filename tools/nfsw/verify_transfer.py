"""Checks test_roadtransfer.ps1's files (27/11).

  viaduct.txt        export from the fork on UG2: only the 2 viaduct roads (the group joined to the first road),
                     at the coordinates they had, every value after id/a/b as in the source
  imported-L5RA.txt  NFS World network after both imports: Gui's roads unchanged, plus the viaduct and every road
                     of the original editor's roads-L4RA.txt, each group moved by one offset (shape, heights and
                     profiles kept), ids new
Usage: verify_transfer.py <test folder>
"""
import os
import sys


def read(path):
    nodes, segs = {}, []
    for line in open(path, encoding='ascii', errors='replace'):
        t = line.split()
        if not t:
            continue
        if t[0] == 'node':
            nodes[int(t[1])] = ([float(v) for v in t[2:5]], t[5:])
        elif t[0] == 'segment':
            segs.append((int(t[1]), int(t[2]), int(t[3]), t[4:]))
    return nodes, segs


def same_tail(a, b):
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if x == y:
            continue
        try:
            if abs(float(x) - float(y)) > 1e-3:
                return False
        except ValueError:
            return False
    return True


def node_tail(t):
    # hasHeading heading crosswalks follow followOffset: follow is an id, compared apart
    return t[:3] + t[4:]


def match(src, out, out_ids, label):
    """Every road of src has one in out (among out_ids) with the same values and ends moved by one offset."""
    snodes, ssegs = src
    onodes, osegs = out
    delta, used, bad = None, set(), 0
    for sid, sa, sb, tail in ssegs:
        found = False
        for oid, oa, ob, otail in osegs:
            if oid in used or oid not in out_ids or not same_tail(tail, otail):
                continue
            d = [onodes[oa][0][k] - snodes[sa][0][k] for k in range(3)]
            d2 = [onodes[ob][0][k] - snodes[sb][0][k] for k in range(3)]
            if any(abs(d[k] - d2[k]) > 0.01 for k in range(3)):
                continue
            if delta is None:
                delta = d
            if any(abs(d[k] - delta[k]) > 0.01 for k in range(3)):
                continue
            if not same_tail(node_tail(snodes[sa][1]), node_tail(onodes[oa][1])) or \
               not same_tail(node_tail(snodes[sb][1]), node_tail(onodes[ob][1])):
                continue
            used.add(oid)
            found = True
            break
        if not found:
            bad += 1
            print(f'   FAIL {label}: road {sid} ({sa}-{sb}) has no copy')
    print(f'   {"ok  " if not bad else "FAIL"} {label}: {len(ssegs) - bad}/{len(ssegs)} roads copied, '
          f'offset {[round(v, 2) for v in delta] if delta else None}')
    return used, bad == 0


def main():
    out = sys.argv[1]
    ok = True
    source = read(os.path.join(out, 'ug2-source-L4RA.txt'))
    viaduct = read(os.path.join(out, 'viaduct.txt'))
    original = read(os.path.join(out, 'original-L4RA.txt'))
    gui = read(os.path.join(out, 'gui-L5RA.txt'))
    imported = read(os.path.join(out, 'imported-L5RA.txt'))

    # Export: the two viaduct roads only, in place.
    if len(viaduct[1]) != 2 or len(viaduct[0]) != 3:
        print(f'   FAIL export: {len(viaduct[1])} roads, {len(viaduct[0])} nodes (want 2, 3)')
        ok = False
    src_via = ({k: v for k, v in source[0].items() if k >= 900}, [s for s in source[1] if s[0] >= 900])
    _, good = match(src_via, viaduct, {s[0] for s in viaduct[1]}, 'export from UG2 (viaduct)')
    ok &= good
    z = sorted(v[0][2] for v in viaduct[0].values())
    print(f'   {"ok  " if z == [0.0, 0.0, 8.0] else "FAIL"} export heights {z}')
    ok &= z == [0.0, 0.0, 8.0]

    # Import: Gui's roads untouched, both files' roads added.
    gui_ids = {s[0] for s in gui[1]}
    for s in gui[1]:
        o = [t for t in imported[1] if t[0] == s[0]]
        if not o or o[0][1:3] != s[1:3] or not same_tail(o[0][3], s[3]):
            print(f'   FAIL Gui road {s[0]} changed or gone')
            ok = False
    new_ids = {s[0] for s in imported[1]} - gui_ids
    want = len(viaduct[1]) + len(original[1])
    print(f'   {"ok  " if len(new_ids) == want else "FAIL"} {len(new_ids)} new roads in NFS World (want {want}); '
          f'{len(gui_ids)} of Gui kept')
    ok &= len(new_ids) == want
    used, good = match(viaduct, imported, new_ids, 'import of the export (viaduct)')
    ok &= good
    _, good = match(original, imported, new_ids - used, "import of the original editor's roads-L4RA.txt")
    ok &= good
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
