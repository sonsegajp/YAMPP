bl_info = {"name":"Melee Workshop","author":"YAMPP contributors","version":(0,1,0),"blender":(4,2,0),"category":"Import-Export","description":"Blender glTF interchange with Melee platform, spawn, and boundary metadata"}
import bpy,json,struct
from bpy_extras.io_utils import ImportHelper,ExportHelper
from bpy.props import StringProperty,EnumProperty

def glb_document(path):
 data=open(path,'rb').read()
 if data[:4]!=b'glTF':return json.loads(data),None
 offset=12;doc=None;binary=None
 while offset+8<=len(data):
  size,kind=struct.unpack_from('<II',data,offset);offset+=8;chunk=data[offset:offset+size];offset+=size
  if kind==0x4e4f534a:doc=json.loads(chunk)
  elif kind==0x004e4942:binary=chunk
 return doc,binary

def inject_metadata(path,metadata):
 doc,binary=glb_document(path);doc['asset'].setdefault('extras',{})['melee']=metadata
 js=json.dumps(doc,separators=(',',':')).encode();js+=b' '*(-len(js)%4);binary=binary or b'';binary+=b'\0'*(-len(binary)%4)
 with open(path,'wb') as f:f.write(struct.pack('<III',0x46546c67,2,28+len(js)+len(binary))+struct.pack('<II',len(js),0x4e4f534a)+js+struct.pack('<II',len(binary),0x004e4942)+binary)

def make_platform(name,a,b,drop):
 mesh=bpy.data.meshes.new(name);mesh.from_pydata([(a[0],0,a[1]),(b[0],0,b[1])],[(0,1)],[]);obj=bpy.data.objects.new(name,mesh);bpy.context.collection.objects.link(obj);obj['melee_role']='collision';obj['melee_drop_through']=drop;obj.show_in_front=True;obj.display_type='WIRE';return obj

class MW_OT_import(bpy.types.Operator,ImportHelper):
 bl_idname='import_scene.melee_workshop';bl_label='Melee Workshop (.glb/.gltf)';filename_ext='.glb'
 filter_glob:StringProperty(default='*.glb;*.gltf',options={'HIDDEN'})
 def execute(self,context):
  doc,_=glb_document(self.filepath);bpy.ops.import_scene.gltf(filepath=self.filepath)
  meta=doc.get('asset',{}).get('extras',{}).get('melee')
  if meta:
   context.scene['melee_stage_metadata']=json.dumps(meta)
   for p in meta.get('platforms',[]):make_platform(p.get('name','Platform'),p['a'],p['b'],p['dropThrough'])
  self.report({'INFO'},'Imported model, materials, rig and available animations');return {'FINISHED'}

class MW_OT_export(bpy.types.Operator,ExportHelper):
 bl_idname='export_scene.melee_workshop';bl_label='Melee Workshop (.glb)';filename_ext='.glb'
 filter_glob:StringProperty(default='*.glb',options={'HIDDEN'})
 def execute(self,context):
  metadata=json.loads(context.scene.get('melee_stage_metadata','{}'));platforms=[];hidden=[]
  for obj in context.scene.objects:
   if obj.get('melee_role')=='collision' and obj.type=='MESH':
    for edge in obj.data.edges:
     a=obj.matrix_world@obj.data.vertices[edge.vertices[0]].co;b=obj.matrix_world@obj.data.vertices[edge.vertices[1]].co
     if a.x>b.x:a,b=b,a
     platforms.append({'name':obj.name,'a':[a.x,a.z],'b':[b.x,b.z],'dropThrough':bool(obj.get('melee_drop_through',True))})
    hidden.append((obj,obj.hide_viewport));obj.hide_viewport=True
  if platforms:metadata['platforms']=platforms
  try:bpy.ops.export_scene.gltf(filepath=self.filepath,export_format='GLB',export_extras=True,export_animations=True,use_visible=True)
  finally:
   for obj,value in hidden:obj.hide_viewport=value
  if metadata:inject_metadata(self.filepath,metadata)
  self.report({'INFO'},'Exported GLB with Melee metadata');return {'FINISHED'}

class MW_OT_platform(bpy.types.Operator):
 bl_idname='melee_workshop.add_platform';bl_label='Add collision platform';bl_options={'REGISTER','UNDO'}
 def execute(self,context):
  obj=make_platform('Platform',[-25,30],[25,30],True);bpy.ops.object.select_all(action='DESELECT');obj.select_set(True);context.view_layer.objects.active=obj;return {'FINISHED'}

class MW_PT_tools(bpy.types.Panel):
 bl_label='Melee Workshop';bl_idname='MW_PT_tools';bl_space_type='VIEW_3D';bl_region_type='UI';bl_category='Melee'
 def draw(self,context):
  layout=self.layout;layout.operator(MW_OT_import.bl_idname);layout.operator(MW_OT_export.bl_idname);layout.separator();layout.operator(MW_OT_platform.bl_idname)
  obj=context.object
  if obj:
   if 'melee_role' not in obj:layout.label(text='Use object custom property melee_role:');layout.label(text='geometry, background, or collision')
   else:layout.prop(obj,'["melee_role"]',text='Role')
   if 'melee_drop_through' in obj:layout.prop(obj,'["melee_drop_through"]',text='Drop-through')
  layout.label(text='Blender X/Z = Melee X/Y')

CLASSES=(MW_OT_import,MW_OT_export,MW_OT_platform,MW_PT_tools)
def menu_import(self,context):self.layout.operator(MW_OT_import.bl_idname,text='Melee Workshop (.glb/.gltf)')
def menu_export(self,context):self.layout.operator(MW_OT_export.bl_idname,text='Melee Workshop (.glb)')
def register():
 for c in CLASSES:bpy.utils.register_class(c)
 bpy.types.TOPBAR_MT_file_import.append(menu_import);bpy.types.TOPBAR_MT_file_export.append(menu_export)
def unregister():
 bpy.types.TOPBAR_MT_file_import.remove(menu_import);bpy.types.TOPBAR_MT_file_export.remove(menu_export)
 for c in reversed(CLASSES):bpy.utils.unregister_class(c)
if __name__=='__main__':register()
