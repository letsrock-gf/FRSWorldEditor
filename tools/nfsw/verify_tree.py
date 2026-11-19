"""Checks the scenery trees (0x34105) of written section files the way nfsw.exe walks them (0x73DB90: a 64-entry
stack of nodes to visit, child < 0 is node -child at 48 bytes each from node 0, child >= 0 a record), against the
shipped file:
  - every record of the container reached from the root exactly once, no node reached twice, no bad index;
  - at most 64 nodes waiting on the walk's stack;
  - every node's box holds its children's (records' bounds @0/@0x10, nodes' @0/@0x10) and counts are 0..10;
  - the shipped nodes are still there, in place (only children moved into appended nodes, boxes grown);
  - the node count is the chunk size / 48 (no other count read: loader 0x746060).
Usage: verify_tree.py <written folder> <shipped TRACKS folder> [section numbers...]"""
import os
import re
import struct
import sys


def kids(b, s, e):
    p = s
    while p + 8 <= e:
        k, n = struct.unpack_from("<II", b, p)
        if p + 8 + n > e:
            return
        yield k, p + 8, p + 8 + n
        p += 8 + n


def containers(b):
    out = {}
    for k, s, e in kids(b, 0, len(b)):
        if k != 0x80034100:
            continue
        ch = {kk: ((ss + 15) & ~15 if kk in (0x34101, 0x34103, 0x34105) else ss, ee) for kk, ss, ee in kids(b, s, e)}
        if 0x34105 not in ch or 0x34103 not in ch or 0x34101 not in ch:
            continue
        num = struct.unpack_from("<I", b, ch[0x34101][0] + 0x0C)[0]
        ns, ne = ch[0x34105]
        rs, re_ = ch[0x34103]
        nodes = [b[q:q + 48] for q in range(ns, ne - 47, 48)]
        recs = [b[q:q + 96] for q in range(rs, re_ - 95, 96)]
        out[num] = (nodes, recs, (ne - ns) % 48)
    return out


def box(raw):
    return struct.unpack_from("<3f", raw, 0), struct.unpack_from("<3f", raw, 0x10)


def holds(outer, inner, eps=0.01):
    (a, b), (c, d) = outer, inner
    return all(a[i] - eps <= c[i] and d[i] <= b[i] + eps for i in range(3))


def main():
    written, shipped = sys.argv[1], sys.argv[2]
    only = {int(x) for x in sys.argv[3:]}
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)
        print(("  ok   " if ok else "  FAIL ") + what)

    for name in sorted(os.listdir(written)):
        m = re.match(r"STREAML5RA_(\d+)\.BUN$", name)
        if not m or (only and int(m.group(1)) not in only):
            continue
        kept = os.path.join(shipped, "frs-original", name)
        ob = open(kept if os.path.exists(kept) else os.path.join(shipped, name), "rb").read()
        nb = open(os.path.join(written, name), "rb").read()
        old, new = containers(ob), containers(nb)
        for num, (nodes, recs, rest) in sorted(new.items()):
            onodes, orecs, _ = old.get(num, ([], [], 0))
            if nodes == onodes and recs == orecs:
                continue
            print(f"{name} scenery {num}: nodes {len(onodes)} -> {len(nodes)}, records {len(orecs)} -> {len(recs)}")
            check(rest == 0, "node table a whole number of 48-byte nodes")
            seen_rec, seen_node, stack, peak, bad = [0] * len(recs), [0] * len(nodes), [0], 1, False
            while stack:
                i = stack.pop()
                if i >= len(nodes) or seen_node[i]:
                    bad = True
                    break
                seen_node[i] = 1
                nd = nodes[i]
                cnt = struct.unpack_from("<h", nd, 0x0C)[0]
                if not 0 <= cnt <= 10:
                    bad = True
                    break
                for c in range(cnt):
                    ch = struct.unpack_from("<h", nd, 0x1C + 2 * c)[0]
                    if ch < 0:
                        stack.append(-ch)
                        if not holds(box(nd), box(nodes[-ch]) if -ch < len(nodes) else box(nd)):
                            check(False, f"node {i} holds node {-ch}")
                    elif ch < len(recs):
                        seen_rec[ch] += 1
                        if not holds(box(nd), box(recs[ch])):
                            check(False, f"node {i} holds record {ch}")
                    else:
                        bad = True
                peak = max(peak, len(stack))
            check(not bad, "no loop, bad index or bad count on the walk")
            check(all(s == 1 for s in seen_rec), f"every record reached once ({sum(seen_rec)} of {len(recs)})")
            check(all(seen_node), f"every node reached ({sum(seen_node)} of {len(nodes)})")
            check(peak <= 64, f"walk stack peak {peak} (the game keeps 64)")
            # Shipped nodes keep their place; a count only grows, and a child only changes into an appended node.
            same = True
            for i in range(len(onodes)):
                oc, nc = struct.unpack_from("<h", onodes[i], 0x0C)[0], struct.unpack_from("<h", nodes[i], 0x0C)[0]
                for c in range(min(oc, 10)):
                    was, now = (struct.unpack_from("<h", x, 0x1C + 2 * c)[0] for x in (onodes[i], nodes[i]))
                    if was != now and not (now < 0 and -now >= len(onodes)):
                        same = False
                same = same and nc >= oc
            check(len(nodes) >= len(onodes) and same, f"shipped nodes in place ({len(nodes) - len(onodes)} appended)")
    print("ALL CHECKS PASS" if not fails else f"{len(fails)} FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
