"""Create Melee-compatible skinned assets from the extracted Brawl fighter."""
import copy,json,sys,warnings
from pathlib import Path
import numpy as np
from scipy.spatial.transform import Rotation
from brawl_interchange import import_animation
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'data/brawl-diddy'
OUT=ROOT/'build/brawl/native-import'

def prepare():
 OUT.mkdir(parents=True,exist_ok=True);anims=OUT/'animations-json';anims.mkdir(exist_ok=True)
 model=json.loads((SOURCE/'blender/diddy-melee.model.json').read_text());original=model['bones'];order=[]
 def visit(i):
  order.append(i)
  children=[b['id'] for b in original if b['parent']==i]
  if i==0:children.sort(key=lambda j:original[j]['name']!='TransN')
  for j in children:visit(j)
 visit(0);remap={old:new for new,old in enumerate(order)}
 bones=[];world=[]
 for b in original:
  t=np.eye(4);t[:3,:3]=Rotation.from_euler('xyz',b['rotation']).as_matrix()@np.diag(b['scale']);t[:3,3]=b['translation'];world.append(world[b['parent']]@t if b['parent']>=0 else t)
 for old in order:
  b=copy.deepcopy(original[old]);b.update(id=remap[old],parent=remap.get(b['parent'],-1),inverseBind=np.linalg.inv(world[old])[:3].ravel().tolist());bones.append(b)
 while len(bones)<75:bones.append(dict(id=len(bones),name='MeleeReserved'+str(len(bones)),parent=0,translation=[0,0,0],rotation=[0,0,0],scale=[1,1,1],inverseBind=np.eye(4)[:3].ravel().tolist()))
 assert len(bones)==75 and [b['name'] for b in bones[:5]]==['TopN','TransN','XRotN','YRotN','HipN']
 model['bones']=bones;model['symbol']='PlyDiddy5K_Share_joint';model['meshes']=[m for m in model['meshes'] if m['defaultVisible']]
 # HSD treats single-weight envelopes as positions in bone space; multi-weight
 # positions remain in bind/world space and use each joint's inverse bind matrix.
 for m in model['meshes']:
  for i,(js,ws) in enumerate(zip(m['skin'],m['weights'])):
   active=[(j,w) for j,w in zip(js,ws) if w>0]
   if len(active)==1:
    old=active[0][0];m['positions'][i]=(np.linalg.inv(world[old])@np.r_[m['positions'][i],1])[:3].tolist();n=world[old][:3,:3].T@np.array(m['normals'][i]);m['normals'][i]=(n/max(np.linalg.norm(n),1e-8)).tolist();m['rigid'][i]=True
   m['skin'][i]=[remap[j] for j in js]
 (OUT/'model.json').write_text(json.dumps(model,separators=(',',':')))
 part_names='TopN TransN XRotN YRotN HipN WaistN LLegJ LLegJ LKneeJ LFootJ LFootJ RLegJ RLegJ RKneeJ RFootJ RFootJ WaistN BustN LShoulderN LShoulderJ LShoulderJ LArmJ LHandN L1stNa L1stNb L2ndNa L2ndNb L3rdNa L3rdNb L4thNa L4thNb LHaveN LThumbNa LThumbNb NeckN HeadN RShoulderN RShoulderJ RShoulderJ RArmJ RHandN R1stNa R1stNb R2ndNa R2ndNb R3rdNa R3rdNb R4thNa R4thNb RHaveN RThumbNa RThumbNb ThrowN ThrowN'.split()
 name_ids={b['name']:b['id'] for b in bones};parts=[name_ids[n] for n in part_names];assert len(parts)==54
 (OUT/'parts-map.bin').write_bytes(b'MP01'+bytes(parts));(OUT/'bone-map.json').write_text(json.dumps({'sourceToNative':remap,'byName':name_ids,'parts':parts},indent=2))
 paths=list((SOURCE/'export/FitDiddyMotionEtc').rglob('*.animation.json'))
 for i,p in enumerate(paths):
  a=import_animation(p,{'bones':original});a['nodes']=[a['nodes'][old] for old in order]+[[] for _ in range(75-len(order))]
  # A track omitted by CHR0 inherits the bind pose. Melee's transition blending
  # requires explicit bind tracks so a previous animation cannot leak its pose.
  for b,tracks in zip(bones,a['nodes']):
   present={t['type'] for t in tracks}
   for key,ids in [('rotation',[1,2,3]),('translation',[5,6,7]),('scale',[8,9,10])]:
    for axis,kind in enumerate(ids):
     if kind not in present:tracks.append({'type':kind,'values':[b[key][axis]]})
  # Melee owns TopN world position, scale and facing. Brawl's bind-pose
  # defaults must not reset the engine's root rotation during playback.
  a['nodes'][0]=[]
  # Melee's walk/run physics supplies travel; these common states do not
  # consume TransN root motion. Preserve the limb pose, but remove Brawl's
  # accumulating root translation so the model stays on its collision body.
  if p.name.split('.')[0] in ('WalkSlow','WalkMiddle','WalkFast','Run'):
   for track in a['nodes'][name_ids['TransN']]:
    if track['type'] in (5,6,7):
     axis=track['type'];track.clear();track.update(type=axis,values=[0.0])
  (anims/p.name.replace('.animation.json','.json')).write_text(json.dumps(a,separators=(',',':')))
 print(json.dumps({'nativeBones':len(bones),'meshes':len(model['meshes']),'animations':len(paths),'output':str(OUT)}))
if __name__=='__main__':prepare()
