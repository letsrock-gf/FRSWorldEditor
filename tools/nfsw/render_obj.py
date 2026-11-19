"""Renders each object of an exported OBJ the way Blender reads it (vt with v up, image origin bottom-left),
looking at the side its normals face. A wrong or flipped UV shows as scrambled or mirrored texture.
Usage: render_obj.py <file.obj> <out folder>"""
import os
import sys

import numpy as np
from PIL import Image

obj_path, out = sys.argv[1], sys.argv[2]
base = os.path.dirname(obj_path)

# --- materials -------------------------------------------------------------------
textures = {}
mtl = os.path.splitext(obj_path)[0] + ".mtl"
current = None
for line in open(mtl, encoding="utf-8", errors="replace"):
    parts = line.split()
    if not parts:
        continue
    if parts[0] == "newmtl":
        current = parts[1]
        textures[current] = None
    elif parts[0] == "map_Kd" and current:
        path = os.path.join(base, " ".join(parts[1:]))
        textures[current] = (os.path.basename(path), np.asarray(Image.open(path).convert("RGB")))

# --- geometry ----------------------------------------------------------------------
v, vt, objects = [], [], {}
name, material = "object", None
for line in open(obj_path, encoding="utf-8", errors="replace"):
    parts = line.split()
    if not parts:
        continue
    if parts[0] == "v":
        v.append([float(x) for x in parts[1:4]])
    elif parts[0] == "vt":
        vt.append([float(x) for x in parts[1:3]])
    elif parts[0] in ("o", "g"):
        name = parts[1]
    elif parts[0] == "usemtl":
        material = parts[1]
    elif parts[0] == "f":
        corners = [p.split("/") for p in parts[1:4]]
        objects.setdefault(name, []).append((material, [int(c[0]) - 1 for c in corners], [int(c[1]) - 1 for c in corners]))
v, vt = np.array(v), np.array(vt)
print(f"{len(v)} vertices, {len(vt)} uvs, {len(objects)} objects; uv range u {vt[:,0].min():.2f}..{vt[:,0].max():.2f} v {vt[:,1].min():.2f}..{vt[:,1].max():.2f}")

SIZE = 640
for name, faces in objects.items():
    tri = np.array([f[1] for f in faces])
    p = v[tri]                                    # (n, 3, 3)
    normal = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]).sum(axis=0)
    length = np.linalg.norm(normal)
    if length < 1e-9:
        continue
    n = normal / length
    # Look at the face side: the eye is along +n, looking down -n.
    helper = np.array([0.0, 1.0, 0.0]) if abs(n[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
    right = np.cross(helper, n); right /= np.linalg.norm(right)
    up = np.cross(n, right)
    pts = p.reshape(-1, 3)
    x, y, depth = pts @ right, pts @ up, pts @ n
    span = max(x.max() - x.min(), y.max() - y.min(), 1e-6)
    sx = ((x - x.min()) / span * (SIZE - 1)).reshape(-1, 3)
    sy = ((y.max() - y) / span * (SIZE - 1)).reshape(-1, 3)
    sz = depth.reshape(-1, 3)
    image = np.zeros((SIZE, SIZE, 3), np.uint8); image[:] = (40, 40, 60)
    zbuf = np.full((SIZE, SIZE), -1e30)
    used = {}
    for i, (material, _, uvi) in enumerate(faces):
        tex = textures.get(material)
        used[material] = tex[0] if tex else None
        x0, x1 = int(np.floor(sx[i].min())), int(np.ceil(sx[i].max()))
        y0, y1 = int(np.floor(sy[i].min())), int(np.ceil(sy[i].max()))
        if x1 < 0 or y1 < 0 or x0 >= SIZE or y0 >= SIZE:
            continue
        gx, gy = np.meshgrid(np.arange(max(x0, 0), min(x1, SIZE - 1) + 1), np.arange(max(y0, 0), min(y1, SIZE - 1) + 1))
        (ax, ay), (bx, by), (cx, cy) = zip(sx[i], sy[i])
        det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(det) < 1e-9:
            continue
        w0 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / det
        w1 = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / det
        w2 = 1 - w0 - w1
        inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
        z = w0 * sz[i][0] + w1 * sz[i][1] + w2 * sz[i][2]
        keep = inside & (z > zbuf[gy, gx])
        if not keep.any():
            continue
        if tex is None:
            colour = np.array([200, 0, 200], np.uint8)
            image[gy[keep], gx[keep]] = colour
        else:
            uv = vt[uvi]
            u = w0 * uv[0][0] + w1 * uv[1][0] + w2 * uv[2][0]
            w = w0 * uv[0][1] + w1 * uv[1][1] + w2 * uv[2][1]
            h, wd = tex[1].shape[:2]
            tx = (np.floor((u % 1.0) * wd).astype(int)) % wd
            ty = (np.floor((1.0 - (w % 1.0)) * h).astype(int)) % h     # Blender: v = 0 is the bottom row
            image[gy[keep], gx[keep]] = tex[1][ty[keep], tx[keep]]
        zbuf[gy[keep], gx[keep]] = z[keep]
    safe = "".join(c if c.isalnum() or c in "_-" else "_" for c in name)[:60]
    Image.fromarray(image).save(os.path.join(out, f"render-{safe}.png"))
    print(f"{name}: {len(faces)} triangles, facing {np.round(n, 2)}, textures {sorted(set(str(t) for t in used.values()))}")
