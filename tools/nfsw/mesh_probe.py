"""Dumps the chunk layout of solid objects in an NFSW section file: offsets, fillers, header and material fields.
Usage: mesh_probe.py <file.BUN> [object name or hash, default: first 2 objects]"""
import struct
import sys


def walk(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            return
        yield k, p, p + 8, p + 8 + n
        p += 8 + n


def objects(b):
    for k, h, s, e in walk(b, 0, len(b)):
        if k == 0x80134000:
            for k2, h2, s2, e2 in walk(b, s, e):
                if k2 == 0x80134010:
                    yield h2, s2, e2


def dump(b, h, s, e):
    print(f"object chunk @{h:#x} size {e - h:#x} mod16={h % 16} mod128={h % 128}")
    for k, ch, cs, ce in walk(b, s, e):
        fill = 0
        while cs + fill < ce and b[cs + fill] == 0x11 and fill < 16:
            fill += 1
        print(f"  {k:08X} @{ch:#x} size {ce - cs:#x} filler {fill} data@mod16={(cs + fill) % 16}")
        d = cs + fill
        if k == 0x00134011:
            name = b[d + 0xA0:d + 0xC0].split(b"\0")[0]
            print("    name", name, "hash %08X" % struct.unpack_from("<I", b, d + 0x10))
            for i in range(0, min(0xA0, ce - d), 16):
                print(f"    {i:03X} " + " ".join(f"{v:08X}" for v in struct.unpack_from("<4I", b, d + i)),
                      " ".join(f"{v:10.3f}" for v in struct.unpack_from("<4f", b, d + i)))
        if k == 0x80134100:
            for k3, h3, s3, e3 in walk(b, cs, ce):
                fill = 0
                while s3 + fill < e3 and b[s3 + fill] == 0x11 and fill < 16:
                    fill += 1
                print(f"    {k3:08X} @{h3:#x} size {e3 - s3:#x} filler {fill} data@mod16={(s3 + fill) % 16} "
                      f"end pad: {b[e3 - 8:e3].hex()}")
                d3 = s3 + fill
                if k3 == 0x00134900:
                    for i in range(0, e3 - d3, 16):
                        print(f"      {i:03X} " + " ".join(f"{v:08X}" for v in struct.unpack_from("<4I", b, d3 + i)))
                if k3 == 0x00134B02:
                    for m in range((e3 - d3) // 116):
                        a = d3 + m * 116
                        w = struct.unpack_from("<29I", b, a)
                        print(f"      mat {m}: " + " ".join(f"{v:08X}" for v in w))
                        print("         bounds", struct.unpack_from("<3f", b, a + 0x10), struct.unpack_from("<3f", b, a + 0x1C))


if __name__ != "__main__":
    pass
else:
  b = open(sys.argv[1], "rb").read()
  want = sys.argv[2] if len(sys.argv) > 2 else None
  n = 0
  for h, s, e in objects(b):
      hdr = next((cs for k, ch, cs, ce in walk(b, s, e) if k == 0x00134011), None)
      fill = 0
      while b[hdr + fill] == 0x11 and fill < 16:
          fill += 1
      name = b[hdr + fill + 0xA0:hdr + fill + 0xC0].split(b"\0")[0].decode(errors="replace")
      hsh = "%08X" % struct.unpack_from("<I", b, hdr + fill + 0x10)
      if want and want not in (name, hsh):
          continue
      dump(b, h, s, e)
      n += 1
      if not want and n >= 2:
          break
