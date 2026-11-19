"""Dumps the larger-level packs (0xB3300011) of a section file whole: where each chunk lies, the pack's own header
bytes, every row, and how the pixel chunk is padded. For comparing a shipped file with a written one.
Usage: tier_dump.py <section file>..."""
import struct
import sys


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def children(b, begin, end):
    at = begin
    while at + 8 <= end:
        size = u32(b, at + 4)
        if at + 8 + size > end:
            print(f"  !! chunk {u32(b, at):08X} at {at:#x} runs past its parent")
            break
        yield u32(b, at), at, at + 8, at + 8 + size
        at += 8 + size


def dump(path):
    b = open(path, "rb").read()
    print(f"=== {path}: {len(b)} bytes ({len(b):#x})")
    for cid, header, begin, end in children(b, 0, len(b)):
        print(f"top {cid:08X} at {header:#x} size {end - begin:#x} (ends {end:#x}, end % 128 = {end % 128})")
        if cid == 0xB3300000:
            for c, h, s, e in children(b, begin, end):
                print(f"    {c:08X} at {h:#x} size {e - s:#x}")
                if c in (0xB3310000, 0xB3320000):
                    for c2, h2, s2, e2 in children(b, s, e):
                        note = ""
                        if c2 in (0x33310001, 0x33320001, 0x33330001):
                            note = "  " + b[s2:min(e2, s2 + 0x90)].hex(" ", 4)
                        print(f"        {c2:08X} at {h2:#x} size {e2 - s2:#x}{note}")
        if cid != 0xB3300011:
            continue
        for c, h, s, e in children(b, begin, end):
            print(f"    {c:08X} at {h:#x} size {e - s:#x}")
            if c == 0x33310021:
                print("        " + b[s:e].hex(" ", 4))
            elif c == 0x33310022:
                for row in range(s, e - 31, 32):
                    print("        row " + b[row:row + 32].hex(" ", 4))
            elif c == 0:
                print(f"        filler: {set(b[s:e])}")
            elif c == 0x33320002:
                start = (s + 127) & ~127
                print(f"        filler {start - s} bytes {set(b[s:start])}, data from {start:#x}, {e - start:#x} bytes")


for f in sys.argv[1:]:
    dump(f)
