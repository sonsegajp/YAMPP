import bpy,math,json,sys
variant=sys.argv[sys.argv.index("--")+1] if "--" in sys.argv else "original"
assert variant in ["original","melee"]
from pathlib import Path
from mathutils import Vector
root=Path(__file__).resolve().parents[1];out=root/'build/brawl'/('blender-'+variant);out.mkdir(parents=True,exist_ok=True)
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.gltf(filepath=str(root/f'data/brawl-diddy/blender/diddy-{variant}.glb'))
arm=next(o for o in bpy.data.objects if o.type=='ARMATURE');actions=list(bpy.data.actions)
print('Imported bones',len(arm.data.bones),'actions',len(actions),flush=True)
assert len(arm.data.bones)==71 and len(actions)>=349
for track in arm.animation_data.nla_tracks:track.mute=True
wait=next(a for a in actions if a.name=='Wait1')
arm.animation_data.action=wait
if hasattr(wait,'slots') and wait.slots:arm.animation_data.action_slot=wait.slots[0]
bpy.context.scene.frame_set(1)
meshes=[o for o in bpy.data.objects if o.type=='MESH'];assert meshes
coords=[o.matrix_world@Vector(v) for o in meshes for v in o.bound_box];lo=Vector(tuple(min(v[i] for v in coords) for i in range(3)));hi=Vector(tuple(max(v[i] for v in coords) for i in range(3)))
center=(lo+hi)/2;size=max(hi-lo);print('Bounds',tuple(lo),tuple(hi),flush=True)
assert 3<size<100
cam_data=bpy.data.cameras.new('Review camera');cam=bpy.data.objects.new('Review camera',cam_data);bpy.context.collection.objects.link(cam);cam.location=center+Vector((size*.65,-size*1.7,size*.32));cam.rotation_euler=(center-cam.location).to_track_quat('-Z','Y').to_euler();cam_data.type='ORTHO';cam_data.ortho_scale=size*1.1
scene=bpy.context.scene;scene.camera=cam;scene.render.engine='CYCLES';scene.cycles.samples=24;scene.render.resolution_x=640;scene.render.resolution_y=640;scene.render.resolution_percentage=100;scene.render.film_transparent=True
scene.world.color=(.3,.3,.3)
for name,offset,power in [('Key',(15,-20,25),3500),('Fill',(-15,-10,10),1800),('Rim',(0,15,20),3000)]:
 d=bpy.data.lights.new(name,'AREA');d.energy=power;d.shape='DISK';d.size=12;o=bpy.data.objects.new(name,d);bpy.context.collection.objects.link(o);o.location=center+Vector(offset);o.rotation_euler=(center-o.location).to_track_quat('-Z','Y').to_euler()
scene.view_settings.view_transform='Standard'
scene.render.image_settings.file_format='PNG';scene.render.filepath=str(out/'diddy-idle.png');bpy.ops.render.render(write_still=True)
scene.frame_set(12);deps=bpy.context.evaluated_depsgraph_get();arm_eval=arm.evaluated_get(deps)
assert all(math.isfinite(v) for b in arm_eval.pose.bones for row in b.matrix for v in row)
bpy.ops.wm.save_as_mainfile(filepath=str(root/f'data/brawl-diddy/blender/diddy-{variant}.blend'))
(out/'report.json').write_text(json.dumps({'passed':True,'bones':len(arm.data.bones),'actions':len(actions),'meshes':len(meshes),'bounds':[list(lo),list(hi)]},indent=2))
