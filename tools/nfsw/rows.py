import os, struct, sys
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
    if (k & 0xFFFFFF00) == 0x00034100 and k not in ch: ch[k] = (al(s, e), e, s)
rq, re_, _ = ch[0x34103]; n = (re_ - rq) // 96
lq, le, _ = ch[0x3410F]; rows = [struct.unpack_from("<II", b, lq + 32 * i + 16) for i in range((le - lq) // 32)]
flags = [struct.unpack_from("<I", b, rq + 96 * i + 12)[0] for i in range(n)]
print("records", n, "rows", len(rows))
print("rows 110..:", [r[0] for r in rows[110:]])
# split into groups where the record index drops
groups = [[]]
for i, (a, info) in enumerate(rows):
    if groups[-1] and a <= groups[-1][-1]: groups.append([])
    groups[-1].append(a)
print("group sizes", [len(g) for g in groups][:12])
for gi, g in enumerate(groups[1:5], 1):
    inset = set(g); bits = [bit for bit in range(32) if all((flags[a] >> bit) & 1 for a in g) and not any((flags[a] >> bit) & 1 for a in range(n) if a not in inset)]
    print(f"group {gi}: {len(g)} records; flag bits set exactly for its members: {bits}")
print("flag values:", sorted({hex(x) for x in flags}))
# record tail fields and 3410D
print("record +0x54..+0x60 distinct:", sorted({b[rq + 96 * i + 0x54: rq + 96 * i + 0x60].hex() for i in range(n)})[:6])
hq, he, hs = ch[0x34101]; print("header raw (from chunk start):", b[hs:he].hex(" ", 4))
