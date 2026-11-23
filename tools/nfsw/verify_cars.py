"""Checks a placed car as test_cars.ps1 left it written: verify_cars.py <step folder> <model name>.
The section file holding the model must have, before the solid list that stores it, a 0x135200 light material named
FRS_PAINT_<hash> (hash = bStringHash of the name), and the model's 0x134013 table must name that hash; the model's hash
must be among the pack's sorted 0x134003 rows; every texture it draws with must be in some texture pack written there
or be a GlobalB one (listed, not failed). Prints the paint colour and PASS/FAIL."""
import struct, sys
from pathlib import Path

U = lambda b, p: struct.unpack_from('<I', b, p)[0]


def bh(s):
    h = 0xFFFFFFFF
    for c in s.encode(): h = (h * 33 + c) & 0xFFFFFFFF
    return h


def walk(b, s=0, e=None, depth=0):
    e = len(b) if e is None else e; p = s
    while p + 8 <= e:
        k, n = struct.unpack_from('<II', b, p)
        if p + 8 + n > e: break
        yield k, p, p + 8, p + 8 + n, depth
        if k & 0x80000000: yield from walk(b, p + 8, p + 8 + n, depth + 1)
        p += 8 + n


def payload(b, s):   # skip the 0x11 filler to 16
    q = s
    while q < ((s + 15) & ~15) and b[q] == 0x11: q += 1
    return q


def main():
    folder, name = Path(sys.argv[1]), sys.argv[2]
    model = bh(name); ok = True
    files = [f for f in folder.rglob('*.BUN')]
    hits = [f for f in files if name.encode() in f.read_bytes()]
    if not hits: print('FAIL: no file holds', name); return 1
    f = hits[0]; b = f.read_bytes(); print('file', f.name, len(b))
    paint = 'FRS_PAINT_%08X' % model
    top = [(k, p, s, e) for k, p, s, e, d in walk(b) if d == 0]
    mats = [(p, s, e) for k, p, s, e in top if k == 0x135200 and b[s + 0x14:s + 0x34].split(b'\0')[0].decode() == paint]
    if len(mats) != 1: print('FAIL: light material', paint, 'found', len(mats), 'times'); return 1
    mp, ms, me = mats[0]
    fl = lambda i: struct.unpack_from('<f', b, ms + 0x34 + 4 * i)[0]
    print('paint %s hash %08X (name hash %s) rgb %.3f %.3f %.3f envmap %.2f/%.2f flakes %.2f size %d' % (
        paint, U(b, ms + 0xC), 'ok' if U(b, ms + 0xC) == bh(paint) else 'WRONG', fl(14), fl(15), fl(16), fl(34), fl(37), fl(24), me - ms))
    ok &= U(b, ms + 0xC) == bh(paint) and me - ms == 252
    packs = [(p, s, e) for k, p, s, e in top if k == 0x80134000 and name.encode() in b[s:e]]
    if not packs: print('FAIL: no solid list holds the model'); return 1
    pp, ps, pe = packs[0]
    print('light material at', mp, 'solid list at', pp, '(before: %s)' % (mp < pp))
    ok &= mp < pp
    rows = [U(b, q) for k, p, s, e, d in walk(b, ps, pe) if k == 0x134003 for q in range(payload(b, s), e, 8)]
    print('pack rows', len(rows), 'sorted', rows == sorted(rows), 'model listed', model in rows)
    ok &= rows == sorted(rows) and model in rows
    for k, p, s, e, d in walk(b, ps, pe):
        if k != 0x80134010: continue
        objs = list(walk(b, s, e))
        hd = [x for x in objs if x[0] == 0x134011][0]; hp = payload(b, hd[2])
        if b[hp + 0xA0:hp + 0xA0 + len(name) + 1] != name.encode() + b'\0': continue
        lights = [U(b, q) for x in objs if x[0] == 0x134013 for q in range(x[2], x[3], 8)]
        tex = [U(b, q) for x in objs if x[0] == 0x134012 for q in range(x[2], x[3], 8)]
        nmat = sum((x[3] - payload(b, x[2])) // 116 for x in objs if x[0] == 0x134B02)
        print('model %08X: %d materials, %d textures, %d light materials, paint named: %s, header counts %s' % (
            U(b, hp + 0x10), nmat, len(tex), len(lights), bh(paint) in lights, list(b[hp + 0x18:hp + 0x1C])))
        ok &= bh(paint) in lights and b[hp + 0x19] == len(tex) and b[hp + 0x1A] == len(lights) and U(b, hp + 0x10) == model
        written = set()
        for g in files:
            gb = g.read_bytes()
            for k2, p2, s2, e2, d2 in walk(gb):
                if k2 == 0x33310002: written |= {U(gb, q) for q in range(s2, e2, 8)}
        missing = [t for t in tex if t not in written]
        print('textures in written packs: %d of %d; not there (GlobalB or shipped elsewhere): %s' % (
            len(tex) - len(missing), len(tex), ' '.join('%08X' % t for t in missing)))
        break
    else:
        print('FAIL: model object not found'); return 1
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
