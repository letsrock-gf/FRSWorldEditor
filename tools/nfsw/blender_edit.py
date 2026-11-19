"""Scripted Blender save for the quick-edit round trip. Run after blender/frs_quickedit.py in the same Blender:
  blender --background --python blender/frs_quickedit.py --python tools/nfsw/blender_edit.py -- <file.obj>
Object 1 (by name order): its upper half is raised 2 units (same vertex and triangle count).
Object 2, if any: four faces are cut (more vertices and triangles) and one face is deleted.
FRS_EDIT=cut: object 1 is cut instead (and raised too), for a selection of one.
Then the .blend is saved, which makes frs_quickedit export the OBJ and touch <obj>.sent, as Ctrl+S does."""
import bmesh
import bpy

meshes = sorted((o for o in bpy.data.objects if o.type == 'MESH'), key=lambda o: o.name)
print("FRS edit: objects", [(o.name, len(o.data.vertices), len(o.data.polygons)) for o in meshes])

import os
cut_first = os.environ.get("FRS_EDIT") == "cut"
if meshes:
    first = meshes[0].data
    zs = sorted(v.co.z for v in first.vertices)
    mid = zs[len(zs) // 2]
    moved = 0
    for v in first.vertices:
        if v.co.z >= mid:
            v.co.z += 2.0
            moved += 1
    first.update()
    print("FRS edit: raised", moved, "vertices of", meshes[0].name)

if len(meshes) > 1 or cut_first:
    target = meshes[0] if cut_first else meshes[1]
    mesh = target.data
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.faces.ensure_lookup_table()
    faces = bm.faces[:4]
    edges = list({e for f in faces for e in f.edges})
    bmesh.ops.subdivide_edges(bm, edges=edges, cuts=1, use_grid_fill=True)
    bm.faces.ensure_lookup_table()
    bmesh.ops.delete(bm, geom=[bm.faces[len(bm.faces) - 1]], context='FACES')
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    print("FRS edit: cut", target.name, "now", len(mesh.vertices), "vertices", len(mesh.polygons), "faces")

bpy.ops.wm.save_mainfile()
print("FRS edit: saved")
