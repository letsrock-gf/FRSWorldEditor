"""Measure NFS World world animations over every section file (read-only).

Layout read from nfsw.exe (REA): loader 0x750B80 registered for 0x37220/0x37240; tree setup 0x7503B0;
per-frame update 0x74FCE0 -> clock 0x74F520 (frame += dt * 15 * rate).
  0x37220 header (payload at (chunk+0x17)&~15): u32 a, u32 b, u32 section number (25002 = shared library), u32 trees
  0x37260 tree: u32 entity count, followed by that many 0x37250 (+ 0x37240 when it has frames), then 0x37270
  0x37250 entity, 116 bytes: float4x4 @0 (rest), hash @0x40, u8 parent @0x44 (0xFF none), u8 rate @0x45
          (rate = (b & 15)/15 + max(b >> 4, 1)), u8 flags @0x46 (2 = from library, 4 = copy of parent), model
          hashes @0x48/@0x4C, @0x54 event hash, @0x58 = hash of the scenery record it drives (record +0x50),
          i16 spins @0x5C/@0x5E/@0x60 and speed @0x6C, u32 frames @0x64, u32 @0x68 (bit31 loop, bit30 ping-pong)
  0x37240 frames: one float4x4 per frame, absolute (copied straight to the node, no parent product)
Usage: anim_stats.py <TRACKS dir>"""
import glob
import os
import struct
import sys
from collections import Counter


def payload(pos):
    return (pos + 0x17) & ~15


tracks = sys.argv[1]
solids = set()
trees = entities = withframes = badframes = 0
parents, rates, flags46, flags68, sections = Counter(), Counter(), Counter(), Counter(), Counter()
models, sample = [], []
files = sorted(glob.glob(os.path.join(tracks, "STREAML5RA_*.BUN")))
for path in files:
    b = open(path, "rb").read()
    name = os.path.basename(path)

    def walk(lo, hi):
        global trees, entities, withframes, badframes
        pos, last = lo, None
        while pos + 8 <= hi:
            cid, size = struct.unpack_from("<II", b, pos)
            if pos + 8 + size > hi:
                return
            if cid == 0x134011:  # solid header: hash at +0x?? found below by the 0x134003 list instead
                pass
            if cid == 0x134003:
                for at in range(pos + 8, pos + 8 + size - 7, 8):
                    solids.add(struct.unpack_from("<I", b, at)[0])
                    solids.add(struct.unpack_from("<I", b, at + 4)[0])
            if cid == 0x37220:
                sections[struct.unpack_from("<I", b, payload(pos) + 8)[0] == 25002] += 1
            if cid == 0x37260:
                trees += 1
            if cid == 0x37250:
                e = payload(pos)
                entities += 1
                m = struct.unpack_from("<16f", b, e)
                parents[b[e + 0x44] != 0xFF] += 1
                rates[b[e + 0x45]] += 1
                flags46[b[e + 0x46]] += 1
                f68 = struct.unpack_from("<I", b, e + 0x68)[0]
                flags68[f68 >> 28] += 1
                frames = struct.unpack_from("<I", b, e + 0x64)[0]
                models.append(struct.unpack_from("<I", b, e + 0x48)[0])
                last = (frames, m, name)
            if cid == 0x37240 and last:
                frames, m, nm = last
                n = (pos + 8 + size - payload(pos)) // 64
                withframes += 1
                if n != frames:
                    badframes += 1
                f0 = struct.unpack_from("<16f", b, payload(pos))
                if len(sample) < 6:
                    sample.append((nm, frames, [round(x, 1) for x in m[12:15]], [round(x, 1) for x in f0[12:15]]))
                last = None
            if cid & 0x80000000:
                walk(pos + 8, pos + 8 + size)
            pos += 8 + size

    walk(0, len(b))

print(f"{len(files)} files; headers {dict(sections)} (True = library 25002)")
print(f"trees {trees}, entities {entities}, with frame chunk {withframes}, frame count != chunk/64: {badframes}")
print("parent set:", dict(parents), " rate byte:", dict(rates), " flags@0x46:", dict(flags46), " @0x68>>28:", dict(flags68))
found = sum(1 for h in models if h in solids)
print(f"model hash @0x48 in some 0x134003 list: {found} of {len(models)}")
for s in sample:
    print("  ", s[0], "frames", s[1], "rest pos", s[2], "frame0 pos", s[3])
