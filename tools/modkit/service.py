"""Melee Workshop asset operations shared by the desktop worker."""
import argparse,base64,hashlib,json,mimetypes,os,re,shutil,subprocess,sys,threading,time,traceback,uuid
from pathlib import Path
from urllib.parse import urlparse,parse_qs,unquote
from catalog import *
from interchange import export_model,export_stage,load_gltf
from model_parts import script_events,describe,annotate,visible_dobjs
BRIDGE=ROOT/'build/modkit/bridge/ModBridge.exe'
WEB=Path(__file__).parent/'web'
LOCK=threading.RLock()
import community
community.INSTALL_LOCK=LOCK

def run_bridge(*args):
 p=subprocess.run([str(BRIDGE),*map(str,args)],capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW,timeout=180)
 if p.returncode:raise ValueError(p.stderr.strip()[-2000:] or 'HSD conversion failed')
def cached(f,command,costume=None):
 path=asset_path(f,costume or 'Pl'+f['prefix']+'.dat')
 if not path.is_file():raise ValueError('Asset file is missing')
 key=hashlib.sha256(path.read_bytes()+command.encode()+b'model-parts-v3').hexdigest()[:20]
 out=CACHE/(key+'.json');CACHE.mkdir(parents=True,exist_ok=True)
 with LOCK:
  if not out.exists():run_bridge(command,path,out)
 return out

def get_fighter(id):
 f=next((item for item in editor_fighters() if item['id']==id),None)
 if not f:raise ValueError('Unknown fighter')
 return f

def editor_fighters():
 result=fighters()
 stock={f['id']:f for f in result if f['stock']}
 for folder in package_folders(MODS):
  m=read_manifest(folder)
  if m.get('kind')!='costume' or not m.get('additive'):continue
  base=stock.get(m.get('base'))
  if not base:continue
  rows=[]
  for index,costume in enumerate(m.get('costumes',[])):
   asset=community.safe_source(folder,costume['file'])
   reference=costume_reference(base['prefix'],asset);reference_index=next((c['index'] for c in base['costumes'] if c['file']==reference),0)
   rows.append(dict(costume,index=reference_index,bytes=asset.stat().st_size))
  result.append(dict(m,stock=False,folder=str(folder),prefix=base['prefix'],external=base['external'],internal=base['internal'],costumes=rows))
 return result


def asset_path(f,filename):
 path=Path(f['folder'])/filename
 if f.get('kind')=='costume':
  if filename in ('Pl'+f['prefix']+'.dat','Pl'+f['prefix']+'AJ.dat'):
   return ASSETS/filename
  return community.safe_source(Path(f['folder']),filename)
 return path


def clone_costumes(data):
 base=get_fighter(data['base'])
 if not base['stock']:raise ValueError('Choose an original fighter as the costume base')
 name=str(data.get('name','')).strip()
 if not name or len(name)>48 or any(ord(c)<32 for c in name):raise ValueError('Use a name of 1 to 48 printable characters')
 chosen=data.get('costumes',[])
 if not isinstance(chosen,list) or not 1<=len(chosen)<=16 or len(set(chosen))!=len(chosen):raise ValueError('Choose 1 to 16 different stock colors')
 allowed={c['file']:c for c in base['costumes']}
 if any(c not in allowed for c in chosen):raise ValueError('Unknown stock costume')
 occupied=len(base['costumes'])+sum(len(m.get('costumes',[])) for folder in package_folders(MODS) if (m:=read_manifest(folder)).get('kind')=='costume' and m.get('enabled',True) and m.get('base')==base['id'])
 if occupied+len(chosen)>64:raise ValueError('This fighter supports at most 64 combined original and additional costumes')
 ident=re.sub('[^a-z0-9]+','-',name.lower()).strip('-')[:44]+'-'+uuid.uuid4().hex[:6]
 folder=MODS/ident;target=folder/'files';target.mkdir(parents=True)
 entries=[]
 for index,filename in enumerate(chosen):
  newname='Pl'+base['prefix']+'C'+str(index+1).zfill(2)+'.dat'
  shutil.copy2(asset_path(base,filename),target/newname)
  entries.append({'id':'color-'+str(index+1),'name':allowed[filename]['name'],'file':'files/'+newname})
 manifest={'schema':1,'kind':'costume','id':ident,'name':name,'version':'1.0.0','base':base['id'],'additive':True,'enabled':True,'costumes':entries}
 write_manifest(folder,manifest);runtime_registry();return manifest


events=script_events

def info(f):
 d=json.loads(cached(f,'inspect').read_text())
 for m in d.get('moves',[]):m['events']=events(m['script']);m['label']=m['name'].split('_ACTION_')[-1].replace('_figatree','') if m.get('name') else 'Action '+str(m['id'])
 d['fighter']={k:v for k,v in f.items() if k!='folder'}
 return d

def animation(f,id):
 move=info(f)['moves'][int(id)]
 if move['animationSize']<32:raise ValueError('This action has no separate animation archive')
 aj=asset_path(f,'Pl'+f['prefix']+'AJ.dat');key=hashlib.sha256(aj.read_bytes()+str(id).encode()).hexdigest()[:20];out=CACHE/('anim-'+key+'.json')
 with LOCK:
  if not out.exists():run_bridge('animation',aj,move['animationOffset'],move['animationSize'],out)
 return json.loads(out.read_text())

def export_fighter(f,costume,all_animations=False,selection="base",preview=False):
 if costume not in [c['file'] for c in f['costumes']]:raise ValueError('Unknown costume')
 modelpath=cached(f,'model',costume);dat=cached(f,'inspect');key=hashlib.sha256(('native-cw-v2:'+modelpath.stem+dat.stem+selection+str(preview)+str(all_animations)).encode()).hexdigest()[:24]
 out=CACHE/(key+'.glb')
 with LOCK:
  if not out.exists():
   animations=[]
   if all_animations:
    for m in info(f)['moves']:
     if m['animationSize']>=32:animations.append((m['label'],animation(f,m['id'])))
   model=annotate(json.loads(modelpath.read_text()),describe(f,info(f),costume))
   if not preview:
    active=visible_dobjs(model['modelParts'],[m['dobj'] for m in model['meshes']],selection)
    model['meshes']=[dict(m,defaultVisible=True) for m in model['meshes'] if m['dobj'] in active and not m['hidden']]
   export_model(model,out,animations,source_winding='clockwise')
 return out

def atomic_json(path,data):
 path.parent.mkdir(parents=True,exist_ok=True);tmp=path.with_suffix(path.suffix+'.tmp');tmp.write_text(json.dumps(data,indent=2,allow_nan=False));tmp.replace(path)

def clone_fighter(base,name):
 f=get_fighter(base);name=name.strip()
 if not name or len(name)>48 or any(ord(c)<32 for c in name):raise ValueError('Use a name of 1 to 48 printable characters')
 id=re.sub('[^a-z0-9]+','-',name.lower()).strip('-')[:44]+'-'+uuid.uuid4().hex[:6];folder=MODS/id
 folder.mkdir(parents=True);target=folder/'files';target.mkdir()
 for p in Path(f['folder']).glob('Pl'+f['prefix']+'*'):
  if p.suffix in ['.dat','.usd']:shutil.copy2(p,target/p.name)
 manifest={'schema':1,'id':id,'name':name,'kind':'fighter','base':f.get('base',f['id']),'prefix':f['prefix'],'external':f['external'],'internal':f['internal'],'enabled':True,'defaultCostume':0,'attributes':dict(f.get('attributes',{})),'moves':list(f.get('moves',[]))}
 if not f['stock']:
  for name in ['art','stock-icon.dat','scripts']:
   original=Path(f['folder']).parent/name
   if original.is_dir():shutil.copytree(original,folder/name)
   elif original.is_file():shutil.copy2(original,folder/name)
 if f.get('lua'):manifest['lua']=dict(f['lua'])
 write_manifest(folder,manifest);runtime_registry();return manifest

def validate_metadata(m):
 platforms=m.get('platforms',[])
 if not 1<=len(platforms)<=1000:raise ValueError('Add at least one collision platform')
 for p in platforms:
  for key in ['a','b']:
   if len(p[key])!=2 or any(not isinstance(v,(int,float)) or abs(v)>10000 for v in p[key]):raise ValueError('Platform coordinates must be finite and within 10000 units')
  if p['a'][0]>=p['b'][0]:raise ValueError('A platform must run from left to right')
 if not 1<=len(m.get('spawns',[]))<=4:raise ValueError('Provide 1 to 4 spawn points')
 for key in ['cameraBounds','blastBounds']:
  b=m.get(key,[])
  if len(b)!=4 or not b[0]<b[1] or not b[2]<b[3]:raise ValueError('Invalid '+key)
 json.dumps(m,allow_nan=False)

def compile_stage(id):
 folder=MODS/validate_id(id);scene=json.loads((folder/'scene.json').read_text());validate_metadata(scene['metadata'])
 run_bridge('stage',ASSETS/'GrNLa.dat',folder/'scene.json',folder/'stage.dat')
 export_stage(scene,folder/'stage.glb');runtime_registry();return {'id':id,'compiledBytes':(folder/'stage.dat').stat().st_size,'download':'/mods/'+id+'/stage.glb'}

class AssetFile:
 def __init__(self,path,download=None):self.path=Path(path).resolve();self.download=download

def read_resource(raw_path):
 url=urlparse(raw_path);q={k:v[0] for k,v in parse_qs(url.query).items()};path=unquote(url.path)
 if path=='/api/community/catalog':return community.catalog(q.get('q',''),q.get('kind',''))
 if path=='/api/community/packages':return community.local_packages()
 if path=='/api/community/job':return community.job(q['id'])
 if path=='/api/lua':
  import lua_scripts
  return lua_scripts.read(q['id'],q.get('file'))
 if path=='/api/catalog':return ({'fighters':[{k:v for k,v in f.items() if k!='folder'} for f in editor_fighters()],'stages':stages(),'formats':['glb','gltf'],'blenderAddon':'/download/blender-addon.py'})
 if path=='/api/fighter':return (info(get_fighter(q['id'])))
 if path=='/api/textures':return texture_list(get_fighter(q['id']),q['costume'])
 if path=='/api/texture':return texture_file(get_fighter(q['id']),q)
 if path=='/api/animation':return (animation(get_fighter(q['id']),q['action']))
 if path=='/api/model':return AssetFile(export_fighter(get_fighter(q['id']),q['costume'],preview=True))
 if path=='/api/export':return AssetFile(export_fighter(get_fighter(q['id']),q['costume'],q.get('animations')=='1',q.get('selection','base')),q['id']+'.glb')
 if path=='/api/stage':return (json.loads((MODS/validate_id(q['id'])/'scene.json').read_text()))
 if path=='/download/blender-addon.py':return AssetFile(Path(__file__).parent/'blender_addon.py','melee_workshop.py')
 if path.startswith('/mods/'):
  parts=path.split('/');folder=MODS/validate_id(parts[2]);name=parts[-1]
  if name not in ['stage.glb','stage.dat','manifest.xml']:raise ValueError('Unknown package asset')
  return AssetFile(folder/name)
 raise ValueError('Unknown asset operation')

def mutate(path,data):
 with LOCK:
  if path=='/api/costume/clone':return clone_costumes(data)
  if path=='/api/community/prepare':return community.prepare(data)
  if path=='/api/community/upload':return community.start_job('upload',data['preparedId'])
  if path=='/api/community/install':return community.start_job('install',data['sha256'])
  if path.startswith('/api/lua/'):
   import lua_scripts
   if path=='/api/lua/validate':return lua_scripts.validate(data['source'],data.get('file','scripts/fighter.lua'))
   if path=='/api/lua/save':return lua_scripts.save(data)
   if path=='/api/lua/launch':return lua_scripts.launch()
  if path=='/api/texture/import':return import_texture(data)
  if path=='/api/clone':return (clone_fighter(data['base'],data['name']))
  if path=='/api/fighter/save':
   f=get_fighter(data['id'])
   if f['stock']:raise ValueError('Clone a stock fighter before editing')
   if f.get('kind')=='costume':raise ValueError('Costume packages contain visual assets only; gameplay data stays original')
   folder=MODS/f['id'];manifest=read_manifest(folder);manifest['attributes']=data['attributes'];manifest['moves']=data.get('moves',manifest.get('moves',[]));manifest['name']=data.get('name',manifest['name'])
   if not manifest['name'] or len(manifest['name'])>48 or any(ord(c)<32 for c in manifest['name']):raise ValueError('Invalid fighter name')
   patch=folder/'patch.json';atomic_json(patch,{'attributes':manifest['attributes'],'moves':manifest['moves']})
   # Apply the submitted fields to this package, preserving inherited/custom actions.
   source=folder/'files'/('Pl'+f['prefix']+'.dat');dest=source.with_suffix('.editing.dat')
   run_bridge('patch',source,patch,dest);dest.replace(source);write_manifest(folder,manifest);runtime_registry();return ({'saved':True})
  if path=='/api/stage/import':
   name=data.get('name','Imported stage').strip()
   if not name or len(name)>48 or any(ord(c)<32 for c in name):raise ValueError('Invalid stage name')
   id=re.sub('[^a-z0-9]+','-',name.lower()).strip('-')[:44]+'-'+uuid.uuid4().hex[:6];folder=MODS/id;inputs=folder/'source';inputs.mkdir(parents=True)
   for item in data['files']:
    dest=(inputs/item['name']).resolve()
    if not dest.is_relative_to(inputs.resolve()) or dest.suffix.lower() not in ['.glb','.gltf','.bin','.png','.jpg','.jpeg','.webp']:raise ValueError('Unsupported or unsafe import file')
    dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(base64.b64decode(item['data'],validate=True))
   primary=(inputs/data['primary']).resolve()
   if not primary.is_relative_to(inputs.resolve()) or primary.suffix.lower() not in ['.glb','.gltf']:raise ValueError('Choose a GLB or glTF scene')
   scene=load_gltf(primary)
   if not scene['metadata']:scene['metadata']={'schema':1,'platforms':[{'name':'Main floor','a':[-60,0],'b':[60,0],'dropThrough':False}],'spawns':[[-30,12],[30,12],[-10,12],[10,12]],'cameraBounds':[-160,160,-80,130],'blastBounds':[-220,220,-130,180]}
   atomic_json(folder/'scene.json',scene);export_stage(scene,folder/'stage.glb')
   manifest={'schema':1,'id':id,'name':name,'kind':'stage','enabled':False,'sourceFormat':primary.suffix[1:]};write_manifest(folder,manifest)
   return (manifest)
  if path=='/api/stage/save':
   folder=MODS/validate_id(data['id']);scene=json.loads((folder/'scene.json').read_text());validate_metadata(data['metadata']);scene['metadata']=data['metadata'];atomic_json(folder/'scene.json',scene)
   result=compile_stage(data['id']);manifest=read_manifest(folder);manifest['enabled']=True;write_manifest(folder,manifest);runtime_registry();return (result)
  if path=='/api/package/enabled':
   folder=MODS/validate_id(data['id']);manifest=read_manifest(folder);manifest['enabled']=bool(data['enabled']);write_manifest(folder,manifest);runtime_registry();return (manifest)
  raise ValueError('Unknown operation')


def texture_list(f,costume):
 if costume not in [c['file'] for c in f['costumes']]:raise ValueError('Unknown costume')
 rows=json.loads(cached(f,'images',costume).read_text())
 original=dict(f,folder=str(ASSETS),kind='fighter');original_costume=costume if f.get('kind')!='costume' else costume_reference(f['prefix'],asset_path(f,costume));source=json.loads(cached(original,'images',original_costume).read_text())
 return {'images':[{'id':im['id'],'width':im['width'],'height':im['height'],'format':im['format'],'changed':im['rgba']!=source[im['id']]['rgba'] if im['id']<len(source) else True} for im in rows if im.get('rgba')], 'icons':['css','stock']}


def texture_file(f,q):
 from PIL import Image
 import io
 costume=q['costume']
 if costume not in [c['file'] for c in f['costumes']]:raise ValueError('Unknown costume')
 if q.get('original')=='1':
  if f.get('kind')=='costume':costume=costume_reference(f['prefix'],asset_path(f,costume))
  f=dict(f,folder=str(ASSETS),kind='fighter')
 source=cached(f,'images',costume);out=CACHE/(source.stem+'-texture-'+str(int(q['image']))+'.png')
 if not out.exists():
  row=json.loads(source.read_text())[int(q['image'])]
  if not row.get('rgba'):raise ValueError('This texture needs an animated palette')
  Image.frombytes('RGBA',(row['width'],row['height']),base64.b64decode(row['rgba'])).save(out)
 return AssetFile(out,'texture-'+str(int(q['image']))+'.png')


def import_texture(data):
 from PIL import Image
 import io,struct
 f=get_fighter(data['id'])
 if f['stock']:raise ValueError('Clone a stock fighter before editing')
 raw=base64.b64decode(data['data'],validate=True)
 if len(raw)>16*1024*1024:raise ValueError('Image exceeds 16 MiB')
 with Image.open(io.BytesIO(raw)) as source:
  if source.width>2048 or source.height>2048:raise ValueError('Image dimensions exceed 2048')
  image=source.convert('RGBA')
 folder=MODS/f['id'];art=folder/'art';art.mkdir(exist_ok=True)
 kind=data.get('kind','texture')
 if f.get('kind')=='costume' and kind!='texture':raise ValueError('Edit the costume textures; shared stock and roster icons stay original')
 if kind in ('css','stock'):
  expected=(64,56) if kind=='css' else (24,24)
  if image.size!=expected:raise ValueError('Icon must be %d x %d pixels'%expected)
  stem='css-icon' if kind=='css' else 'stock-icon';image.save(art/(stem+'.png'))
  (art/(stem+'.rgba')).write_bytes(struct.pack('<II',*image.size)+image.tobytes())
  if kind=='stock':
   patch=art/(stem+'.json');atomic_json(patch,{'width':image.width,'height':image.height,'rgba':base64.b64encode(image.tobytes()).decode()})
   run_bridge('image-archive',patch,folder/'stock-icon.dat')
 else:
  costume=data['costume'];index=int(data['image'])
  if costume not in [c['file'] for c in f['costumes']]:raise ValueError('Unknown costume')
  rows=json.loads(cached(f,'images',costume).read_text())
  if index<0 or index>=len(rows):raise ValueError('Unknown texture')
  row=rows[index]
  if image.size!=(row['width'],row['height']):raise ValueError('Keep the original %d x %d texture size to preserve UV mapping'%(row['width'],row['height']))
  patches=[dict(id=r['id'],width=image.width,height=image.height,rgba=base64.b64encode(image.tobytes()).decode()) for r in rows if r.get('rgba')==row['rgba'] and r['width']==image.width and r['height']==image.height]
  patch=art/'texture-import.json';atomic_json(patch,patches);source=asset_path(f,costume);dest=source.with_suffix('.editing.dat')
  run_bridge('patch-image',source,patch,dest);dest.replace(source);image.save(art/(Path(costume).name+'-texture-'+str(index)+'.png'))
 runtime_registry();return {'saved':True}
