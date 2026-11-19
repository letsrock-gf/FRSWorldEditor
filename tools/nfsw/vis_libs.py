"""Do the game's visibility lists (L5RA.BUN 0x80034150 / 0x34153) name the shared library sections (U..Y)?
Usage: vis_libs.py <L5RA.BUN>"""
import struct, sys
from collections import Counter
b = open(sys.argv[1], "rb").read()
MARK = None


def chunks(s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        yield k, p + 8, p + 8 + n
        p += 8 + n


table = {}
lists = {}
for k, s, e in chunks(0, len(b)):
    if k == 0x34110:
        for i in range((e - s) // 208):
            at = s + 208 * i
            tag = b[at:at + 8].split(b"\0")[0].decode("latin1")
            table[struct.unpack_from("<I", b, at + 8)[0]] = tag
    if k == 0x80034150:
        for k2, s2, e2 in chunks(s, e):
            if k2 == 0x34153:
                d = b[s2:e2]
                # records start with the same 8-byte marker as the first one
                mark = bytes([11, 0, 0, 0, 11, 0, 0, 0])
                starts = [i for i in range(0, len(d) - 8, 4) if d[i:i + 8] == mark]
                for a, z in zip(starts, starts[1:] + [len(d)]):
                    body = d[a + 8:z]
                    zid = struct.unpack_from("<H", body, 0)[0]
                    cnt = struct.unpack_from("<I", body, 8)[0]
                    if cnt == 0 or 12 + cnt * 2 > len(body): continue
                    lists[zid] = struct.unpack_from(f"<{cnt}H", body, 12)
letters = Counter()
for zid, secs in lists.items():
    for n in secs:
        letters[table.get(n, "?")[:1]] += 1
print("zones", len(lists), "entries by first letter of the section tag:", dict(letters))
print("table numbers by letter:", dict(Counter(t[:1] for t in table.values())))
