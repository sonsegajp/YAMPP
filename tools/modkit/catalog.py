from pathlib import Path
import json,re,os,sys,hashlib
from xml_manifest import read_manifest,write_manifest,package_folders
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
from project_config import configuration,project_path
CONFIG=configuration()
ASSETS=project_path(CONFIG['assets']['directory'])/'files'
MODS=project_path(CONFIG['user']['mods'])
# Desktop checks use a build-local sandbox; installed user packages stay separate.
if os.environ.get('MELEE_WORKSHOP_TEST_MODS'):
 test_mods=Path(os.environ['MELEE_WORKSHOP_TEST_MODS']).resolve()
 if not test_mods.is_relative_to((ROOT/'build/modkit').resolve()):raise ValueError('Invalid workshop test directory')
 MODS=test_mods
CACHE=ROOT/'build/modkit/cache'
NAMES=[('Captain Falcon','Ca',2),('Donkey Kong','Dk',3),('Fox','Fx',1),('Mr. Game & Watch','Gw',24),('Kirby','Kb',4),('Bowser','Kp',5),('Link','Lk',6),('Luigi','Lg',17),('Mario','Mr',0),('Marth','Ms',18),('Mewtwo','Mt',16),('Ness','Ns',8),('Peach','Pe',9),('Pikachu','Pk',12),('Ice Climbers','Pp',10),('Jigglypuff','Pr',15),('Samus','Ss',13),('Yoshi','Ys',14),('Zelda','Zd',19),('Sheik','Sk',7),('Falco','Fc',22),('Young Link','Cl',20),('Dr. Mario','Dr',21),('Roy','Fe',26),('Pichu','Pc',23),('Ganondorf','Gn',25)]
COLORS={'Nr':'Default','Re':'Red','Bu':'Blue','Gr':'Green','Wh':'White','Gy':'Black','Bk':'Black','Ye':'Yellow','Pi':'Pink','Or':'Orange','Aq':'Aqua','La':'Lavender','Vi':'Violet'}
COSTUME_ORDER=json.loads((Path(__file__).parent/'costume_order.json').read_text())

def costumes(prefix,folder=ASSETS):
 order=COSTUME_ORDER.get(prefix,[])
 files=sorted(folder.glob('Pl'+prefix+'??.dat'),key=lambda p:(order.index(p.name) if p.name in order else 100,p.name))
 return [{'file':p.name,'index':order.index(p.name) if p.name in order else 0,'name':COLORS.get(p.stem[-2:],p.stem[-2:]),'bytes':p.stat().st_size} for p in files if p.stem[-2:]!='AJ']
def fighters():
 result=[{'id':'stock-'+prefix,'name':name,'prefix':prefix,'external':e,'internal':i,'kind':'fighter','stock':True,'folder':str(ASSETS),'costumes':costumes(prefix)} for e,(name,prefix,i) in enumerate(NAMES)]
 for folder in package_folders(MODS):
  m=read_manifest(folder);
  if m['kind']=='fighter':result.append(dict(m,stock=False,folder=str(folder/'files'),costumes=costumes(m['prefix'],folder/'files')))
 return result

def stages():
 return [dict(m,stock=False) for folder in package_folders(MODS) if (m:=read_manifest(folder)).get('kind')=='stage']
def find_fighter(id):
 return next((f for f in fighters() if f['id']==id),None)
def validate_id(id):
 if not re.fullmatch('[a-z0-9][a-z0-9-]{0,63}',id):raise ValueError('Invalid package ID')
 return id

def costume_reference(prefix,path):
 import struct
 with Path(path).open('rb') as stream:
  header=stream.read(32)
  if len(header)!=32:raise ValueError('Invalid costume DAT')
  _,size,relocs,roots,refs=struct.unpack_from('>5I',header)
  if roots>16 or refs>16:raise ValueError('Invalid costume roots')
  table=32+size+relocs*4;strings=table+(roots+refs)*8
  stream.seek(table);entries=stream.read(roots*8)
  if len(entries)!=roots*8:raise ValueError('Truncated costume root table')
  for index in range(roots):
   _,offset=struct.unpack_from('>2I',entries,index*8);stream.seek(strings+offset)
   symbol=stream.read(128).split(b'\0',1)[0].decode('ascii',errors='ignore')
   match=re.fullmatch(r'Ply[A-Za-z]+5K([A-Z][a-z])?_Share_joint',symbol)
   if match:
    candidate='Pl'+prefix+(match.group(1) or 'Nr')+'.dat'
    if candidate in COSTUME_ORDER.get(prefix,[]):return candidate
 raise ValueError('Costume model has no recognized original color root')


def costume_packages():
 # Costume packages never enter the legacy F registry or add fighter entries.
 result=[];seen=set()
 for folder in package_folders(MODS):
  manifest=read_manifest(folder)
  if manifest.get('kind')!='costume':continue
  ident=validate_id(manifest.get('id',''))
  if ident in seen or ident!=folder.name:raise ValueError('Duplicate or inconsistent costume package ID')
  seen.add(ident)
  if manifest.get('additive') is not True:raise ValueError('Costume packages must add colors')
  if not isinstance(manifest.get('enabled',True),bool):raise ValueError('Invalid costume enabled state')
  if not isinstance(manifest.get('costumes'),list) or not 1<=len(manifest['costumes'])<=16:raise ValueError('A costume pack must contain 1 to 16 colors')
  result.append(dict(manifest,folder=str(folder)))
 return result


def costume_slots(room_packages=()):
 # Import lazily: community itself imports catalog for install destinations.
 from community import verified_install,safe_source,validator
 validator()
 from mod_repository import validate_costume_dat,validate_native_texture
 bases={'stock-'+prefix:(external,internal,prefix) for external,(_,prefix,internal) in enumerate(NAMES)}
 packages=[]
 for pack in costume_packages():
  if pack.get('base') not in bases:raise ValueError('A costume pack must use an original fighter')
  verified=verified_install(pack['folder'])
  packages.append((verified['sha256'] if verified else '-',pack,verified))
 known={sha for sha,_,verified in packages if verified}
 for pack,verified in room_packages:
  if pack.get('base') not in bases or pack.get('enabled') is not False:raise ValueError('Invalid room-only costume package')
  if verified['sha256'] not in known:
   packages.append((verified['sha256'],pack,verified));known.add(verified['sha256'])
 result=[];active={prefix:len(COSTUME_ORDER[prefix]) for _,prefix,_ in NAMES}
 for sha,pack,verified in sorted(packages,key=lambda row:(row[0],row[1]['id'])):
  external,internal,prefix=bases[pack['base']];slotids=set()
  for index,costume in enumerate(pack['costumes']):
   slotid=costume.get('id','');name=costume.get('name','')
   if not re.fullmatch('[a-z0-9][a-z0-9_-]{0,63}',slotid) or slotid in slotids:raise ValueError('Invalid or duplicate costume slot ID')
   slotids.add(slotid)
   if not isinstance(name,str) or not 1<=len(name)<=64 or any(ord(c)<32 for c in name):raise ValueError('Invalid costume name')
   path=safe_source(Path(pack['folder']),costume['file'])
   if path.suffix.lower()!='.dat':raise ValueError('Costume slots must contain DAT models')
   payload=path.read_bytes();validate_costume_dat(payload,pack['base'])
   payloadhash=verified['files'][costume['file']] if verified else hashlib.sha256(payload).hexdigest()
   reference=costume_reference(prefix,path);basecolor=COSTUME_ORDER[prefix].index(reference)
   art={}
   for field in ('portraitTexture','stockIconTexture'):
    if costume.get(field):
     imagepath=safe_source(Path(pack['folder']),costume[field]);imagebytes=imagepath.read_bytes();validate_native_texture(imagebytes,field)
     art[field]=str(imagepath.resolve());art[field+'Sha256']=verified['files'][costume[field]] if verified else hashlib.sha256(imagebytes).hexdigest()
    else:art[field]='-';art[field+'Sha256']='-'
   result.append({'packid':pack['id'],'sha256':sha,'enabled':pack.get('enabled',True),'external':external,'internal':internal,'prefix':prefix,'slotid':slotid,'name':name,'path':str(path.resolve()),'baseColor':basecolor,'order':index,'datSha256':payloadhash,**art})
   if pack.get('enabled',True):active[prefix]+=1
 if any(count>64 for count in active.values()):raise ValueError('A fighter exceeds 64 combined original and additional costumes')
 return result


def costume_registry(room_packages=(), *, output=None):
 import uuid
 lines=['MELEE_COSTUMES\t3']
 for row in costume_slots(room_packages):
  fields=['C',row['packid'],row['sha256'],'1' if row['enabled'] else '0',str(row['external']),str(row['internal']),row['prefix'],row['slotid'],row['name'],row['path'],str(row['baseColor']),row['datSha256'],row['portraitTexture'],row['portraitTextureSha256'],row['stockIconTexture'],row['stockIconTextureSha256']]
  if any('\t' in value or '\r' in value or '\n' in value for value in fields):raise ValueError('Unsafe costume registry field')
  lines.append('\t'.join(fields))
 path=Path(output or os.environ.get('MELEE_COSTUME_REGISTRY') or MODS/'costumes.tsv')
 path.parent.mkdir(parents=True,exist_ok=True);tmp=path.parent/(path.stem+'-'+uuid.uuid4().hex+'.tmp')
 tmp.write_text('\n'.join(lines)+'\n',encoding='utf-8',newline='\n');tmp.replace(path)
 return path


def roster_icons():
 import struct,subprocess,base64
 target=MODS/'catalog';target.mkdir(parents=True,exist_ok=True)
 # Versioned source archive mappings: imported from this pinned Melee release.
 import xml.etree.ElementTree as ET
 mappings=ET.parse(ROOT/'config/character-assets.xml').getroot()
 missing=[e for e in mappings if not (target/(e.get('prefix')+'.rgba')).exists()]
 if missing:
  extracted=CACHE/'roster-icons.json';CACHE.mkdir(parents=True,exist_ok=True)
  subprocess.run([str(ROOT/'build/modkit/bridge/ModBridge.exe'),'images',str(ASSETS/mappings.get('source')),str(extracted)],check=True,capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
  images=json.loads(extracted.read_text())
  for e in missing:
   image=images[int(e.get('image'))];pixels=base64.b64decode(image['rgba'])
   (target/(e.get('prefix')+'.rgba')).write_bytes(struct.pack('<II',image['width'],image['height'])+pixels)
 return target

def runtime_registry():
 costume_registry()
 # Native parser uses bounded tab-separated records. IDs remain stable as UI grids grow.
 icons=roster_icons()
 lines=['MELEE_MODS\t3']
 for f in fighters():
  if not f['stock'] and f.get('enabled',True):
   import hashlib,subprocess
   folder=MODS/f['id'];icon=folder/'art/css-icon.rgba'
   if not icon.exists():icon=icons/(f['prefix']+'.rgba')
   source=ASSETS/'MnSlChr.usd';digest=hashlib.sha256(b'vanilla-css-v1'+source.read_bytes()+icon.read_bytes()).hexdigest()
   stamp=folder/'css-portrait.sha256';portrait=folder/'css-portrait.dat'
   if not portrait.exists() or not stamp.exists() or stamp.read_text()!=digest:
    subprocess.run([str(ROOT/'build/modkit/bridge/ModBridge.exe'),'css-portrait',str(source),str(icon),str(portrait)],check=True,capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
    stamp.write_text(digest)
  if f.get('enabled',True):lines.append('\t'.join(['F',f['id'],f['name'],str(f['external']),str(f['internal']),f['prefix'],'-' if f['stock'] else (MODS/f['id']/'files').relative_to(ROOT).as_posix(),str(f.get('defaultCostume',0)),str(icons/(f['prefix']+'.rgba')) if f['stock'] or not (MODS/f['id']/'art/css-icon.rgba').exists() else str(MODS/f['id']/'art/css-icon.rgba'),','.join(c['file'] for c in f['costumes']),'-' if not f.get('lua') else (MODS/f['id']/f['lua']['entry']).relative_to(ROOT).as_posix()]))
 for s in stages():
  if s.get('enabled',True):lines.append('\t'.join(['S',s['id'],s['name'],'32','0','-',(MODS/s['id']/'stage.dat').relative_to(ROOT).as_posix(),'0','-','-','-']))
 MODS.mkdir(parents=True,exist_ok=True);tmp=MODS/'registry.tsv.tmp';tmp.write_text('\n'.join(lines)+'\n',encoding='utf-8');tmp.replace(MODS/'registry.tsv')
