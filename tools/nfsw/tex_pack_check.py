"""Lists the texture packs and larger-level packs of section files and says what in them
io/WorldTexture.cpp would refuse. Usage: tex_pack_check.py <section file>... [--hash HEX]..."""
import struct
import sys


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def children(b, begin, end):
    at = begin
    while at + 8 <= end:
        size = u32(b, at + 4)
        if at + 8 + size > end:
            print(f"    !! chunk {u32(b, at):08X} at {at:#x} runs past its parent ({size:#x})")
            break
        yield u32(b, at), at, at + 8, at + 8 + size
        at += 8 + size


def align128(v):
    return (v + 127) & ~127


def check(path, wanted):
    b = open(path, "rb").read()
    print(f"{path}: {len(b)} bytes")
    for cid, header, begin, end in children(b, 0, len(b)):
        if cid == 0xB3300000:
            parts = {c: (h, s, e) for c, h, s, e in children(b, begin, end)}
            info, data = parts.get(0xB3310000), parts.get(0xB3320000)
            print(f"  pack at {header:#x}: parts {[f'{c:08X}' for c in parts]}")
            if not info or not data:
                continue
            inner = {c: (h, s, e) for c, h, s, e in children(b, info[1], info[2])}
            dparts = {c: (h, s, e) for c, h, s, e in children(b, data[1], data[2])}
            print(f"    info {[f'{c:08X}' for c in inner]}  data {[f'{c:08X}' for c in dparts]}")
            headers, pixels = inner.get(0x33310004), dparts.get(0x33320002)
            if not headers or not pixels:
                continue
            start = align128(pixels[1])
            length = pixels[2] - start
            entry = headers[1]
            count = 0
            while entry + 0x91 <= headers[2]:
                h, size, off = u32(b, entry + 0x0C), u32(b, entry + 0x4C), u32(b, entry + 0x50)
                w, hgt, mips = u32(b, entry + 0x20), u32(b, entry + 0x24), b[entry + 0x31]
                bad = off + size > length
                if bad or h in wanted or not wanted and count < 3:
                    name = b[entry + 0x91:entry + 0x91 + b[entry + 0x90]].split(b"\0")[0].decode("latin1")
                    print(f"    {h:08X} {name:32s} {w}x{hgt} mips {mips} size {size:#x} @0x18 {u32(b, entry + 0x18):#x} "
                          f"offset {off:#x} file {start + off:#x}{'  !! past the data (' + hex(length) + ')' if bad else ''}")
                entry += 0x91 + b[entry + 0x90]
                count += 1
            print(f"    {count} textures, data {length:#x} bytes from {start:#x}")
        elif cid == 0xB3300011:
            parts = {c: (h, s, e) for c, h, s, e in children(b, begin, end)}
            print(f"  levels at {header:#x}: parts {[f'{c:08X}' for c in parts]}")
            rows, pixels = parts.get(0x33310022), parts.get(0x33320002)
            if not rows or not pixels:
                continue
            start = align128(pixels[1])
            length = pixels[2] - start
            n = 0
            for row in range(rows[1], rows[2] - 31, 32):
                h, off, size, base = struct.unpack_from("<IIII", b, row)
                w, hgt = struct.unpack_from("<HH", b, row + 16)
                bad = off + base > length
                if bad or h in wanted:
                    print(f"    {h:08X} {w}x{hgt} mips {u32(b, row + 28)} size {size:#x} base {base:#x} offset {off:#x} "
                          f"file {start + off:#x}{'  !! past the data (' + hex(length) + ')' if bad else ''}")
                n += 1
            print(f"    {n} rows, data {length:#x} bytes from {start:#x}")


def main():
    args = sys.argv[1:]
    wanted = set()
    files = []
    while args:
        a = args.pop(0)
        if a == "--hash":
            wanted.add(int(args.pop(0), 16))
        else:
            files.append(a)
    for f in files:
        check(f, wanted)


if __name__ == "__main__":
    main()
