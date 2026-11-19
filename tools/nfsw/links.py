"""What the link rows (0x3410F) are: rows per record, and how the box rows (0x3410E) line up with them."""
import os, struct, sys
from collections import Counter, defaultdict
T, f = sys.argv[1], sys.argv[2]
b = open(os.path.join(T, f), "rb").read()
def walk(s=0, e=None):
    e = len(b) if e is None else e; p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000: yield from walk(p + 8, p + 8 + n)
        p += 8 + n
def al(s, e, a=16):
    end = min(e, (s + a - 1) & ~(a - 1)); q = s
    while q < end and b[q] == 0x11: q += 1
    return q if q == end else s
ch = {}
for k, s, e in walk():
    if (k & 0xFFFFFF00) == 0x00034100 and k not in ch: ch[k] = (al(s, e), e)
rq, re_ = ch[0x34103]; n = (re_ - rq) // 96
iq, ie = ch[0x34102]; infos = [(b[r:r + 24].split(b"\0")[0].decode(), struct.unpack_from("<4I", b, r + 0x18)) for r in range(iq, ie - 71, 72)]
lq, le = ch[0x3410F]; rows = [struct.unpack_from("<4I", b, lq + 32 * i + 16) for i in range((le - lq) // 32)]
bq, be = ch[0x3410E]
hq, he = ch[0x3410D]
by = defaultdict(list)
for r, (a, info, c, d) in enumerate(rows):
    if r and not any((a, info, c, d)): continue
    by[a].append((r, info, c, d))
print("records", n, "rows used", sum(len(v) for v in by.values()), "rows per record:", dict(Counter(len(v) for v in by.values())))
print("field2 == field0 in all rows:", all(c == a for a, v in by.items() for _, _, c, _ in v), "| field3 values:", sorted(set(d for v in by.values() for *_, d in v))[:6])
for a in list(by)[:3] + [k for k, v in by.items() if len(v) > 2][:2]:
    rec = b[rq + 96 * a: rq + 96 * a + 96]
    print(f"record {a}: flags {rec[12:16].hex()} hash@50 {rec[0x50:0x54].hex()} +54 {rec[0x54:0x60].hex(' ', 4)}")
    print("   3410D:", b[hq + 32 * a: hq + 32 * a + 32].hex(" ", 4))
    for r, info, c, d in by[a]:
        box = struct.unpack_from("<8f", b, bq + 32 * r)
        same = b[bq + 32 * r: bq + 32 * r + 12] == rec[0:12]
        print(f"   row {r}: info {info} {infos[info][0] if info < len(infos) else '?'} | box min {[round(x, 1) for x in box[:3]]} w {box[3]:.2f} | box == record box: {same}")
# does a row's box differ from its record's box?
diff = sum(1 for a, v in by.items() for r, *_ in v if b[bq + 32 * r: bq + 32 * r + 12] != b[rq + 96 * a: rq + 96 * a + 12])
print("rows whose box differs from the record's:", diff)
print("row order: record index non-decreasing:", all(rows[i][0] <= rows[i + 1][0] for i in range(sum(len(v) for v in by.values()) - 1)))
