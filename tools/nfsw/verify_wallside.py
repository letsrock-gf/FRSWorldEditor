"""Each wall-like face of the edit list (A lines) must be written as a barrier that blocks on the face's front.

A barrier holds cars to the right of its first end towards its second, seen from above (shipped data: the road
lies there for 5476 of 5510 barriers with ground on one side). The writer's barrier for a face: ends = the two
corners furthest apart in plan. Usage: verify_wallside.py <edit list> <written STREAML5RA_25000.BUN>
"""
import math
import sys

import col_lib


def faces(path):
    for line in open(path):
        f = line.split()
        if not f or f[0] != 'A' or f[2] != '0':
            continue
        quad = f[3] == '1'
        v = [float(x) for x in f[-12:]]
        corners = [tuple(v[i:i + 3]) for i in range(0, 12, 3)][:4 if quad else 3]
        a, b, c = corners[0], corners[1], corners[2]
        u = [b[i] - a[i] for i in range(3)]
        w = [c[i] - a[i] for i in range(3)]
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        length = math.sqrt(sum(x * x for x in n))
        if length < 1e-6 or abs(n[2]) / length > 0.6:
            continue
        zs = [c[2] for c in corners]
        if max(zs) - min(zs) < 0.5:   # ground to drive over (nfs::WallLike)
            continue
        far, ends = -1, None
        for i in range(len(corners)):
            for j in range(i + 1, len(corners)):
                d = math.hypot(corners[i][0] - corners[j][0], corners[i][1] - corners[j][1])
                if d > far:
                    far, ends = d, (corners[i], corners[j])
        if far < 0.05:
            continue
        yield ends, n


def barriers(path):
    buf = open(path, 'rb').read()
    out = []
    for num, data in col_lib.packs(buf):
        arts, insts = col_lib.load(data)
        for ins in insts:
            art = arts.get(ins.article)
            if not art:
                continue
            for b in art.barriers:
                out.append((col_lib.game(ins.to_world(b[0])), col_lib.game(ins.to_world(b[1]))))
    return out


def main():
    written = barriers(sys.argv[2])
    index = {}
    for p, q in written:
        index.setdefault((round(p[0], 1), round(p[1], 1)), []).append((p, q))
    right = wrong = missing = 0
    for (e0, e1), n in faces(sys.argv[1]):
        # A face and its twin (stored both ways round) share their ends: the face
        # passes when one of the records with those ends blocks on its front.
        found = []
        want = {(round(e0[0], 1), round(e0[1], 1)), (round(e1[0], 1), round(e1[1], 1))}
        for key in want:
            for p, q in index.get(key, []):
                if {(round(p[0], 1), round(p[1], 1)), (round(q[0], 1), round(q[1], 1))} == want:
                    found.append((p, q))
        if not found:
            missing += 1
            continue
        if any((q[1] - p[1]) * n[0] - (q[0] - p[0]) * n[1] >= 0 for p, q in found):
            right += 1
        else:
            wrong += 1
    print(f'walls blocking on the front {right}, on the back {wrong}, not found as barriers {missing}')
    print('PASS' if wrong == 0 and right > 0 else 'FAIL')


if __name__ == '__main__':
    main()
