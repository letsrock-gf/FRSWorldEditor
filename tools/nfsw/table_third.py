"""What is the third word of a texture section's triples in the section table? Tries, on every 5-part entry,
offsets found in the shipped file: of each top-level texture pack, of its data block (0xB3320000), of its pixel
chunk (0x33320002) and of the pixels themselves, and says which candidate equals the word and how often.
Usage: table_third.py <shipped L5RA.BUN> <TRACKS folder>"""
import collections
import os
import struct
import sys


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def table(b):
    at = 0
    while at + 8 <= len(b):
        cid, size = u32(b, at), u32(b, at + 4)
        if cid == 0x34110:
            return at + 8, size // 208
        at += 8 + size
    raise SystemExit("no section table")


def walk(b, begin, end):
    at = begin
    while at + 8 <= end:
        size = u32(b, at + 4)
        yield u32(b, at), at, at + 8, at + 8 + size
        at += 8 + size


b = open(sys.argv[1], "rb").read()
tracks = sys.argv[2]
start, count = table(b)
hits = collections.Counter()
none = []
total = 0
for i in range(count):
    e = start + i * 208
    if u32(b, e + 0x5C) != 5:
        continue
    name = b[e:e + 8].split(b"\0")[0].decode("latin1")
    file = f"STREAML5RA_0x{u32(b, e + 0x1C):08X}.BUN"
    if name == "Z0":
        file = "STREAML5RA_25000.BUN"
    path = os.path.join(tracks, "frs-original", file)
    if not os.path.exists(path):
        path = os.path.join(tracks, file)
    if not os.path.exists(path):
        continue
    f = open(path, "rb").read()
    third = u32(b, e + 0x68)
    total += 1
    cands = {}
    packs = 0
    for cid, header, begin, end in walk(f, 0, len(f)):
        if cid != 0xB3300000:
            continue
        packs += 1
        tag = "first" if packs == 1 else "later"
        cands.setdefault(f"{tag} pack header", header)
        for c, h, s, t in walk(f, begin, end):
            if c == 0xB3320000:
                cands.setdefault(f"{tag} data block header", h)
                for c2, h2, s2, t2 in walk(f, s, t):
                    if c2 == 0x33320002:
                        cands.setdefault(f"{tag} pixel chunk header", h2)
                        cands.setdefault(f"{tag} pixel chunk payload", s2)
                        cands.setdefault(f"{tag} pixels (128-aligned)", (s2 + 127) & ~127)
                    if c2 == 0:
                        cands.setdefault(f"{tag} filler before pixel chunk: its header", h2)
                        cands.setdefault(f"{tag} filler before pixel chunk: rounded down to 128", h2 & ~127)
    found = [k for k, v in cands.items() if v == third]
    if found:
        for k in found:
            hits[k] += 1
    else:
        none.append((name, file, hex(third), {k: hex(v) for k, v in cands.items()}, packs))
print("entries:", total)
for k, n in hits.most_common():
    print(f"  {n:5d}  {k}")
print("no candidate:", len(none))
for row in none[:6]:
    print("  ", row)
