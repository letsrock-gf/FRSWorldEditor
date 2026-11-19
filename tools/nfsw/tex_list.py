import struct, sys
b = open(sys.argv[1], "rb").read()
def walk(s=0, e=None):
    e = len(b) if e is None else e; p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e: return
        yield k, p + 8, p + 8 + n
        if k & 0x80000000: yield from walk(p + 8, p + 8 + n)
        p += 8 + n
for k, s, e in walk():
    if k == 0x33310004:
        p = s
        while p + 0x91 <= e:
            ln = b[p + 0x90]; name = b[p + 0x91:p + 0x91 + ln].split(b"\0")[0].decode()
            h, = struct.unpack_from("<I", b, p + 0x0C)
            size, base, w, h2 = struct.unpack_from("<IIII", b, p + 0x18)
            sz2, off = struct.unpack_from("<II", b, p + 0x4C)
            print(f"{h:08X} {w}x{h2} mips={b[p+0x31]} type={b[p+0x30]:02X} size={size:#x} base={base:#x} off={off:#x} sz2={sz2:#x} {name}")
            p += 0x91 + ln
    if k == 0x33310022:
        for q in range(s, e, 32):
            print("  stream mip:", b[q:q+32].hex(" ", 4))
