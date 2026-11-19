"""Reads the game's own TRACKS folder (nothing is written): for every section file that has a kept shipped copy
in frs-original, says whether its entry in the live L5RA.BUN matches the live file -- one part: its size; five
parts: where each part ends -- and what changed against the shipped entry.
Usage: verify_live.py <TRACKS folder>"""
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


tracks = sys.argv[1]
kept = os.path.join(tracks, "frs-original")
live = open(os.path.join(tracks, "L5RA.BUN"), "rb").read()
shipped_path = os.path.join(kept, "L5RA.BUN")
shipped = open(shipped_path, "rb").read() if os.path.exists(shipped_path) else live
print("live table", os.path.getmtime(os.path.join(tracks, "L5RA.BUN")), "kept table:", os.path.exists(shipped_path))
keptfiles = {n.upper() for n in os.listdir(kept) if n.upper().startswith("STREAML5RA_")}
start, count = table(live)
bad = 0
for i in range(count):
    e = start + i * 208
    name = live[e:e + 8].split(b"\0")[0].decode("latin1")
    files = [f"STREAML5RA_{u32(live, e + 8)}.BUN", f"STREAML5RA_0x{u32(live, e + 0x1C):08X}.BUN"]
    file = next((f for f in files if f.upper() in keptfiles), None)
    changed = live[e:e + 208] != shipped[e:e + 208]
    if not file:
        if changed:
            bad += 1
            print(f"FAIL {name}: entry differs from shipped but no kept copy of its file")
        continue
    f = open(os.path.join(tracks, file), "rb").read()
    old = os.path.getsize(os.path.join(kept, file))
    parts = u32(live, e + 0x5C)
    got = [struct.unpack_from("<3I", live, e + 0x60 + 12 * k) for k in range(parts)]
    was = [struct.unpack_from("<3I", shipped, e + 0x60 + 12 * k) for k in range(parts)]
    if parts == 1:
        want = [(len(f), len(f), was[0][2] + len(f) - old)]
    else:
        ends = parts_of(f)
        grew = max(0, resident(f) - resident(open(os.path.join(kept, file), "rb").read()))
        grew = (grew + 127) & ~127
        want = [(end, end, was[k][2] + grew) for k, end in enumerate(ends)]
    ok = [tuple(t) for t in got] == want
    bad += not ok
    print(("ok   " if ok else "FAIL ") + f"{name} {file}: {old:#x} -> {len(f):#x}, {parts} part(s) " +
          ", ".join(hex(t[0]) for t in got) + ("" if ok else "   want " + ", ".join(hex(t[0]) for t in want) +
          "   thirds " + ", ".join(hex(t[2]) for t in got)))
print("ALL MATCH" if not bad else f"{bad} DO NOT MATCH")
