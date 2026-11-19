"""Prints format, alpha flags and the mean alpha of level 0 of the pack copy of every texture whose name holds
the given text.   python alpha_stored.py <section file> <name part>"""
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
def alpha_of(block):
    a0, a1 = block[0], block[1]; bits = int.from_bytes(block[2:8], "little")
    t = [a0, a1] + ([((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1
                    else [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
    return [t[(bits >> (3 * i)) & 7] for i in range(16)]
ent = []; fm = []; data = None
for k, s, e in walk():
    if k == 0x33310004:
        p = s
        while p + 0x91 <= e:
            ln = b[p + 0x90]
            ent.append((b[p + 0x91:p + 0x91 + ln].split(b"\0")[0].decode(), struct.unpack_from("<II", b, p + 0x20),
                        b[p + 0x38], b[p + 0x39], struct.unpack_from("<I", b, p + 0x50)[0]))
            p += 0x91 + ln
    if k == 0x33310005: fm = [b[q + 12:q + 16] for q in range(s, e, 32)]
    if k == 0x33320002 and data is None: data = s
start = (data + 127) & ~127
for (name, (w, h), usage, blend, off), cc in zip(ent, fm):
    if sys.argv[2] in name:
        text = ""
        if cc == b"DXT5":
            n = (w // 4) * (h // 4)
            total = sum(sum(alpha_of(b[start + off + 16 * i:start + off + 16 * i + 8])) for i in range(n))
            text = f"level 0 alpha mean {total / n / 16:.1f}"
        print(name, f"{w}x{h}", cc.decode(), "usage", usage, "blend", blend, text)
