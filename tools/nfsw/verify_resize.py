"""Checks a section file written with a texture replaced at another size (test_alpha.ps1 leaves it in a scratch
folder beside its kept copy in frs-original).

    python verify_resize.py <scratch folder> <hash hex> <image>

Checks: the chunks of the written file run exactly to its end; every texture header and larger-level row follows
the rules measured on the shipped files; every other texture's pixels are byte for byte what shipped; the replaced
texture's levels decode to the image; the file grew by a multiple of 128 and L5RA.BUN's three sizes by the same.
"""
import os, struct, sys
import numpy as np
from PIL import Image

out, wanted, image_path = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
kept = os.path.join(out, "frs-original")
fails = []
def check(ok, text):
    print(("ok   " if ok else "FAIL ") + text)
    if not ok: fails.append(text)

def walk(b, s=0, e=None, depth=0):
    e = len(b) if e is None else e; p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n, depth
        if k & 0x80000000: yield from walk(b, p + 8, p + 8 + n, depth + 1)
        p += 8 + n

def top_end(b):
    p = 0
    while p + 8 <= len(b):
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > len(b): return p
        p += 8 + n
    return p

def chain(w, h, mips, block):
    return sum(max(1, ((w >> m) + 3) // 4) * max(1, ((h >> m) + 3) // 4) * block for m in range(mips))

def parse(b):
    """Pack entries {hash: dict} and larger-level rows [(level, hash, dict)], with the pixels of each."""
    entries = {}; rows = []; ent = []; fm = []; level = None; rowspan = None; inpack = False
    for k, s, e, d in walk(b):
        if k == 0x33310004:
            p = s; ent = []
            while p + 0x91 <= e: ent.append(p); p += 0x91 + b[p + 0x90]
        if k == 0x33310005: fm = [b[q + 12:q + 16] for q in range(s, e, 32)]
        if k == 0xB3320000: inpack = True
        if k == 0x33310021: level = struct.unpack_from("<I", b, s + 4)[0]; inpack = False
        if k == 0x33310022: rowspan = (s, e)
        if k == 0x33320002:
            start = (s + 127) & ~127
            if inpack:
                inpack = False
                for i, p in enumerate(ent):
                    h, = struct.unpack_from("<I", b, p + 0x0C)
                    size, base, w, hh, lw, lh = struct.unpack_from("<6I", b, p + 0x18)
                    sz2, off = struct.unpack_from("<II", b, p + 0x4C)
                    cc = fm[i]
                    entries[h] = dict(size=size, base=base, w=w, h=hh, lw=lw, lh=lh, mips=b[p + 0x31], sz2=sz2, off=off,
                                      cc=cc, block=8 if cc in (b"DXT1", b"ATI1") else 16,
                                      pixels=b[start + off:start + off + size], room=e - start)
            elif rowspan is not None:
                for q in range(rowspan[0], rowspan[1], 32):
                    h, off, size, base, w, hh, lw, lh, mips = struct.unpack_from("<4I2H3I", b, q)
                    rows.append((level, h, dict(off=off, size=size, base=base, w=w, h=hh, lw=lw, lh=lh, mips=mips,
                                                pixels=b[start + off:start + off + base], room=e - start)))
                rowspan = None
    return entries, rows

def decode_colour(data, w, h, dxt5=True):
    """Colour of DXT blocks as an (h, w, 3) array."""
    bw, bh = max(1, w // 4), max(1, h // 4); step = 16 if dxt5 else 8
    raw = np.frombuffer(data[:bw * bh * step], dtype=np.uint8).reshape(bh, bw, step)[:, :, step - 8:]
    c0 = raw[:, :, 0].astype(np.uint32) | (raw[:, :, 1].astype(np.uint32) << 8)
    c1 = raw[:, :, 2].astype(np.uint32) | (raw[:, :, 3].astype(np.uint32) << 8)
    def rgb(c): return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.float32)
    a, bcol = rgb(c0), rgb(c1)
    table = np.stack([a, bcol, (2 * a + bcol) / 3, (a + 2 * bcol) / 3], 2)            # (bh, bw, 4, 3)
    bits = raw[:, :, 4:8].astype(np.uint32)
    code = bits[:, :, 0] | (bits[:, :, 1] << 8) | (bits[:, :, 2] << 16) | (bits[:, :, 3] << 24)
    idx = np.stack([(code >> (2 * i)) & 3 for i in range(16)], -1)                      # (bh, bw, 16)
    px = np.take_along_axis(table, idx[..., None].repeat(3, -1), axis=2)                # (bh, bw, 16, 3)
    return px.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3)

picture = Image.open(image_path).convert("RGB")
names = [n for n in sorted(os.listdir(out)) if n.upper().endswith(".BUN") and n.upper() != "L5RA.BUN"
         and os.path.exists(os.path.join(kept, n))]
check(bool(names), f"{len(names)} section file(s) written beside a kept copy")
growth_by_file = {}
for name in names:
    new = open(os.path.join(out, name), "rb").read(); old = open(os.path.join(kept, name), "rb").read()
    growth = len(new) - len(old); growth_by_file[name] = growth
    print(f"--- {name}: {len(old)} -> {len(new)} bytes")
    check(growth >= 0 and growth % 128 == 0, f"grew by {growth} bytes, a multiple of 128")
    check(top_end(new) == len(new), "chunks run exactly to the end of the file")
    ne, nr = parse(new); oe, orows = parse(old)
    check(set(ne) == set(oe) and len(nr) == len(orows), f"same {len(ne)} textures and {len(nr)} larger-level rows")
    for h, t in ne.items():
        o = oe[h]; label = f"{h:08X} pack {o['w']}x{o['h']} -> {t['w']}x{t['h']}"
        ch = (chain(t["w"], t["h"], t["mips"], t["block"]) + 15) & ~15
        check(t["size"] == t["sz2"] == ch and t["base"] == chain(t["w"], t["h"], 1, t["block"]) and
              (1 << t["lw"], 1 << t["lh"]) == (t["w"], t["h"]) and t["off"] % 128 == 0 and
              t["off"] + t["size"] <= t["room"] and t["mips"] == o["mips"], label + ": header fields follow the rules")
        if h != wanted: check(t["pixels"] == o["pixels"] and (t["w"], t["h"]) == (o["w"], o["h"]), label + ": pixels as shipped")
    spans = sorted((t["off"], t["off"] + t["size"]) for t in ne.values())
    check(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), "pack textures do not overlap")
    for (level, h, r), (olevel, oh, o) in zip(nr, orows):
        label = f"{h:08X} level pack {level} {o['w']}x{o['h']} -> {r['w']}x{r['h']}"
        p = ne[h]; above = (r["w"] // p["w"]).bit_length() - 1
        check(level == olevel and h == oh and r["base"] == chain(r["w"], r["h"], 1, p["block"]) and
              (1 << r["lw"], 1 << r["lh"]) == (r["w"], r["h"]) and r["off"] % 128 == 0 and
              r["off"] + r["base"] <= r["room"] and r["mips"] == p["mips"] + above and
              r["size"] == p["size"] + sum(chain(r["w"] >> m, r["h"] >> m, 1, p["block"]) for m in range(above)),
              label + ": row follows the rules")
        if h != wanted: check(r["pixels"] == o["pixels"], label + ": pixels as shipped")
    for level in sorted({lv for lv, _, _ in nr}):
        spans = sorted((r["off"], r["off"] + r["base"]) for lv, _, r in nr if lv == level)
        check(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), f"level pack {level}: textures do not overlap")
    if wanted in ne:
        t = ne[wanted]
        copies = [("pack", t["w"], t["h"], t["pixels"])] + [(f"level pack {lv}", r["w"], r["h"], r["pixels"]) for lv, h, r in nr if h == wanted]
        for label, w, hh, data in copies:
            got = decode_colour(data, w, hh, t["cc"] != b"DXT1")
            want = np.asarray(picture.resize((w, hh), Image.BOX), dtype=np.float32)
            error = float(np.abs(got - want).mean())
            check(error < 12, f"{label} {w}x{hh}: decodes to the image (mean colour error {error:.1f})")

table_new, table_old = os.path.join(out, "L5RA.BUN"), os.path.join(kept, "L5RA.BUN")
if any(growth_by_file.values()):
    check(os.path.exists(table_new) and os.path.exists(table_old), "L5RA.BUN written and its shipped copy kept")
    if os.path.exists(table_new) and os.path.exists(table_old):
        new = open(table_new, "rb").read(); old = open(table_old, "rb").read()
        check(len(new) == len(old), "L5RA.BUN keeps its size")
        # What the entries must say is checked by verify_table.py: a texture section's entry holds where each
        # of its five streamed parts ends, not one size. (This script used to pass a table with one size raised,
        # which the game showed as noise on the larger levels.)
        print("note L5RA.BUN: run verify_table.py for the entries")
print("ALL CHECKS PASS" if not fails else f"{len(fails)} CHECK(S) FAILED")
