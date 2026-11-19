"""Texture pack tree + header dump. Usage: tpk_probe.py <file>"""
import struct, sys
b = open(sys.argv[1], "rb").read()
def walk(s, e, d):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n, d
        if k & 0x80000000: yield from walk(p + 8, p + 8 + n, d + 1)
        p += 8 + n
def hd(s, e, n):
    for i in range(s, min(e, s + n), 16):
        r = b[i:i + 16]; print(f"      {i - s:03X}  {r.hex(' ')}  {''.join(chr(c) if 32 <= c < 127 else '.' for c in r)}")
for k, s, e, d in walk(0, len(b), 0):
    if k == 0: continue
    print("  " * d + f"{k:08X} {e - s} @file {s:#x}")
    if k in (0x33310001, 0x33310002, 0x33310004, 0x33310005, 0x33310021, 0x33310022, 0x33320001, 0x33312001, 0x33312002, 0x33330001, 0xB3312004):
        hd(s, e, 320 if k == 0x33310004 else 96)
    if k == 0x33320002: hd(s, e, 160)
