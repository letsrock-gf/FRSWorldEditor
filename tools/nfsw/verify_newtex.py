"""Checks the texture sections a save wrote new textures into (io/WorldTexture.cpp WriteNewTextures), against the
shipped files. Every texture section file in <written> that differs from <shipped>:
  - pack tables: hash table sorted, entry +4 zero, as many headers (0x91 + length byte) and formats (32) as hashes,
    header i's hash = hash i, length byte = name + NUL made up so the header ends on 4, format = FourCC only;
  - pixels: data on a 128-byte file boundary, every texture's offset 128-aligned, inside the data, none overlapping;
  - shipped textures: header and pixels byte for byte as shipped; everything after the pack as shipped (moved);
  - new ones: name FRS_*, hash = bStringHash(name), power-of-two sides <= 512, levels down to 4 px, size fields;
  - the file grew by a multiple of 128, and its L5RA.BUN entry has its five part ends where the parts end now.
Usage: verify_newtex.py <written TRACKS folder> <shipped TRACKS folder (Data2015\\TRACKS)> [--edits <edit list>]"""
import os
import struct
import sys


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def children(b, s, e):
    out = []
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            break
        out.append((k, p, p + 8, p + 8 + n))
        p += 8 + n
    return out


def find(nodes, k):
    return next((n for n in nodes if n[0] == k), None)


def bhash(s):
    h = 0xFFFFFFFF
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def chain(w, h, mips, block):
    return sum(max(1, (w >> m) + 3 >> 2) * max(1, (h >> m) + 3 >> 2) * block for m in range(mips))


def pack_of(b):
    top = children(b, 0, len(b))
    pk = find(top, 0xB3300000)
    parts = children(b, pk[2], pk[3])
    info, data = find(parts, 0xB3310000), find(parts, 0xB3320000)
    ip = children(b, info[2], info[3])
    dp = children(b, data[2], data[3])
    ht, hd, fm, px = find(ip, 0x33310002), find(ip, 0x33310004), find(ip, 0x33310005), dp[-1]
    hashes = [u32(b, q) for q in range(ht[2], ht[3], 8)]
    zeros = all(u32(b, q + 4) == 0 for q in range(ht[2], ht[3], 8))
    heads = []
    p = hd[2]
    while p + 0x91 <= hd[3]:
        heads.append(b[p:p + 0x91 + b[p + 0x90]])
        p += 0x91 + b[p + 0x90]
    fmts = [b[q:q + 32] for q in range(fm[2], fm[3], 32)]
    start = (px[2] + 127) & ~127
    return dict(pack=pk, hashes=hashes, zeros=zeros, heads=heads, headEnd=p == hd[3], fmts=fmts, start=start,
                pixEnd=px[3], b=b)


def streamed_parts(b):
    ends, last = [], 0
    for k, h, s, e in children(b, 0, len(b)):
        if k == 0xB3300011:
            if not ends:
                ends.append(last)
            ends.append(e)
        elif k != 0 and not ends:
            last = e
    return ends


def main():
    written, shipped = sys.argv[1], sys.argv[2]
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)
        print(("  ok   " if ok else "  FAIL ") + what)

    table_b = open(os.path.join(written, "L5RA.BUN"), "rb").read() if os.path.exists(os.path.join(written, "L5RA.BUN")) else None
    ship_table = open(os.path.join(shipped, "frs-original", "L5RA.BUN") if os.path.exists(
        os.path.join(shipped, "frs-original", "L5RA.BUN")) else os.path.join(shipped, "L5RA.BUN"), "rb").read()
    entries = {}
    for tb in [table_b] if table_b else []:
        for k, h, s, e in children(tb, 0, len(tb)):
            if k == 0x34110:
                for q in range(s, e, 208):
                    entries[u32(tb, q + 0x1C)] = q
    ship_entries = {}
    for k, h, s, e in children(ship_table, 0, len(ship_table)):
        if k == 0x34110:
            for q in range(s, e, 208):
                ship_entries[u32(ship_table, q + 0x1C)] = q

    def shipped_file(name):
        kept = os.path.join(shipped, "frs-original", name)
        return open(kept if os.path.exists(kept) else os.path.join(shipped, name), "rb").read()

    sources = {}

    def source_of(h):
        # The shipped header and pack pixels of texture h, from the texture sections of <shipped>.
        if not sources:
            for f in sorted(os.listdir(shipped)):
                if not ((f.startswith("STREAML5RA_0x") or f.startswith("STREAML5RA_2500")) and f.endswith(".BUN")):
                    continue
                b = shipped_file(f)
                if not find(children(b, 0, len(b)), 0xB3300000):
                    continue
                pk = pack_of(b)
                for hd in pk["heads"]:
                    sources.setdefault(u32(hd, 0x0C), (hd, b[pk["start"] + u32(hd, 0x50):pk["start"] + u32(hd, 0x50) + u32(hd, 0x4C)]))
        return sources.get(h)

    seen = 0
    added_hashes = set()
    made = []
    for name in sorted(os.listdir(written)):
        if not (name.startswith("STREAML5RA_0x") and name.endswith(".BUN")):
            continue
        nb = open(os.path.join(written, name), "rb").read()
        ob = shipped_file(name)
        if nb == ob:
            continue
        if not find(children(ob, 0, len(ob)), 0xB3300000):
            continue   # a library section (models), not a texture pack
        n, o = pack_of(nb), pack_of(ob)
        if set(n["hashes"]) == set(o["hashes"]):
            continue   # textures replaced in place, none added (verify_resize.py)
        seen += 1
        print(name, len(ob), "->", len(nb))
        check((len(nb) - len(ob)) % 128 == 0, f"file grew by {len(nb) - len(ob)}, a multiple of 128")
        check(n["hashes"] == sorted(n["hashes"]) and n["zeros"], "hash table sorted, +4 zero")
        check(len(n["heads"]) == len(n["hashes"]) == len(n["fmts"]) and n["headEnd"], f"{len(n['hashes'])} hashes = headers = formats")
        check(all(u32(hd, 0x0C) == h for hd, h in zip(n["heads"], n["hashes"])), "header i is hash i")
        check(all(hd[0x90] == ((0x91 + len(hd[0x91:].split(b'\0')[0]) + 1 + 3) & ~3) - 0x91 for hd in n["heads"]), "length bytes")
        check(all(f[:12] == bytes(12) and f[16:] == bytes(16) for f in n["fmts"]), "formats hold only a FourCC")
        check(n["start"] % 128 == 0, "pixels start on a 128-byte boundary")
        spans = sorted((u32(hd, 0x50), u32(hd, 0x4C)) for hd in n["heads"])
        check(all(off % 128 == 0 for off, size in spans), "texture offsets 128-aligned")
        check(all(a[0] + a[1] <= b2[0] for a, b2 in zip(spans, spans[1:])) and spans[-1][0] + spans[-1][1] <= n["pixEnd"] - n["start"],
              "textures inside the data, none overlapping")
        old = {u32(hd, 0x0C): hd for hd in o["heads"]}
        same = True
        for hd in n["heads"]:
            h = u32(hd, 0x0C)
            if h not in old:
                continue
            od = old[h]
            if hd != od or nb[n["start"] + u32(hd, 0x50):n["start"] + u32(hd, 0x50) + u32(hd, 0x4C)] != \
                    ob[o["start"] + u32(od, 0x50):o["start"] + u32(od, 0x50) + u32(od, 0x4C)]:
                same = False
        check(same and set(old) <= set(n["hashes"]), f"the {len(old)} shipped textures as shipped (header and pixels)")
        check(nb[n["pack"][3]:] == ob[o["pack"][3]:], "everything after the pack as shipped")
        for hd in n["heads"]:
            h = u32(hd, 0x0C)
            if h in old:
                continue
            added_hashes.add(h)
            nm = hd[0x91:].split(b"\0")[0].decode("latin1")
            w, hh, mips = u32(hd, 0x20), u32(hd, 0x24), hd[0x31]
            four = n["fmts"][n["hashes"].index(h)][12:16]
            block = 8 if four in (b"DXT1", b"ATI1") else 16
            size = (chain(w, hh, mips, block) + 15) & ~15
            copy = not nm.startswith("FRS_")
            if not copy:
                made.append(h)
            print(f"    {'copied' if copy else 'new'} {nm} {h:08X} {w}x{hh} {four.decode()} {mips} levels")
            px = nb[n["start"] + u32(hd, 0x50):n["start"] + u32(hd, 0x50) + size]
            sizes_ok = u32(hd, 0x18) == u32(hd, 0x4C) == size and u32(hd, 0x1C) == chain(w, hh, 1, block) and \
                u32(hd, 0x28) == w.bit_length() - 1 and u32(hd, 0x2C) == hh.bit_length() - 1
            if copy:
                # A game texture copied: its shipped header but for size, levels and offset; the copy ends with
                # the shipped pack's own levels (the chain from the pack's size down).
                src = source_of(h)
                check(src is not None, f"{nm}: shipped in a texture section")
                if src is None:
                    continue
                sh, spx = src
                keep = [i for i in range(0x90) if not (0x18 <= i < 0x30 or i == 0x31 or 0x4C <= i < 0x54)]
                check(all(hd[i] == sh[i] for i in keep) and hd[0x90:] == sh[0x90:], f"{nm}: header as shipped but sizes and offset")
                check(sizes_ok and max(w, hh) <= 512, f"{nm}: size fields, at most 512")
                base = chain(u32(sh, 0x20), u32(sh, 0x24), sh[0x31], block)
                whole = chain(w, hh, mips, block)
                check(u32(sh, 0x20) <= w and px[whole - base:whole] == spx[:base], f"{nm}: ends with the shipped pack's levels")
            else:
                check(bhash(nm) == h, f"{nm}: hash = bStringHash(name)")
                check(w & (w - 1) == 0 and hh & (hh - 1) == 0 and max(w, hh) <= 512 and min(w, hh) >> (mips - 1) == 4 or min(w, hh) == 4,
                      f"{nm}: power-of-two sides, levels down to 4 px")
                check(sizes_ok and hd[0x30] == (0x22 if four == b"DXT1" else 0x26), f"{nm}: size fields")
            check(len(px) == size and any(px), f"{nm}: pixels there")
        fh = int(name[len("STREAML5RA_0x"):-4], 16)
        if table_b and fh in entries:
            q = entries[fh]
            parts = streamed_parts(nb)
            cnt = u32(table_b, q + 0x5C)
            words = [struct.unpack_from("<3I", table_b, q + 0x60 + 12 * i) for i in range(cnt)]
            check(cnt == len(parts) and all(w[0] == w[1] == e for w, e in zip(words, parts)), f"table entry: {cnt} part ends {parts}")
            sq = ship_entries[fh]
            third_old = u32(ship_table, sq + 0x68)
            check(all(w[2] >= third_old for w in words) and len({w[2] for w in words}) == 1,
                  f"table third word {third_old} -> {words[0][2]}")
        else:
            check(False, "L5RA.BUN written beside it with an entry for the file")
    if len(sys.argv) > 4 and sys.argv[3] == "--edits":
        lines = [l for l in open(sys.argv[4]).read().splitlines() if l.startswith("X ")]
        print("  edit list:", *lines, sep="\n    ")
        check(len(lines) > 0 or not made, "edit list names the textures the editor made (copies are not listed)")
        for line in lines:
            h = int(line.split()[1], 16)
            check(h in added_hashes, f"{line.split()[3]} {h:08X} of the list is in a written texture pack")
    print("ALL CHECKS PASS" if not fails else f"{len(fails)} FAILED", f"({seen} texture sections written)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
