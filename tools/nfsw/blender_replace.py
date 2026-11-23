# Run by test_replace.ps1 after frs_quickedit.py, in Blender's background mode:
#   FRS_EDIT=swap   the second object of the export is deleted and a sphere with a texture packed into the
#                   scene (as a .glb brings it) is added where it stood: a model swapped for another one;
#                   the first object gets a new material (packed image too) on half its faces: a material
#                   none of the shipped model's.
#   FRS_EDIT=again  the session opened again (its .blend): the sphere is moved up 2 m.
# Then the scene is saved, which is what makes frs_quickedit.py send the OBJ.
import os

import bmesh
import bpy


def packed_material(name, colour):
    image = bpy.data.images.new(name + "_img", 64, 64)
    pixels = []
    for y in range(64):
        for x in range(64):
            on = ((x // 8) + (y // 8)) % 2 == 0
            pixels += list(colour) + [1.0] if on else [1.0, 1.0, 1.0, 1.0]
    image.pixels = pixels
    image.pack()
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    shader = material.node_tree.nodes.get("Principled BSDF")
    texture = material.node_tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    material.node_tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
    return material


meshes = [o for o in bpy.data.objects if o.type == 'MESH']
mode = os.environ.get("FRS_EDIT", "swap")
if mode == "swap":
    first, second = (meshes[0], meshes[1]) if len(meshes) > 1 else (None, meshes[0])
    print("FRS test: keeps", first.name if first else "-", "deletes", second.name)
    centre = sum((second.matrix_world @ v.co for v in second.data.vertices), second.location * 0) / len(second.data.vertices)
    bpy.data.objects.remove(second, do_unlink=True)

    bpy.ops.mesh.primitive_uv_sphere_add(radius=2.0, location=centre)
    sphere = bpy.context.active_object
    sphere.name = "Bola"
    sphere.data.materials.append(packed_material("Bola_Azul", (0.1, 0.3, 0.9)))

    if first:
        first.data.materials.append(packed_material("Faixa_Verde", (0.1, 0.8, 0.2)))
        slot = len(first.data.materials) - 1
        for face in first.data.polygons[: len(first.data.polygons) // 2]:
            face.material_index = slot
elif mode == "again":
    sphere = bpy.data.objects["Bola"]
    sphere.location.z += 2.0   # Blender Z is the game's up (the axes are set on import)
    print("FRS test: moved", sphere.name)

bpy.ops.wm.save_mainfile()
