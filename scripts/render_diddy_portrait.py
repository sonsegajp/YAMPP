"""Render the imported Diddy scene as an editable Melee-style CSS tile."""
import bpy,math,json
from pathlib import Path
from mathutils import Vector
root=Path(__file__).resolve().parents[1];out=root/'data/brawl-diddy/art'
bpy.ops.wm.open_mainfile(filepath=str(root/'data/brawl-diddy/blender/diddy-melee.blend'))
scene=bpy.context.scene;arm=next(o for o in bpy.data.objects if o.type=='ARMATURE');scene.frame_set(1)
arm.rotation_euler.z+=math.pi
bpy.context.view_layer.update()
head=arm.matrix_world@arm.pose.bones['HeadN'].matrix.translation;target=head+Vector((0,-.5,-.55));cam=scene.camera
cam.location=target+Vector((-7,-20,2.5));cam.rotation_euler=(target-cam.location).to_track_quat('-Z','Y').to_euler();cam.data.ortho_scale=10.2;cam.data.clip_start=.01
scene.render.resolution_x=512;scene.render.resolution_y=448;scene.render.resolution_percentage=100;scene.cycles.samples=64;scene.render.film_transparent=False;scene.world.color=(.005,.001,.001)
if scene.world.use_nodes:
 scene.world.node_tree.nodes.get('Background').inputs['Color'].default_value=(.006,.001,.001,1)
for name,pos,energy,color in [('Key',(-9,-18,22),6500,(1,.82,.48)),('Fill',(-4,-20,0),7500,(1,.90,.72)),('Rim',(0,8,20),4000,(1,.3,.1))]:
 obj=bpy.data.objects.get(name);obj.location=head+Vector(pos);obj.data.energy=energy;obj.data.color=color;obj.data.size=8
 obj.rotation_euler=(head-obj.location).to_track_quat('-Z','Y').to_euler()
# Screen-aligned scene geometry keeps border/text editable in Blender.
w=cam.data.ortho_scale;h=w*448/512

def emission(name,color):
 mat=bpy.data.materials.new(name);mat.use_nodes=True;nodes=mat.node_tree.nodes;nodes.clear();em=nodes.new('ShaderNodeEmission');em.inputs['Color'].default_value=(*color,1);output=nodes.new('ShaderNodeOutputMaterial');mat.node_tree.links.new(em.outputs[0],output.inputs['Surface']);return mat

def rect(name,x,y,ww,hh,color,depth=-.2):
 mesh=bpy.data.meshes.new(name);mesh.from_pydata([(-ww/2,-hh/2,0),(ww/2,-hh/2,0),(ww/2,hh/2,0),(-ww/2,hh/2,0)],[],[(0,1,2,3)]);o=bpy.data.objects.new(name,mesh);bpy.context.collection.objects.link(o);o.parent=cam;o.location=(x,y,depth);o.data.materials.append(emission(name,color))
px=w/64
rect('Name strip',0,-h/2+6.5*px,w,13*px,(.017,.001,.002))
rect('Top highlight',0,h/2-px,w,2*px,(.49,.49,.49),-.15);rect('Left highlight',-w/2+px,0,2*px,h,(.49,.49,.49),-.15)
rect('Bottom shadow',0,-h/2+px,w,2*px,(.20,.20,.20),-.15);rect('Right shadow',w/2-px,0,2*px,h,(.20,.20,.20),-.15)
text=bpy.data.curves.new('Diddy label','FONT');text.body='DIDDY';text.align_x='CENTER';text.align_y='CENTER';text.size=7*px;text.space_character=1.25;text.extrude=0
font=Path('C:/Windows/Fonts/arialbd.ttf')
if font.exists():text.font=bpy.data.fonts.load(str(font))
o=bpy.data.objects.new('Diddy label',text);bpy.context.collection.objects.link(o);o.parent=cam;o.location=(0,-h/2+6.5*px,-.1);text.materials.append(emission('Metallic label',(.62,.62,.62)))
scene.render.image_settings.file_format='PNG';scene.render.filepath=str(out/'diddy-css-render.png');bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_as_mainfile(filepath=str(out/'diddy-css.blend'))
