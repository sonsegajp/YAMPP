"""Blender-compatible glTF 2.0 interchange. Game coordinates are X right/Y up."""
import base64,io,json,math,struct
from pathlib import Path
import numpy as np
from PIL import Image

DTYPES={5120:'i1',5121:'u1',5122:'<i2',5123:'<u2',5125:'<u4',5126:'<f4'}
WIDTHS={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}

def quaternion(e):
 x,y,z=np.asarray(e)/2;cx,cy,cz=np.cos([x,y,z]);sx,sy,sz=np.sin([x,y,z])
 return [sx*cy*cz-cx*sy*sz,cx*sy*cz+sx*cy*sz,cx*cy*sz-sx*sy*cz,cx*cy*cz+sx*sy*sz]

def matrix(node):
 if 'matrix' in node:return np.array(node['matrix'],float).reshape(4,4).T
 x,y,z,w=node.get('rotation',[0,0,0,1]);m=np.eye(4)
 m[:3,:3]=np.array([[1-2*y*y-2*z*z,2*x*y-2*z*w,2*x*z+2*y*w],[2*x*y+2*z*w,1-2*x*x-2*z*z,2*y*z-2*x*w],[2*x*z-2*y*w,2*y*z+2*x*w,1-2*x*x-2*y*y]])@np.diag(node.get('scale',[1,1,1]))
 m[:3,3]=node.get('translation',[0,0,0]);return m

class GLB:
 def __init__(self):
  self.doc={'asset':{'version':'2.0','generator':'Melee Workshop'},'scene':0,'scenes':[{'nodes':[]}],'nodes':[],'meshes':[],'materials':[],'bufferViews':[],'accessors':[]};self.data=bytearray()
 def view(self,data):
  self.data.extend(b'\0'*(-len(self.data)%4));offset=len(self.data);self.data.extend(data)
  self.doc['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(data)});return len(self.doc['bufferViews'])-1
 def acc(self,values,kind='VEC3',component=5126):
  a=np.asarray(values,dtype=DTYPES[component]);a=a.reshape(-1,WIDTHS[kind]);v=self.view(a.tobytes())
  d={'bufferView':v,'componentType':component,'count':len(a),'type':kind}
  if len(a) and kind!='MAT4':d.update(min=a.min(axis=0).tolist(),max=a.max(axis=0).tolist())
  self.doc['accessors'].append(d);return len(self.doc['accessors'])-1
 def material(self,color,texture=None):
  pbr={'baseColorFactor':color,'metallicFactor':0,'roughnessFactor':.8};mat={'pbrMetallicRoughness':pbr,'doubleSided':True}
  if texture:
   rgba=base64.b64decode(texture['rgba']);im=Image.frombytes('RGBA',(texture['width'],texture['height']),rgba);out=io.BytesIO();im.save(out,format='PNG')
   images=self.doc.setdefault('images',[]);images.append({'bufferView':self.view(out.getvalue()),'mimeType':'image/png'})
   samplers=self.doc.setdefault('samplers',[]);wrap={0:33071,1:10497,2:33648};samplers.append({'wrapS':wrap.get(texture.get('wrapS',1),10497),'wrapT':wrap.get(texture.get('wrapT',1),10497)})
   textures=self.doc.setdefault('textures',[]);textures.append({'source':len(images)-1,'sampler':len(samplers)-1});pbr['baseColorTexture']={'index':len(textures)-1}
  if color[3]<1:mat['alphaMode']='BLEND'
  elif texture:
   mat['alphaMode']=texture.get('alphaMode','MASK')
   if mat['alphaMode']=='MASK':mat['alphaCutoff']=.2
  self.doc['materials'].append(mat);return len(self.doc['materials'])-1
 def write(self,path):
  self.doc['buffers']=[{'byteLength':len(self.data)}]
  js=json.dumps(self.doc,separators=(',',':'),allow_nan=False).encode();js+=b' '*(-len(js)%4)
  buf=bytes(self.data)+b'\0'*(-len(self.data)%4)
  Path(path).write_bytes(struct.pack('<III',0x46546c67,2,28+len(js)+len(buf))+struct.pack('<II',len(js),0x4e4f534a)+js+struct.pack('<II',len(buf),0x004e4942)+buf)

 def add_mesh(self,m,positions=None,normals=None,skin=False):
  attrs={'POSITION':self.acc(m['positions'] if positions is None else positions),'NORMAL':self.acc(m['normals'] if normals is None else normals)}
  uv=np.asarray(m.get('texcoords',[[0,0]]*len(m['positions'])),float)
  tex=m.get('texture')
  if tex:uv=uv*np.array(tex.get('scale',[1,1]))+np.array(tex.get('offset',[0,0]))
  attrs['TEXCOORD_0']=self.acc(uv,'VEC2')
  if skin:
   attrs['JOINTS_0']=self.acc(m['skin'],'VEC4',5123)
   w=np.asarray(m['weights'],float);s=w.sum(axis=1);w=w/np.where(s==0,1,s)[:,None];attrs['WEIGHTS_0']=self.acc(w,'VEC4')
  primitive={'attributes':attrs,'indices':self.acc(m['indices'],'SCALAR',5125),'material':self.material(m.get('color',[1,1,1,1]),tex)}
  self.doc['meshes'].append({'name':m['name'],'primitives':[primitive]});return len(self.doc['meshes'])-1

def export_model(model,path,animations=(),*,source_winding='counterclockwise'):
 # glTF triangles face counterclockwise. Native Melee GX exports face clockwise;
 # reverse their indices, retaining the authored split normals and UV seams.
 # DAE and other glTF-native callers keep their existing counterclockwise order.
 if source_winding not in ('clockwise','counterclockwise'):
  raise ValueError('Unknown source triangle winding')
 g=GLB();world=[];bones=model['bones']
 g.doc['asset']['extras']={'melee_character':model.get('modelParts',{})}
 for b in bones:
  n={k:b[k] for k in ['name','translation','scale']};n['rotation']=quaternion(b['rotation']);n['extras']={'melee_bone_id':b['id'],'hsd_flags':b['flags']}
  local=matrix(n);world.append(local if b['parent']<0 else world[b['parent']]@local)
  g.doc['nodes'].append(n)
  if b['parent']<0:g.doc['scenes'][0]['nodes'].append(b['id'])
  else:g.doc['nodes'][b['parent']].setdefault('children',[]).append(b['id'])
 g.doc['skins']=[{'joints':list(range(len(bones))),'inverseBindMatrices':g.acc([np.linalg.inv(m).T.ravel() for m in world],'MAT4')}]
 for m in model['meshes']:
  if not m['indices']:continue
  p=np.asarray(m['positions'],float).copy();n=np.asarray(m['normals'],float).copy()
  for i,rigid in enumerate(m['rigid']):
   if rigid:
    w=world[m['skin'][i][0]];p[i]=(w@np.r_[p[i],1])[:3];n[i]=np.linalg.inv(w[:3,:3]).T@n[i]
  indices=np.asarray(m['indices'],dtype=np.uint32).reshape(-1,3)
  if source_winding=='clockwise':indices=indices[:,[0,2,1]]
  exported=dict(m,indices=indices.reshape(-1))
  mesh=g.add_mesh(exported,p,n,True);node={'name':m['name'],'mesh':mesh,'skin':0,'extras':{'melee_hidden':m.get('hidden',False),'melee_joint':m['joint'],'melee_dobj':m.get('dobj',-1),'melee_category':m.get('category','main'),'melee_default_visible':m.get('defaultVisible',True),'melee_model_parts':m.get('parts',[])}}
  g.doc['scenes'][0]['nodes'].append(len(g.doc['nodes']));g.doc['nodes'].append(node)
 for name,anim in animations:
  channels=[];samplers=[];frames=int(math.ceil(anim['frames']))+1;time=g.acc(np.arange(frames)/60,'SCALAR')
  for b,tracks in enumerate(anim['nodes'][:len(bones)]):
   values={t['type']:t['values'] for t in tracks}
   for prop,ids,default,kind in [('translation',[5,6,7],bones[b]['translation'],'VEC3'),('rotation',[1,2,3],bones[b]['rotation'],'VEC4'),('scale',[8,9,10],bones[b]['scale'],'VEC3')]:
    if not any(t in values for t in ids):continue
    v=np.array([values.get(t,[default[i]]*frames) for i,t in enumerate(ids)]).T
    if prop=='rotation':
     v=np.array([quaternion(e) for e in v])
     for k in range(1,len(v)):
      if np.dot(v[k-1],v[k])<0:v[k]*=-1
    samplers.append({'input':time,'output':g.acc(v,kind),'interpolation':'LINEAR'});channels.append({'sampler':len(samplers)-1,'target':{'node':b,'path':prop}})
  if channels:g.doc.setdefault('animations',[]).append({'name':name,'samplers':samplers,'channels':channels})
 g.write(path)

def load_gltf(path):
 path=Path(path);raw=path.read_bytes();binary=None
 if raw[:4]==b'glTF':
  if len(raw)<20 or struct.unpack_from('<I',raw,4)[0]!=2:raise ValueError('Expected glTF 2.0')
  cursor=12;doc=None
  while cursor+8<=len(raw):
   size,kind=struct.unpack_from('<II',raw,cursor);cursor+=8;chunk=raw[cursor:cursor+size];cursor+=size
   if kind==0x4e4f534a:doc=json.loads(chunk)
   elif kind==0x004e4942:binary=chunk
 else:doc=json.loads(raw)
 if doc.get('asset',{}).get('version')!='2.0':raise ValueError('Expected glTF 2.0')
 if doc.get('extensionsRequired'):raise ValueError('Required glTF extensions need baking first: '+', '.join(doc['extensionsRequired']))
 def uri(value):
  if value.startswith('data:'):return base64.b64decode(value.split(',',1)[1],validate=True)
  p=(path.parent/value).resolve()
  if not p.is_relative_to(path.parent.resolve()):raise ValueError('Asset URI leaves the import directory')
  return p.read_bytes()
 buffers=[uri(b['uri']) if 'uri' in b else binary for b in doc.get('buffers',[])]
 def view(i):
  v=doc['bufferViews'][i];start=v.get('byteOffset',0);return buffers[v['buffer']][start:start+v['byteLength']]
 def access(i):
  a=doc['accessors'][i];width=WIDTHS[a['type']];dtype=np.dtype(DTYPES[a['componentType']]);count=a['count']
  if count>2000000:raise ValueError('Accessor exceeds two million elements')
  if 'bufferView' in a:
   v=doc['bufferViews'][a['bufferView']];stride=v.get('byteStride',dtype.itemsize*width)
   result=np.ndarray((count,width),dtype,view(a['bufferView']),offset=a.get('byteOffset',0),strides=(stride,dtype.itemsize)).copy()
  else:result=np.zeros((count,width),dtype)
  if 'sparse' in a:
   s=a['sparse'];ix=s['indices'];val=s['values'];indices=np.frombuffer(view(ix['bufferView']),DTYPES[ix['componentType']],s['count'],ix.get('byteOffset',0));result[indices]=np.frombuffer(view(val['bufferView']),dtype,s['count']*width,val.get('byteOffset',0)).reshape(-1,width)
  if a.get('normalized') and a['componentType']!=5126:
   result=result.astype(float)/np.iinfo(dtype).max;result=np.maximum(result,-1)
  if not np.isfinite(result).all():raise ValueError('Mesh contains non-finite values')
  return result
 meshes=[];roles=[];seen=set()
 def visit(index,parent):
  if index in seen:raise ValueError('Repeated/cyclic nodes are unsupported in a stage scene')
  seen.add(index);node=doc['nodes'][index];transform=parent@matrix(node)
  if 'skin' in node:raise ValueError('Stage import requires static meshes; bake armatures in Blender first')
  role=node.get('extras',{}).get('melee_role','background' if node.get('name','').lower().startswith('background') else 'geometry')
  for pi,p in enumerate(doc.get('meshes',[])[node['mesh']]['primitives'] if 'mesh' in node else []):
   vertices=access(p['attributes']['POSITION']);pos=(transform@np.c_[vertices,np.ones(len(vertices))].T).T[:,:3]
   idx=access(p['indices']).ravel().astype(int) if 'indices' in p else np.arange(len(vertices));mode=p.get('mode',4)
   if len(idx) and (idx.min()<0 or idx.max()>=len(vertices)):raise ValueError('Triangle index out of bounds')
   if mode==5:idx=np.array([[idx[i-2],idx[i-1],idx[i]] if i%2==0 else [idx[i-1],idx[i-2],idx[i]] for i in range(2,len(idx))]).ravel()
   elif mode==6:idx=np.array([[idx[0],idx[i-1],idx[i]] for i in range(2,len(idx))]).ravel()
   elif mode!=4:raise ValueError('Use triangle meshes for stage geometry')
   if len(idx)%3:raise ValueError('Incomplete triangle')
   if np.linalg.det(transform[:3,:3])<0:idx=idx.reshape(-1,3)[:,[0,2,1]].ravel()
   normal=access(p['attributes']['NORMAL']) if 'NORMAL' in p['attributes'] else np.zeros_like(vertices)
   if 'NORMAL' in p['attributes']:normal=(np.linalg.inv(transform[:3,:3]).T@normal.T).T
   else:
    for a,b,c in idx.reshape(-1,3):normal[[a,b,c]]+=np.cross(pos[b]-pos[a],pos[c]-pos[a])
   length=np.linalg.norm(normal,axis=1);normal/=np.where(length==0,1,length)[:,None]
   uv=access(p['attributes']['TEXCOORD_0']) if 'TEXCOORD_0' in p['attributes'] else np.zeros((len(pos),2))
   mat=doc.get('materials',[])[p['material']] if 'material' in p else {};pbr=mat.get('pbrMetallicRoughness',{});texture=None
   if 'baseColorTexture' in pbr:
    ti=doc['textures'][pbr['baseColorTexture']['index']];im=doc['images'][ti['source']];data=uri(im['uri']) if 'uri'in im else view(im['bufferView']);image=Image.open(io.BytesIO(data)).convert('RGBA')
    if max(image.size)>1024:image.thumbnail((1024,1024))
    # GameCube RGBA8 textures are tiled in four-pixel blocks.
    w,h=image.size;image=image.resize((max(4,(w+3)//4*4),max(4,(h+3)//4*4)))
    sampler=doc.get('samplers',[])[ti['sampler']] if 'sampler'in ti else {};wrap={33071:0,10497:1,33648:2}
    texture={'width':image.width,'height':image.height,'rgba':base64.b64encode(image.tobytes()).decode(),'wrapS':wrap.get(sampler.get('wrapS'),1),'wrapT':wrap.get(sampler.get('wrapT'),1)}
   meshes.append({'name':node.get('name','Mesh_'+str(index))+'_'+str(pi),'role':role,'positions':pos.tolist(),'normals':normal.tolist(),'texcoords':uv.tolist(),'indices':idx.tolist(),'color':pbr.get('baseColorFactor',[1,1,1,1]),'texture':texture})
  for child in node.get('children',[]):visit(child,transform)
 for index in doc['scenes'][doc.get('scene',0)]['nodes']:visit(index,np.eye(4))
 if not meshes:raise ValueError('No stage meshes found')
 if sum(len(m['indices']) for m in meshes)>600000:raise ValueError('Stage exceeds 200,000 triangles; simplify the scene in Blender')
 return {'meshes':meshes,'metadata':doc['asset'].get('extras',{}).get('melee',{})}

def export_stage(scene,path):
 g=GLB();g.doc['asset']['extras']={'melee':scene.get('metadata',{})}
 for m in scene['meshes']:
  mesh=g.add_mesh(m);g.doc['scenes'][0]['nodes'].append(len(g.doc['nodes']));g.doc['nodes'].append({'name':m['name'],'mesh':mesh,'extras':{'melee_role':m.get('role','geometry')}})
 g.write(path)
