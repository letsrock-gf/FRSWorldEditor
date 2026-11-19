"""Look for a sky dome model in NFS World's GLOBAL files.
GlobalB.lzc: header u32 0x66113388, u32 unpacked size, u32 packed size, u32 2; then blocks
{u32 0x55441122, u32 size, u32 packed (with this 24-byte header), u32 offset in the output, u32 offset of this block
- 16, u32 0} each followed by a JDLZ stream. Unpacks to <out> and lists solid objects (0x134011, name @0xA0 as in
TRACKS) whose name has SKY or DOME, plus texture names with SKY.
Usage: sky_probe.py <Data2015> <out file>"""
import os
import re
import struct
import sys


def jdlz(data, pos):
    size = struct.unpack_from("<I", data, pos + 8)[0]
    p, flags, matches = pos + 16, 1, 1
    out = bytearray()
    while len(out) < size:
        if flags == 1:
            flags = data[p] | 256; p += 1
        if matches == 1:
            matches = data[p] | 256; p += 1
        if flags & 1:
            first, second = data[p], data[p + 1]; p += 2
            if matches & 1:
                length = (second | ((first & 240) << 4)) + 3; distance = (first & 15) + 1
            else:
                length = (first & 31) + 3; distance = (second | ((first & 224) << 3)) + 17
            start = len(out) - distance
            for k in range(length):
                out.append(out[start + k])
            matches >>= 1
        else:
            out.append(data[p]); p += 1
        flags >>= 1
    return out


game, target = sys.argv[1], sys.argv[2]
src = open(os.path.join(game, "GLOBAL", "GlobalB.lzc"), "rb").read()
magic, total, packed, _ = struct.unpack_from("<4I", src, 0)
assert magic == 0x66113388, hex(magic)
if not os.path.exists(target):
    out = bytearray(total)
    pos = 16
    while pos + 24 <= len(src):
        m, size, blk, offset, back, _ = struct.unpack_from("<6I", src, pos)
        if m != 0x55441122:
            break
        assert back == pos - 16
        out[offset:offset + size] = jdlz(src, pos + 24) if src[pos + 24:pos + 28] == b"JDLZ" else src[pos + 24:pos + 24 + size]
        pos += blk
    open(target, "wb").write(out)
buf = open(target, "rb").read()
print("GlobalB unpacked", len(buf), "bytes")


def walk(lo, hi, depth, path):
    pos = lo
    while pos + 8 <= hi:
        cid, size = struct.unpack_from("<II", buf, pos)
        if pos + 8 + size > hi:
            return
        if cid == 0x134011:
            name = buf[pos + 8 + 0xA0:pos + 8 + 0xA0 + 64].split(b"\0")[0]
            # header may carry 0x11 filler: also try the first printable run
            names = re.findall(rb"[A-Za-z0-9_]{4,}", buf[pos + 8:pos + 8 + size])
            hit = [n.decode() for n in names if re.search(rb"SKY|DOME", n, re.I)]
            solids.append(name)
            if hit:
                print(f"  solid @{pos:#x} in {path}: {hit}")
        if cid & 0x80000000:
            walk(pos + 8, pos + 8 + size, depth + 1, path + [hex(cid)])
        pos += 8 + size


solids = []
walk(0, len(buf), 0, [])
print("solids in GlobalB:", len(solids))
for m in sorted(set(re.findall(rb"[A-Za-z0-9_]*(?:SKY|DOME)[A-Za-z0-9_]*", buf, re.I))):
    print("  text:", m.decode())
