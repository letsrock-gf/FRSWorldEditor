"""Checks a written section table against the section files written beside it.
For every section file in <out>: its entry follows the rule measured on the shipped table (table_levels.py) --
one part: the file's size twice, the third word raised by what the file grew; five parts: where each part ends
in the written file, twice, the third word as shipped. And no other entry differs from the shipped table.
Usage: verify_table.py <out folder with L5RA.BUN and sections> <shipped L5RA.BUN> <folder with the shipped sections>..."""
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


def resident(f):
    """Bytes of the first part that are not a texture pack's pixel chunk."""
    def find(begin, end, wanted):
        at = begin
        while at + 8 <= end:
            nxt = at + 8 + u32(f, at + 4)
            if u32(f, at) == wanted:
                return at, nxt
            at = nxt
        return None
    last = pixels = at = 0
    while at + 8 <= len(f):
        cid, end = u32(f, at), at + 8 + u32(f, at + 4)
        if cid == 0xB3300011:
            break
        if cid != 0:
            last = end
        if cid == 0xB3300000:
            block = find(at + 8, end, 0xB3320000)
            chunk = find(block[0] + 8, block[1], 0x33320002) if block else None
            if chunk:
                pixels += chunk[1] - chunk[0]
        at = end
    return last - pixels


def parts_of(f):
    ends, last, at = [], 0, 0
    while at + 8 <= len(f):
        cid, end = u32(f, at), at + 8 + u32(f, at + 4)
        if cid == 0xB3300011:
            if not ends:
                ends.append(last)
            ends.append(end)
        elif cid != 0 and not ends:
            last = end
        at = end
    return ends


out, shipped_table, shipped_folders = sys.argv[1], sys.argv[2], sys.argv[3:]
new = open(os.path.join(out, "L5RA.BUN"), "rb").read()
old = open(shipped_table, "rb").read()
fails = 0


def check(ok, text):
    global fails
    if not ok:
        fails += 1
    print(("ok   " if ok else "FAIL ") + text)


# The file grows by the ids of the physical props placed (chunk 0x34108, io/WorldPlace.h) and by widened
# streaming zones (0x34152, before the table: the table moves); it never shrinks.
check(len(new) >= len(old), "table file is not smaller than the shipped one (+%d bytes)" % (len(new) - len(old)))
start, count = table(old)
moved = table(new)[0] - start   # where the written table is, in the written file
written = {n.upper() for n in os.listdir(out) if n.upper().startswith("STREAML5RA_")}
seen = 0
for i in range(count):
    e = start + i * 208
    n = e + moved   # the same entry in the written file
    name = old[e:e + 8].split(b"\0")[0].decode("latin1")
    files = [f"STREAML5RA_{u32(old, e + 8)}.BUN", f"STREAML5RA_0x{u32(old, e + 0x1C):08X}.BUN"]
    file = next((f for f in files if f.upper() in written), None)
    if not file:
        if new[n:n + 208] != old[e:e + 208]:
            check(False, f"{name}: entry changed but no file was written for it")
        continue
    seen += 1
    f = open(os.path.join(out, file), "rb").read()
    parts = u32(old, e + 0x5C)
    got = [struct.unpack_from("<3I", new, n + 0x60 + 12 * k) for k in range(parts)]
    was = [struct.unpack_from("<3I", old, e + 0x60 + 12 * k) for k in range(parts)]
    rest_same = new[n:n + 0x60] == old[e:e + 0x60] and new[n + 0x60 + 12 * parts:n + 208] == old[e + 0x60 + 12 * parts:e + 208]
    shipped = next((os.path.join(d, file) for d in shipped_folders if os.path.exists(os.path.join(d, file))), None)
    if parts == 1:
        delta = len(f) - os.path.getsize(shipped) if shipped else None
        want = [(len(f), len(f), was[0][2] + delta)] if delta is not None else None
    else:
        ends = parts_of(f)
        grew = max(0, resident(f) - resident(open(shipped, "rb").read())) if shipped else 0
        grew = (grew + 127) & ~127
        want = [(end, end, was[k][2] + grew) for k, end in enumerate(ends)] if len(ends) == parts and shipped else None
    check(want is not None and [tuple(t) for t in got] == want and rest_same,
          f"{name} {file}: {parts} part(s) " + (", ".join(hex(t[0]) for t in got)) +
          ("" if want is None or [tuple(t) for t in got] == want else "  want " + ", ".join(hex(t[0]) for t in want)))
check(seen == len(written), f"{seen} of {len(written)} written section files have an entry")
print("ALL CHECKS PASS" if not fails else f"{fails} CHECK(S) FAILED")
