"""Checks, on every entry of the section table, what the words from +0x5C say against the section's file:
+0x5C a count, then that many triples. For texture sections (count 5) the first two words of triple k should be
where the pack (k = 0) or the k-th larger-level pack ends in the file, and the third where the pack's pixel chunk
begins. The shipped copy in frs-original is read when there is one.
Usage: table_levels.py <shipped L5RA.BUN> <TRACKS folder>"""
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


def main():
    b = open(sys.argv[1], "rb").read()
    tracks = sys.argv[2]
    start, count = table(b)
    counts = collections.Counter()
    thirds = collections.Counter()
    good = bad = missing = 0
    shown = 0
    for i in range(count):
        e = start + i * 208
        name = b[e:e + 8].split(b"\0")[0].decode("latin1")
        levels = u32(b, e + 0x5C)
        counts[levels] += 1
        triples = [struct.unpack_from("<3I", b, e + 0x60 + 12 * k) for k in range(min(levels, 5))]
        if levels == 1:
            thirds[triples[0][2] - triples[0][0]] += 1
            continue
        file = f"STREAML5RA_0x{u32(b, e + 0x1C):08X}.BUN"
        path = os.path.join(tracks, "frs-original", file)
        if not os.path.exists(path):
            path = os.path.join(tracks, file)
        if not os.path.exists(path):
            missing += 1
            continue
        f = open(path, "rb").read()
        # Level 0 ends where the last chunk that is not filler ends before the first larger-level pack.
        ends, last, pixel = [], 0, triples[0][2]
        for cid, header, begin, end in walk(f, 0, len(f)):
            if cid == 0xB3300011:
                if not ends:
                    ends.append(last)
                ends.append(end)
            elif cid != 0 and not ends:
                last = end
        want = [(end, end, pixel) for end in ends]
        same_third = len(set(t[2] for t in triples)) == 1
        if [tuple(t) for t in triples] == want and same_third:
            good += 1
        else:
            bad += 1
            if shown < 8:
                shown += 1
                print(f"differs: {name} {file} file {len(f):#x}")
                print("   table", [[hex(x) for x in t] for t in triples])
                print("   file ", [[hex(x) if x is not None else None for x in t] for t in want])
    print("counts at +0x5C:", dict(counts))
    print("count 1: third minus first:", thirds.most_common(6))
    print(f"count > 1: {good} follow the rule, {bad} differ, {missing} without a file")


if __name__ == "__main__":
    main()
