"""Every physical prop (chunk 0x34027) of a written section must stand where its scenery record stands.

Usage: verify_props.py [--table <L5RA.BUN>] <written STREAML5RA_<n>.BUN> [...]
With --table, each prop's id must also have a 0x34108 entry naming its container and record.
Prop: 64 bytes after 8 bytes, +0x08 quaternion, +0x18 position in collision axes (x = -game y, y = up, z = game x),
+0x30 id; the id names (scenery number, record) through the container's 0x34106 rows (u16 id, u16 record).
Only the header's count (i16 at +0xA) is read, as the game does (0x6F6730). A prop whose record is
zeroed (removed) fails: the game would still spawn it.
"""
import math
import struct
import sys


TABLE = None


def check(path):
    d = open(path, 'rb').read()
    conts, props = {}, []

    def walk(o, end, top):
        while o + 8 <= end:
            cid, sz = struct.unpack_from('<II', d, o)
            if top and cid == 0x34027:
                props.append((o + 8, sz))
            if cid == 0x80034100:
                p, kids = o + 8, {}
                while p + 8 <= o + 8 + sz:
                    k, s = struct.unpack_from('<II', d, p)
                    kids[k] = d[p + 8:p + 8 + s]
                    p += 8 + s
                conts[struct.unpack_from('<I', kids[0x34101].lstrip(b'\x11'), 12)[0]] = kids
            elif cid & 0x80000000:
                walk(o + 8, o + 8 + sz, False)
            o += 8 + sz

    walk(0, len(d), True)
    ids = {}
    for num, kids in conts.items():
        rows = kids.get(0x34106, b'')
        for i in range(0, len(rows) - 3, 4):
            a, b = struct.unpack_from('<2H', rows, i)
            ids[a] = (num, b)
    checked = bad = 0
    for off, sz in props:
        first = off
        while d[first] == 0x11:
            first += 1
        count = max(struct.unpack_from('<h', d, first + 10)[0], 0)
        for e in range(first + 8, min(first + 8 + count * 64, off + sz - 63), 64):
            idv = struct.unpack_from('<I', d, e + 0x30)[0]
            if idv not in ids:
                continue
            num, r = ids[idv]
            rec = conts[num][0x34103].lstrip(b'\x11')
            if not any(struct.unpack_from('<9f', rec, r * 96 + 0x2C)):
                bad += 1
                print(f'  {path}: prop {idv} (scenery {num} record {r}) stays for a removed record')
                continue
            t = struct.unpack_from('<3f', rec, r * 96 + 0x20)
            p = struct.unpack_from('<3f', d, e + 0x18)
            q = struct.unpack_from('<4f', d, e + 0x08)
            checked += 1
            if TABLE is not None:
                if (idv + 1) * 6 > len(TABLE) or struct.unpack_from('<2H', TABLE, idv * 6) != (num, r):
                    bad += 1
                    print(f'  {path}: prop {idv} (scenery {num} record {r}) has no matching 0x34108 entry')
                    continue
            if math.dist((p[2], -p[0], p[1]), t) > 0.01 or abs(math.sqrt(sum(c * c for c in q)) - 1) > 1e-3:
                bad += 1
                print(f'  {path}: prop {idv} (scenery {num} record {r}) at {[round(v, 2) for v in (p[2], -p[0], p[1])]}, '
                      f'record at {[round(v, 2) for v in t]}')
    return checked, bad


if __name__ == '__main__':
    total = fails = 0
    args = sys.argv[1:]
    if args[:1] == ['--table']:
        region = open(args[1], 'rb').read()
        o = 0
        while o + 8 <= len(region):
            cid, sz = struct.unpack_from('<II', region, o)
            if cid == 0x34108:
                TABLE = region[o + 8:o + 8 + sz]
            o += 8 + sz
        args = args[2:]
    for path in args:
        c, b = check(path)
        total += c
        fails += b
    print(f'{total} props checked, {fails} away from their record:', 'PASS' if fails == 0 else 'FAIL')
