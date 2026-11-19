"""Structure of the scenery chunks of one section: sizes against counts, and what the unknown tables hold."""
import os, struct, sys
T, f = sys.argv[1], sys.argv[2]
b = open(os.path.join(T, f), "rb").read()
def walk(s=0, e=None, d=0):
    e = len(b) if e is None else e; p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n, d
        if k & 0x80000000: yield from walk(p + 8, p + 8 + n, d + 1)
        p += 8 + n
def al(s, e, a=16):
    end = min(e, (s + a - 1) & ~(a - 1)); q = s
    while q < end and b[q] == 0x11: q += 1
    return q if q == end else s
top = [(k, s, e) for k, s, e, d in walk() if d == 0 and k != 0]
print("top-level order:", [f"{k:08X}:{e - s}" for k, s, e in top])
for k, s, e, d in walk():
    if k == 0x80034100: print(f"--- scenery container at {s - 8:#x}, {e - s} bytes"); n = links = 0
    if d != 1 or (k & 0xFFFFFF00) != 0x00034100: continue
    q = al(s, e); size = e - q
    if k == 0x34101:
        print(f"  34101 header {size}: " + b[q:q + 84].hex(" ", 4))
    elif k == 0x34103:
        n = size // 96; print(f"  34103 records: {n} (pad {q - s}, rem {size % 96})")
    elif k == 0x34102:
        m = size // 72; print(f"  34102 infos: {m} (rem {size % 72})")
    elif k == 0x3410D:
        print(f"  3410D: {size} bytes = {size / 32:.2f} x 32; per record {size / max(n, 1):.2f}; pad {q - s}")
    elif k == 0x3410E:
        rows = size // 32; nz = sum(1 for i in range(rows) if any(b[q + 32 * i:q + 32 * i + 32]))
        print(f"  3410E: {rows} rows, {nz} non-zero; row n..n+2:", [b[q + 32 * i:q + 32 * i + 32].hex(" ", 4) for i in range(n, min(n + 2, rows))])
        w = [struct.unpack_from("<f", b, q + 32 * i + 12)[0] for i in range(min(n, rows))]
        print(f"     4th float of first rows: {[round(x, 2) for x in w[:6]]}")
    elif k == 0x3410F:
        rows = size // 32; used = sum(1 for i in range(rows) if any(b[q + 32 * i + 16:q + 32 * i + 32]))
        print(f"  3410F: {rows} rows, {used} with numbers (+1 for row 0); pad {q - s}")
    elif k == 0x34105:
        nodes = size // 48; cnt = [struct.unpack_from("<I", b, q + 48 * i + 12)[0] for i in range(nodes)]
        leaves = sum(1 for i in range(nodes) for c in range(min(cnt[i], 10)) if struct.unpack_from("<h", b, q + 48 * i + 0x1C + 2 * c)[0] >= 0)
        mx = max(struct.unpack_from("<h", b, q + 48 * i + 0x1C + 2 * c)[0] for i in range(nodes) for c in range(min(cnt[i], 10)))
        print(f"  34105 nodes: {nodes}, child counts {sorted(set(cnt))}, leaf refs {leaves}, largest leaf index {mx}")
    elif k == 0x34106:
        pairs = [struct.unpack_from("<HH", b, q + 4 * i) for i in range(size // 4)]
        print(f"  34106: {len(pairs)} pairs; second values {sorted(set(p[1] for p in pairs))[:12]}...; first {pairs[:6]}")
    elif k == 0x34107:
        print(f"  34107: {size} bytes, non-zero {sum(1 for x in b[q:e] if x)}")
    else:
        print(f"  {k:05X}: {size} bytes")
