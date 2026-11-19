"""Reads what test_alpha.ps1 left in a scratch folder.

For every section file written beside its kept copy (frs-original): the range that changed is taken as DXT5 blocks
and the alpha of both is decoded. Kept alpha shows as a small alpha difference beside colour blocks that nearly all
changed. Every PNG saved there has its alpha range printed.

    python verify_alpha.py <scratch folder>
"""
import os, struct, sys
from PIL import Image

def alpha_of(block):
    a0, a1 = block[0], block[1]
    bits = int.from_bytes(block[2:8], "little")
    if a0 > a1: table = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:       table = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    return [table[(bits >> (3 * i)) & 7] for i in range(16)]

out = sys.argv[1]
kept = os.path.join(out, "frs-original")
for name in sorted(os.listdir(out)):
    if not name.upper().endswith(".BUN") or not os.path.exists(os.path.join(kept, name)): continue
    new = open(os.path.join(out, name), "rb").read()
    old = open(os.path.join(kept, name), "rb").read()
    if len(new) != len(old): print(name, "size changed"); continue
    diff = [i for i in range(len(new)) if new[i] != old[i]]
    if not diff: print(name, "identical"); continue
    # Runs of change, apart by more than 4 KiB, are separate stored copies. The block grid of each is tried at
    # both phases a 16-byte block can have against the file; the one whose alpha changed least is the real one.
    runs = [[diff[0], diff[0]]]
    for i in diff[1:]:
        if i - runs[-1][1] > 4096: runs.append([i, i])
        else: runs[-1][1] = i
    for lo, hi in runs:
        best = None
        for phase in (0, 8):
            first = ((lo - phase) & ~15) + phase; last = hi + 1
            blocks = (last - first) // 16
            if blocks <= 0: continue
            total = shipped = written = colour = 0
            for b in range(blocks):
                q = first + 16 * b
                a, c = alpha_of(old[q:q + 8]), alpha_of(new[q:q + 8])
                total += sum(abs(x - y) for x, y in zip(a, c)); shipped += sum(a); written += sum(c)
                colour += old[q + 8:q + 16] != new[q + 8:q + 16]
            n = blocks * 16
            line = (total / n, f"{name}: {blocks} blocks from {first:#x}; shipped alpha mean {shipped / n:.1f}, "
                    f"written {written / n:.1f}, mean difference {total / n:.2f}; colour blocks changed {100 * colour / blocks:.1f}%")
            if best is None or line[0] < best[0]: best = line
        if best: print(best[1])

for name in sorted(os.listdir(out)):
    if name.lower().endswith(".png") and name != "view.png":
        image = Image.open(os.path.join(out, name))
        low, high = image.getchannel("A").getextrema() if "A" in image.getbands() else (255, 255)
        print(f"{name}: {image.size[0]}x{image.size[1]} {image.mode}, alpha {low}..{high}")
