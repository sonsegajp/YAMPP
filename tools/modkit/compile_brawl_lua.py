"""Translate extracted PSA timelines to editable Lua, with explicit coverage output."""
import json,sys,collections,math
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def lua(v):
 if v is None:return 'nil'
 if isinstance(v,bool):return 'true' if v else 'false'
 if isinstance(v,str):return json.dumps(v,ensure_ascii=True)
 if isinstance(v,(int,float)):
  if not math.isfinite(v):raise ValueError('Non-finite Lua data')
  return repr(v)
 if isinstance(v,list):return '{'+','.join(lua(x) for x in v)+'}'
 return '{'+','.join('['+lua(k)+']='+lua(x) for k,x in v.items())+'}'

def compile_package(folder):
 folder=Path(folder);src=json.loads((ROOT/'data/brawl-diddy/moves-original.json').read_text());bones=json.loads((ROOT/'build/brawl/native-import/bone-map.json').read_text())['sourceToNative']
 scripts={}
 for s in src['subactions']:
  for sc in s['scripts'].values():scripts[sc['offset']]=sc['block']['events']
 for key in ['scripts_fragment_fighter','scripts_fragment_common']:
  for sc in src[key]:scripts[sc['offset']]=sc['block']['events']
 clips=json.loads((folder/'animations.json').read_text());mapping=json.loads((folder/'files/PlDk.dat.mapping.json').read_text());ids={m['id']:m['clip'] for m in mapping['mappings'] if m['scope']!='demo'}
 audio={s['id']:s['file'] for s in json.loads((folder/'audio.json').read_text())};needed=set();coverage={};moves={}
 effect={'Normal':0,'Fire':1,'Flame':1,'Electric':2,'Slash':3,'Coin':4,'Ice':5,'Sleep':6,'Bury':9,'Stun':12,'Flower':15}
 sounds={'Punch':1,'Kick':0,'Slash':2,'MagicZap':0,'Unique':0,'Burn':0,'Paper':2,'HomeRunBat':0}
 def hit(v):
  damage=v['damage'];damage=damage['Constant'] if isinstance(damage,dict) else damage
  return dict(id=v['hitbox_id'],group=v['set_id'],bone=bones[str(v['bone_index'])],damage=damage,angle=v['trajectory'],growth=v['kbg'],base=v['bkb'],weight=v['wdsk'],size=v['size'],x=v['x_offset'],y=v['y_offset'],z=v['z_offset'],element=effect[v['effect']],shield=v['shield_damage'],sound_level=v['sound_level'],sound_kind=sounds[v['sound']],clank=int(v['clang']),air=int(v['aerial']),ground=int(v['ground']))
 for sub in src['subactions']:
  name=sub['name']
  if name not in clips or name in moves:continue
  events=[];unsupported=collections.Counter();budget=[0];length=len(sub['frames'])
  def run(block,at=0,depth=0):
   if depth>40:unsupported['RecursiveScript']+=1;return at
   for e in block:
    budget[0]+=1
    if budget[0]>10000:unsupported['ScriptLoopLimit']+=1;return length+1
    key=e if isinstance(e,str) else next(iter(e));v=None if isinstance(e,str) else e[key]
    if key=='AsyncWait':at=max(at,v)
    elif key=='SyncWait':at+=v
    elif key in ('Subroutine','Goto'):
     if v['offset'] not in scripts:unsupported['MissingScript:'+str(v['offset'])]+=1
     else:at=run(scripts[v['offset']],at,depth+1)
     if key=='Goto':return at
    elif key=='ForLoop':
     count=v['iterations'];count=1000 if count=='Infinite' else (count.get('Finite',0) if isinstance(count,dict) else int(count))
     for _ in range(count):
      before=at;at=run(v['block']['events'],at,depth+1)
      if at>length:break
      if at==before and count==1000:unsupported['ZeroTimeLoop']+=1;break
    elif key=='Return':return at
    elif key in ('CreateHitBox','CreateSpecialHitBox'):
     args=v if key=='CreateHitBox' else v['hitbox_args']
     try:
      converted=hit(args)
      if converted['id']>3:raise ValueError('Melee supports four fighter hitboxes')
      events.append([at,'hitbox',converted])
     except (KeyError,TypeError,ValueError) as error:unsupported['UnsupportedHitbox:'+str(error)]+=1
     if key=='CreateSpecialHitBox':unsupported['SpecialHitboxFlags']+=1
    elif key=='DeleteAllHitBoxes':events.append([at,'clear'])
    elif key=='AllowInterrupts':events.append([at,'interrupt',True])
    elif key=='EnableInterrupt' and name=='Dash' and v==10009:events.append([at,'run_transition',True])
    elif key in ('BoolVariableSetTrue','BoolVariableSetFalse'):
     var=v['variable'].get('RandomAccessBool');value=key.endswith('True')
     if var=='EnableLandingLag':events.append([at,'landing',value])
     elif var=='EnableActionTransition':events.append([at,'transition',value])
     elif var=='EnableAutoJab':events.append([at,'auto_jab',value])
     else:unsupported[key+':'+str(var)]+=1
    elif key=='ChangeHurtBoxStateAll':events.append([at,'hurt',{'Normal':0,'Invincible':1,'Intangible':2,'IntangibleFlashing':2}[v['state']]])
    elif key=='ReverseDirection':events.append([at,'reverse'])
    elif key in ('SoundEffect1','SoundEffectOther1','SoundEffectOther2'):
     needed.add(v);events.append([at,'sound',v])
    elif key=='SoundVoiceLow':events.append([at,'voice'])
    elif key=='SoundEffectStop':events.append([at,'stop_sound',v])
    elif key=='FrameSpeedModifier':events.append([at,'speed',v['multiplier']])
    else:unsupported[key]+=1
    if at>length:return at
   return at
  run(sub['scripts']['script_main']['block']['events']);main_unsupported=dict(unsupported)
  run(sub['scripts']['script_sfx']['block']['events']);events.sort(key=lambda e:e[0])
  moves[name]=dict(length=length,iasa=sub['iasa'],events=events)
  coverage[name]=dict(mainUnsupported=main_unsupported,allUnsupported=dict(unsupported),events=len(events))
 scripts_dir=folder/'scripts';scripts_dir.mkdir(exist_ok=True)
 (scripts_dir/'moves.lua').write_text('-- Generated from the local Brawl PSA. Edit or regenerate from source.\nreturn '+lua(moves)+'\n')
 (scripts_dir/'assets.lua').write_text('return '+lua(dict(animations=ids,clips=clips,sounds=audio))+'\n')
 (folder/'lua-coverage.json').write_text(json.dumps(coverage,indent=2));(ROOT/'build/brawl/required-sounds.json').write_text(json.dumps(sorted(needed)))
 print(json.dumps({'timelines':len(moves),'completeMainTimelines':sum(not v['mainUnsupported'] for v in coverage.values()),'neededSounds':len(needed),'missingAudio':sorted(needed-audio.keys())}))
if __name__=='__main__':compile_package(sys.argv[1])
