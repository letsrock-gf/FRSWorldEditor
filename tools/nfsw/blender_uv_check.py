"""Run inside Blender (background): imports the OBJ the way the editor's quick edit does, then compares the UVs
Blender holds against the OBJ's own vt lines, lists the images each material got, and renders each object textured.
Usage: blender --background --python blender_uv_check.py -- <file.obj> <out folder>"""
import math
import os
import sys

import bpy
from mathutils import Vector

obj_path, out = sys.argv[sys.argv.index("--") + 1:][:2]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.wm.obj_import(filepath=obj_path, forward_axis='NEGATIVE_Z', up_axis='Y')

# --- the OBJ's own numbers, per object, in face-corner order -------------------------
vt, per_object, name = [], {}, None
for line in open(obj_path, encoding="utf-8", errors="replace"):
    p = line.split()
    if not p:
        continue
    if p[0] == "vt":
        vt.append((float(p[1]), float(p[2])))
    elif p[0] in ("o", "g"):
        name = p[1]
    elif p[0] == "f":
        per_object.setdefault(name, []).extend(vt[int(c.split("/")[1]) - 1] for c in p[1:4])

print("=== UVs: Blender against the OBJ")
for o in bpy.data.objects:
    if o.type != 'MESH':
        continue
    mesh = o.data
    layer = mesh.uv_layers.active
    want = per_object.get(o.name) or per_object.get(o.name.split(".")[0])
    if layer is None:
        print(f"{o.name}: NO UV LAYER"); continue
    got = [tuple(layer.data[i].uv) for poly in mesh.polygons for i in poly.loop_indices]
    if want is None or len(want) != len(got):
        print(f"{o.name}: {len(got)} corners in Blender, {None if want is None else len(want)} in the OBJ (cannot pair)"); continue
    worst = max(max(abs(a[0] - b[0]), abs(a[1] - b[1])) for a, b in zip(got, want))
    print(f"{o.name}: {len(mesh.polygons)} faces, {len(got)} corners, largest UV difference {worst:.6f}")

print("=== materials")
for m in bpy.data.materials:
    images = [n.image for n in (m.node_tree.nodes if m.node_tree else []) if n.type == 'TEX_IMAGE' and n.image]
    for im in images:
        print(f"{m.name}: {os.path.basename(im.filepath)} {im.size[0]}x{im.size[1]} loaded={im.has_data or im.size[0] > 0}")
    if not images:
        print(f"{m.name}: no image")

# --- a textured render of each object, facing its front ---------------------------------
scene = bpy.context.scene
scene.render.engine = os.environ.get('FRS_ENGINE', 'BLENDER_WORKBENCH')
scene.display.shading.light = 'FLAT'
scene.display.shading.color_type = 'TEXTURE'
scene.render.resolution_x = scene.render.resolution_y = 640
scene.render.film_transparent = False
scene.world = scene.world or bpy.data.worlds.new('w')
cam_data = bpy.data.cameras.new("check"); cam_data.type = 'ORTHO'
cam = bpy.data.objects.new("check", cam_data); scene.collection.objects.link(cam); scene.camera = cam
meshes = [o for o in bpy.data.objects if o.type == 'MESH']
for o in meshes:
    for other in meshes:
        other.hide_render = other is not o
    mesh = o.data
    normal = Vector((0, 0, 0))
    for poly in mesh.polygons:
        normal += (o.matrix_world.to_3x3() @ poly.normal) * poly.area
    if normal.length < 1e-9:
        continue
    normal.normalize()
    corners = [o.matrix_world @ Vector(c) for c in o.bound_box]
    centre = sum(corners, Vector()) / 8
    radius = max((c - centre).length for c in corners)
    cam.location = centre + normal * (radius * 3 + 1)
    cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
    cam_data.ortho_scale = radius * 2.1
    cam_data.clip_end = radius * 10 + 100
    safe = "".join(c if c.isalnum() or c in "_-" else "_" for c in o.name)[:60]
    scene.render.filepath = os.path.join(out, f"blender-{os.environ.get('FRS_TAG', 'wb')}-{safe}.png")
    bpy.ops.render.render(write_still=True)
    print("rendered", scene.render.filepath)
