"""Measure NFS World visibility zones in L5RA.BUN (container 0x80034150). Records start at the 8-byte marker
0B000000 0B000000 (on 4-byte steps) like Underground 2, but the bodies differ (measured here):
  0x34152 outline body: u16 id, u8 points, u8 flag, 12 bytes (zero in 1046 of 1051), min xy @16, max xy @24, middle xy @32, points * (x, y) @40
  0x34153 list body:    u32 @0, u16 id @4, u16 count @6, u16 @8, u16 @10, u16 section numbers from +12
Checks: list numbers against the section table (0x34110, 208-byte rows, number @0x08, centre @0x28), and
whether a zone's outline holds the centre of the section with its own number.
Usage: vis_stats.py <L5RA.BUN>"""
import struct
import sys
from collections import Counter

MARK = bytes([11, 0, 0, 0, 11, 0, 0, 0])
buf = open(sys.argv[1], "rb").read()


def top(cid_want):
    pos = 0
    while pos + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, pos)
        if cid == cid_want:
            return pos + 8, size
        pos += 8 + size


def children(lo, size):
    out, pos = {}, lo
    while pos + 8 <= lo + size:
        cid, n = struct.unpack_from("<II", buf, pos)
        out[cid] = (pos + 8, n)
        pos += 8 + n
    return out


def bodies(lo, size):
    starts = [p for p in range(lo, lo + size - 7, 4) if buf[p:p + 8] == MARK]
    return [(s + 8, (starts[i + 1] if i + 1 < len(starts) else lo + size) - s - 8) for i, s in enumerate(starts)]


def inside(ring, x, y):
    c = False
    for i in range(len(ring)):
        (ax, ay), (bx, by) = ring[i], ring[i - 1]
        if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
            c = not c
    return c


lo, size = top(0x34110)
table = {}
for at in range(lo, lo + size - 207, 208):
    num = struct.unpack_from("<I", buf, at + 8)[0]
    table[num] = struct.unpack_from("<3f", buf, at + 0x28)

kids = children(*top(0x80034150))
print("children:", {hex(k): v[1] for k, v in kids.items()})
print("0x34151 words:", struct.unpack_from("<8I", buf, kids[0x34151][0]))
zones = {}
lens, zeros, boxok, w8 = Counter(), Counter(), Counter(), Counter()
for b, n in bodies(*kids[0x34152]):
    zid, pts = struct.unpack_from("<HB", buf, b)
    lens[n - (40 + pts * 8)] += 1
    zeros[buf[b + 4:b + 16] == bytes(12)] += 1
    mn, mx = struct.unpack_from("<2f", buf, b + 16), struct.unpack_from("<2f", buf, b + 24)
    ring = [struct.unpack_from("<2f", buf, b + 40 + i * 8) for i in range(pts)]
    boxok[all(mn[k] - 0.01 <= min(p[k] for p in ring) and max(p[k] for p in ring) <= mx[k] + 0.01 for k in (0, 1))] += 1
    zones[zid] = (mn, mx, ring)
lists = {}
for b, n in bodies(*kids[0x34153]):
    zid, count, a, c = struct.unpack_from("<4H", buf, b + 4)
    w8[(a - count, c == count)] += 1
    lists[zid] = struct.unpack_from("<%dH" % count, buf, b + 12) if 12 + 2 * count <= n else None
print(f"outlines {len(zones)}, lists {len(lists)}, bad lists {sum(v is None for v in lists.values())}")
print("outline slack bytes after ring (top):", lens.most_common(4), " 12 zero bytes:", dict(zeros), " ring inside box:", dict(boxok))
print("list (u16@8 - count, u16@10 == count):", w8.most_common(4))
both = set(zones) & set(lists)
print(f"zones with outline and list {len(both)}")
nums = [s for v in lists.values() if v for s in v]
print(f"list entries {len(nums)}, sizes min/max {min(len(v) for v in lists.values() if v)}/{max(len(v) for v in lists.values() if v)},"
      f" numbers not in table {sum(1 for s in nums if s not in table)}")
own = hold = 0
for zid, (mn, mx, ring) in zones.items():
    if zid in table:
        own += 1
        x, y, _ = table[zid]
        hold += inside(ring, x, y)
print(f"zone ids that are table numbers {own}; their outline holds that section's centre: {hold}")
xs = [p[0] for z in zones.values() for p in z[2]]
ys = [p[1] for z in zones.values() for p in z[2]]
tx = [c[0] for c in table.values()]
ty = [c[1] for c in table.values()]
print(f"section table centres x {min(tx):.0f}..{max(tx):.0f} y {min(ty):.0f}..{max(ty):.0f}")
print(f"outline extent x {min(xs):.0f}..{max(xs):.0f} y {min(ys):.0f}..{max(ys):.0f}")
z = sorted(both)[len(both) // 2]
print("sample zone", z, "box", zones[z][0], zones[z][1], "points", len(zones[z][2]), "list", lists[z][:12])
