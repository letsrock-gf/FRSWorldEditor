"""Prints entries of the section table (chunk 0x34110 of L5RA.BUN, 208 bytes each) by section name, as u32 words.
With --stats, says for every entry how the words at +0x2C..+0x68 relate to the section file's chunks.
Usage: table_entry.py <L5RA.BUN> [name...]   |   table_entry.py <L5RA.BUN> --stats <TRACKS folder>"""
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


def entries(b):
    start, count = table(b)
    for i in range(count):
        e = start + i * 208
        yield b[e:e + 8].split(b"\0")[0].decode("latin1"), e


def show(b, names):
    for name, e in entries(b):
        if name not in names:
            continue
        print(f"{name} at {e:#x}")
        for o in range(0, 208, 16):
            words = struct.unpack_from("<4I", b, e + o)
            print(f"  +{o:02X}  " + " ".join(f"{w:08X}" for w in words))


def top_chunks(path):
    b = open(path, "rb").read()
    out, at = [], 0
    while at + 8 <= len(b):
        cid, size = u32(b, at), u32(b, at + 4)
        out.append((cid, at, at + 8 + size))
        at += 8 + size
    return out, len(b)


def stats(b, tracks):
    kinds = {}
    shown = 0
    for name, e in entries(b):
        w = struct.unpack_from("<16I", b, e + 0x2C)       # +0x2C .. +0x68
        count = w[0]
        triples = [w[1 + 3 * k:4 + 3 * k] for k in range(5)]
        files = [f for f in (f"STREAML5RA_{u32(b, e + 0x10)}.BUN", f"STREAML5RA_0x{u32(b, e + 0xBC):08X}.BUN")
                 if os.path.exists(os.path.join(tracks, f))]
        key = (count, tuple(t[2] != 0 for t in triples), tuple(t[0] == t[1] for t in triples))
        kinds.setdefault(key, []).append(name)
        if shown < 6 and name[:1] in "UVWXY" and files:
            chunks, length = top_chunks(os.path.join(tracks, files[0]))
            ends = [end for cid, at, end in chunks if cid in (0xB3300000, 0xB3300011)]
            starts = [at for cid, at, end in chunks if cid in (0xB3300000, 0xB3300011)]
            print(name, files[0], "file", hex(length), "count", count)
            print("   triples", [[hex(x) for x in t] for t in triples])
            print("   pack/level chunks start", [hex(x) for x in starts], "end", [hex(x) for x in ends])
            shown += 1
    for key, names in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        print(len(names), "entries: count", key[0], "third nonzero", key[1], "first==second", key[2], "e.g.", names[:4])


def main():
    b = open(sys.argv[1], "rb").read()
    if len(sys.argv) > 3 and sys.argv[2] == "--stats":
        stats(b, sys.argv[3])
    else:
        show(b, set(sys.argv[2:]))


if __name__ == "__main__":
    main()
