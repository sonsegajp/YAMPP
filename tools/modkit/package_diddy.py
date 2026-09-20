"""Assemble the extracted Diddy assets into a Workshop package (local assets only)."""
import json,struct,shutil,subprocess,sys,re
from pathlib import Path
from xml_manifest import write_manifest
ROOT=Path(__file__).resolve().parents[2];SRC=ROOT/'data/brawl-diddy';BUILD=ROOT/'build/brawl/native-import'

def assemble(folder):
 folder=Path(folder).resolve();files=folder/'files';files.mkdir(parents=True,exist_ok=True)
 anims={p.stem:p for p in (BUILD/'animations').glob('*.dat')};bank={};blob=bytearray()
 for name,p in sorted(anims.items()):
  raw=p.read_bytes();assert len(raw)<0x20000
  meta=json.loads((BUILD/'animations-json'/f'{name}.json').read_text());bank[name]={'offset':len(blob),'size':len(raw),'loop':meta['loop']};blob+=raw;blob+=bytes((-len(blob))%32)
 assert len(blob)<16*1024*1024
 (files/'PlDkAJ.dat').write_bytes(blob);shutil.copy2(BUILD/'PlDkNr.dat',files/'PlDkNr.dat');shutil.copy2(BUILD/'parts-map.bin',files/'parts-map.bin')
 bones=json.loads((BUILD/'bone-map.json').read_text());remap=bones['sourceToNative'];source=json.loads((SRC/'moves-original.json').read_text());a=source['attributes']
 # Brawl's PSA owns looping for locomotion; the CHR0 header alone is false
 # for walking/running clips, which otherwise freeze and slide at their end.
 for sub in source['subactions']:
  if sub['name'] in bank and 'LOOP' in sub['animation_flags'].split(' | '):bank[sub['name']]['loop']=True
 names={'InitialWalkSpeed':'walk_init_vel','WalkAcceleration':'walk_acc','MaxWalkSpeed':'walk_max_vel','Friction':'ground_friction','InitialDashSpeed':'dash_init_vel','InitialRunSpeed':'dash_run_term_vel','JumpStartupLag':'jump_squat_frames','InitialHorizontalJumpVelocity':'jump_x_init_vel','InitialVerticalJumpVelocity':'jump_y_init_vel','GroundToAirJumpMomentumMultiplier':'jump_x_vel_ground_mult','MaximumShorthopHorizontalVelocity':'jump_x_init_term_vel','MaximumShorthopVerticalVelocity':'jump_y_init_vel_short','VerticalAirJumpMultiplier':'air_jump_y_mult','HorizontalAirJumpMultiplier':'air_jump_x_mult','NumberOfJumps':'num_jumps','Gravity':'gravity','TerminalVelocity':'term_vel','AerialSpeed':'air_mobility_a','MaxAerialHorizontalSpeed':'air_x_term_vel','AirFriction':'air_friction_x','FastFallTerminalVelocity':'fastfall_velocity','Jab2Window':'jab2_window','Jab3Window':'jab3_window','Weight':'weight','ModelScale':'size','ShieldSize':'shield_size','NormalLandingLag':'normal_landing_lag','NairLandingLag':'nair_landing_lag','FairLandingLag':'fair_landing_lag','BairLandingLag':'bair_landing_lag','UairLandingLag':'uair_landing_lag','DairLandingLag':'dair_landing_lag','WallJumpHorizontalVelocity':'walljump_x_vel','WallJumpVerticalVelocity':'walljump_y_vel'}
 attrs={key:a[val] for key,val in names.items()};attrs['Jab2Window']=int(attrs['Jab2Window']);attrs['Jab3Window']=int(attrs['Jab3Window']);attrs['CameraZoomTargetBone']=bones['byName']['BustN'];attrs['VictoryScreenModelScale']=18.0
 hurt=[]
 for h in json.loads((SRC/'moves-original.json.fighter.json').read_text())['hurtboxes']:
  if not h['enabled']:continue
  hurt.append(dict(BoneIndex=remap[str(h['bone_index'])],Type={'Middle':'Mid','High':'High','Low':'Low'}[h['zone']],Grabbable=int(h['grabbable']),Size=h['radius'],**{axis.upper()+str(i):h[key][axis] for i,key in [(1,'offset'),(2,'stretch')] for axis in 'xyz'}))
 aliases={'Wait':'Wait1','WaitItem':'Wait1','WaitItem2':'Wait1','Landing':'LandingLight','LandingFallSpecial':'LandingFallSpecial','AppealS':'AppealS','Appeal':'AppealHiR','Win1Wait':'Win1Wait','Win2Wait':'Win2Wait','Win3Wait':'Win3Wait','Lose':'Lose','RunDirect':'Run','LightThrowF4':'SmashThrowF','LightThrowB4':'SmashThrowB','LightThrowHi4':'SmashThrowHi','LightThrowLw4':'SmashThrowLw'}
 config=dict(actionOverrides={'48':'Attack13','49':'Attack100Start','50':'Attack100','51':'AttackEnd'},animations=bank,aliases=aliases,bones=bones['byName'],hurtboxes=hurt,attributes=attrs,costume=str(files/'PlDkNr.dat'))
 path=BUILD/'fighter-config.json';path.write_text(json.dumps(config));subprocess.run([str(ROOT/'build/modkit/bridge/ModBridge.exe'),'compile-fighter',str(ROOT/'data/GALE01/files/PlDk.dat'),str(path),str(files/'PlDk.dat')],check=True)
 mapping=json.loads((files/'PlDk.dat.mapping.json').read_text());(files/'fighter-runtime.bin').write_bytes(b'MF01'+struct.pack('>III',mapping['actions'],max(v['size'] for v in bank.values()),mapping['demoActions']))
 sound=folder/'audio';sound.mkdir(exist_ok=True);inventory=json.loads((SRC/'audio-original/inventory.json').read_text());wav={p.stem:p for p in (SRC/'audio-original').rglob('*.wav')};sounds=[]
 for entry in inventory:
  path=wav.get(re.sub(r'[<>:"/\\|?*]','_',entry['path']))
  if path is None or entry.get('soundId',-1)<0:continue
  dest=sound/f"{entry['soundId']:05d}.wav";shutil.copy2(path,dest);sounds.append(dict(id=entry['soundId'],name=entry['path'],file=dest.relative_to(folder).as_posix()))
 (folder/'audio.json').write_text(json.dumps(sounds,indent=2));scripts=folder/'scripts';scripts.mkdir(exist_ok=True)
 # Asset check only. Full gameplay code is authored separately, never hidden in
 # native data edits or silently substituted with another character's attacks.
 if not (scripts/'fighter.lua').exists():
  voice=next((s for s in sounds if '/vc/diddy/' in s['name'].lower()),None)
  (scripts/'fighter.lua').write_text('local f = { api_version = 1 }\nfunction f.on_spawn(self)\n self:log("Diddy assets loaded; behavior under development")\n'+(' self:sound("'+voice['file']+'")\n' if voice else '')+'end\nfunction f.on_frame(self) end\nreturn f\n')
 clips={m['clip']:m['id'] for m in mapping['mappings'] if m['scope']=='lua'};(folder/'animations.json').write_text(json.dumps(clips,indent=2))
 write_manifest(folder,dict(kind='fighter',id=folder.name,name='Diddy Kong',prefix='Dk',external=1,internal=3,base='stock-Dk',enabled=True,defaultCostume=0,lua={'api':1,'entry':'scripts/fighter.lua'},source={'game':'RSBE01','fighter':'diddy','manifest':str(SRC/'source-manifest.xml')},development={'status':'asset-validation','unmappedBaseAnimations':len(mapping['fallbacks'])}))
 print(json.dumps({'package':str(folder),'animations':len(clips),'audio':len(sounds),'fallbacks':len(mapping['fallbacks']),'runtimeAnimationBuffer':max(v['size'] for v in bank.values())}))
if __name__=='__main__':assemble(sys.argv[1])
