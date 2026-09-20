"""Render native-size CSS and transparent stock art from the editable 3D scene."""
import bpy,json,base64,struct
from pathlib import Path
from mathutils import Vector
root=Path(__file__).resolve().parents[1];out=root/'data/brawl-diddy/art'
bpy.ops.wm.open_mainfile(filepath=str(out/'diddy-css.blend'))
s=bpy.context.scene;s.cycles.samples=96;s.render.resolution_percentage=100;s.render.resolution_x=64;s.render.resolution_y=56;s.render.filepath=str(out/'css-icon.png');bpy.ops.render.render(write_still=True)
# Isolate geometry actually weighted to the head or its descendants.
arm=next(o for o in bpy.data.objects if o.type=='ARMATURE');head=arm.data.bones['HeadN'];names={head.name}|{b.name for b in head.children_recursive}
for obj in list(bpy.data.objects):
 if obj.parent==s.camera:obj.hide_render=True
 if obj.type!='MESH' or not any(m.type=='ARMATURE' for m in obj.modifiers):continue
 group={g.index for g in obj.vertex_groups if g.name in names}
 allowed={v.index for v in obj.data.vertices if sum(g.weight for g in v.groups if g.group in group)>.8}
 faces=[p for p in obj.data.polygons if all(i in allowed for i in p.vertices)]
 if not faces:obj.hide_render=True;continue
 if len(faces)==len(obj.data.polygons):continue
 import bmesh
 bm=bmesh.new();bm.from_mesh(obj.data);bm.verts.ensure_lookup_table();bm.faces.ensure_lookup_table()
 remove=[f for f in bm.faces if any(v.index not in allowed for v in f.verts)]
 bmesh.ops.delete(bm,geom=remove,context='FACES');bm.to_mesh(obj.data);bm.free()
cam=s.camera;headpos=arm.matrix_world@arm.pose.bones['HeadN'].matrix.translation;target=headpos+Vector((0,-.5,0));cam.location=target+Vector((-7,-20,2.5));cam.rotation_euler=(target-cam.location).to_track_quat('-Z','Y').to_euler();cam.data.ortho_scale=11.2
s.render.resolution_x=24;s.render.resolution_y=24;s.render.film_transparent=True;s.render.filepath=str(out/'stock-icon.png');bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_as_mainfile(filepath=str(out/'diddy-stock.blend'))
