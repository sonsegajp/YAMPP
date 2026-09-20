"""Model visibility from fighter DAT tables; defaults from ft*_OnDeath in the decomp."""
import re

# HSDLib's decompiler labels opcode 5 (call) Goto and opcode 7 (jump) Subroutine.
# Execute blocks rather than scanning their textual order, retaining bounded loops.
def script_events(script):
 blocks={name:re.findall(r'(\w+)\(([^()]*)\);',body) for name,body in re.findall(r'(\w+)\s*\{([^{}]*)\}',script)}
 if not blocks:return []
 block=next(iter(blocks));pc=0;frame=0;stack=[];loops=[];result=[]
 for step in range(20000):
  commands=blocks.get(block,[])
  if pc>=len(commands):break
  command,raw=commands[pc];pc+=1
  params={k.strip():int(v.strip(),0) for k,v in (item.split('=') for item in raw.split(',') if '=' in item)}
  if command=='Goto':
   if raw not in blocks or len(stack)>32:break
   stack.append((block,pc));block=raw;pc=0
  elif command in ('Subroutine','GoTo'):
   if raw not in blocks:break
   block=raw;pc=0
  elif command=='Return':
   if not stack:break
   block,pc=stack.pop()
  elif command=='SetLoop':loops.append([block,pc,max(1,min(params.get('Count',1),1000))])
  elif command=='ExecuteLoop':
   if loops:
    loops[-1][2]-=1
    if loops[-1][2]>0:block,pc=loops[-1][:2]
    else:loops.pop()
  elif command=='AsynchronousTimer':frame=max(frame,params.get('Frame',frame))
  elif command=='SynchronousTimer':frame+=params.get('Frame',0)
  else:result.append({'frame':frame,'command':command,'parameters':params})
  if frame>10000:break
 return result

def defaults(prefix,count,costume_index):
 states=[0]*count
 overrides={'Fe':{2:-1},'Sk':{1:-1},'Gn':{1:-1},'Gw':{1:-1}}
 for group,value in overrides.get(prefix,{}).items():
  if group<count:states[group]=value
 if prefix=='Pc':states=([0]+[0 if i==costume_index else -1 for i in range(1,4)])[:count]
 if prefix=='Pe':states=([0,-1,0,-1,0,0,-1] if costume_index==1 else [0,0,0,-1,0,-1,0])[:count]
 return states

def describe(fighter,inspected,costume):
 raw=inspected.get('modelVisibility') or {'groupCount':0,'costumes':[]}
 index=next(c.get('index',0) for c in fighter['costumes'] if c['file']==costume)
 costumes=raw['costumes'];entry=costumes[index] if index<len(costumes) else {};base=costumes[0] if costumes else {}
 high=entry.get('high') or base.get('high') or [];low=entry.get('low') or base.get('low') or []
 states=defaults(fighter['prefix'],len(high),index);groups=[]
 for group,variants in enumerate(high):
  values=[]
  for value,dobjs in enumerate(variants):
   label='Group '+str(group+1)+' / model '+str(value+1)
   if fighter['prefix']=='Ss' and group==0:label={0:'Main body',1:'Morph ball transition',2:'Morph ball'}.get(value,label)
   if fighter['prefix']=='Kb' and group==0:label={0:'Main body',1:'Inhale body',2:'Stone form 1',3:'Stone form 2',4:'Stone form 3',5:'Stone form 4',6:'Stone form 5'}.get(value,label)
   if value==states[group] and label.startswith('Group '):label='Main model part '+str(group+1)
   uses=[m['id'] for m in inspected.get('moves',[]) if any(e['command']=='ChangleModelState' and e['parameters'].get('StructID')==group and e['parameters'].get('ObjectID')==value for e in m.get('events',[]))]
   if fighter['prefix']=='Kb' and group==0 and value>=2:uses=[332,333,334,335,336,337]
   values.append({'id':value,'label':label,'dobjs':dobjs,'moves':uses})
  groups.append({'id':group,'default':states[group],'variants':values})
 return {'schema':1,'prefix':fighter['prefix'],'groups':groups,'defaults':states,'high':high,'low':low,'source':'fighter DAT visibility tables'}

def visible_dobjs(parts,all_ids,selection='base'):
 high=parts['high'];low=parts['low'];states=list(parts['defaults'])
 controlled={i for table in (high,low) for group in table for variant in group for i in variant}
 if selection.startswith('part:'):
  _,group,value=selection.split(':');group=int(group);value=int(value)
  if group<0 or group>=len(high) or value<0 or value>=len(high[group]):raise ValueError('Unknown submodel')
  return set(high[group][value])
 if selection.startswith('states:'):
  states=[int(x) for x in selection[7:].split(',')] if selection[7:] else []
  if len(states)!=len(high) or any(v < -1 or v>=len(high[i]) for i,v in enumerate(states)):raise ValueError('Invalid model state')
 elif selection not in ('base','all'):raise ValueError('Unknown model selection')
 if selection=='all':return set(all_ids)
 active=set(all_ids)-controlled
 for group,value in enumerate(states):
  if 0<=value<len(high[group]):active.update(high[group][value])
 return active

def annotate(model,parts):
 active=visible_dobjs(parts,[m['dobj'] for m in model['meshes']]);high={i for group in parts['high'] for variant in group for i in variant}
 for mesh in model['meshes']:
  obj=mesh['dobj'];mesh['defaultVisible']=obj in active and not mesh['hidden']
  mesh['category']='main' if mesh['defaultVisible'] else 'submodel' if obj in high else 'detail'
  mesh['parts']=[{'group':g['id'],'variant':v['id'],'label':v['label']} for g in parts['groups'] for v in g['variants'] if obj in v['dobjs']]
 model['modelParts']=parts
 return model
