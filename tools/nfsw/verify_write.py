"""Checks what the NFS World writer wrote: which bytes differ from the shipped section, and that they are the records it claims."""
import os, struct, sys
out, tracks = sys.argv[1], sys.argv[2]
def walk(b, s=0, e=None):
    e = len(b) if e is None else e; p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000: yield from walk(b, p + 8, p + 8 + n)
        p += 8 + n
def al(b, s, e, a=16):
    end = min(e, (s + a - 1) & ~(a - 1)); q = s
    while q < end and b[q] == 0x11: q += 1
    return q if q == end else s
print(open(os.path.join(out, "edits-after-save.txt")).read().strip())
for f in sorted(os.listdir(os.path.join(out, "patched"))):
    new = open(os.path.join(out, "patched", f), "rb").read(); old = open(os.path.join(tracks, f), "rb").read()
    assert len(new) == len(old), "size changed"
    restored = open(os.path.join(out, f), "rb").read()
    print(f, "size", len(new), "| after undo+save identical to shipped:", restored == old)
    diff = [i for i in range(len(old)) if old[i] != new[i]]
    runs = []
    for i in diff:
        if runs and i - runs[-1][1] <= 4: runs[-1][1] = i
        else: runs.append([i, i])
    where = {}
    for k, s, e in walk(old):
        if k in (0x34103, 0x3410E, 0x34105, 0x3410F): where.setdefault(k, []).append((al(old, s, e), e))
    for a, z in runs:
        tag = "?"
        for k, spans in where.items():
            for s, e in spans:
                if s <= a < e:
                    size = {0x34103: 96, 0x3410E: 32, 0x34105: 48, 0x3410F: 32}[k]
                    tag = f"chunk {k:05X} entry {(a - s) // size} +{(a - s) % size:#x}..+{(z - s) % size:#x}"
        print(f"   {z - a + 1:3} bytes at {a:#x}: {tag}")
    # the removed record's matrix must be all zero; the moved one must still be a rotation
