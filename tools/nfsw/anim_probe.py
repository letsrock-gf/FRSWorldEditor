"""Dump NFS World world-animation chunks (0x37220/0x37260/0x37250/0x37240) of one or more section files.
Usage: anim_probe.py <file> [...]"""
import struct
import sys


def body(buf, pos):
    """Payload start the way nfsw.exe 0x750B80 takes it: (chunk + 0x17) & ~15."""
    return (pos + 0x17) & ~15


for path in sys.argv[1:]:
    buf = open(path, "rb").read()
    print("==", path.split("\\")[-1].split("/")[-1], len(buf))
    pos = 0
    while pos + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, pos)
        if cid in (0x37220, 0x37240, 0x37250, 0x37260, 0x37270):
            b = body(buf, pos)
            end = pos + 8 + size
            data = buf[b:end]
            print(f"  {cid:05X} @{pos:#x} size {size} payload {len(data)}")
            n = min(len(data), 0x80 if cid != 0x37240 else 0x40)
            for i in range(0, n, 16):
                w = data[i:i + 16]
                f = struct.unpack_from("<%df" % (len(w) // 4), w)
                print("    %04x %s | %s" % (i, w.hex(" "), " ".join("%.3g" % x for x in f)))
        pos += 8 + size
