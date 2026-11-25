"""Run with Blender: blender -b -t 1 -P export-cd-mesh.py -- input.fbx output.bin."""

import struct
import sys
from pathlib import Path

import bpy


def main():
    source, destination = map(Path, sys.argv[sys.argv.index("--") + 1 :])
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(source.resolve()), use_image_search=False)
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    if len(meshes) != 1 or not meshes[0].data.uv_layers.active:
        raise RuntimeError("Expected one mesh with an active UV map")
    destination.parent.mkdir(parents=True, exist_ok=True)
    mesh = meshes[0].data
    mesh.calc_loop_triangles()
    positions = [v.co for v in mesh.vertices]
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    axis = min(range(3), key=lambda i: hi[i] - lo[i])
    order = [(axis + 1) % 3, (axis + 2) % 3, axis]
    radius = max(hi[i] - lo[i] for i in order[:2]) / 2
    if radius <= 0:
        raise RuntimeError("Disc mesh has no usable radius")
    uv = mesh.uv_layers.active.data
    with destination.open("wb") as out:
        for tri in mesh.loop_triangles:
            for loop in tri.loops:
                p = positions[mesh.loops[loop].vertex_index]
                u, v = uv[loop].uv
                out.write(struct.pack(">5f", *((p[i] - (hi[i] + lo[i]) / 2) / radius
                                               for i in order), u, 1 - v))


if __name__ == "__main__":
    main()
