// Fighter DAT visibility, matching ftParts_80074B6C / ftParts_80074D7C.
export function modelStates(parts, move, frame) {
 let states=[...(parts?.defaults||[])];
 for(const event of move?.events||[]) {
  if(event.frame>frame)continue;
  if(event.command==='RevertModels')states=[...parts.defaults];
  if(event.command==='RemoveModels')states.fill(-1);
  if(event.command==='ChangleModelState') {
   const {StructID, ObjectID}=event.parameters;
   if(StructID<states.length)states[StructID]=ObjectID===255?-1:ObjectID;
  }
 }
 // Stone's held states are selected by ftKb_SpecialHi_800F331C, not its movescript.
 // Preview the first Stone variant deterministically; all five can be inspected separately.
 if(parts?.prefix==='Kb' && [333,336].includes(move?.id))states=[2,-1];
 return states;
}
export function visibleModels(parts, ids, selection) {
 if(!parts)return new Set(ids);
 if(selection.startsWith('part:')) {const [,g,v]=selection.split(':').map(Number);return new Set(parts.high[g]?.[v]||[]);}
 const states=selection==='base'?parts.defaults:selection.slice(7).split(',').filter(x=>x!=='').map(Number);
 const controlled=new Set([...(parts.high||[]),...(parts.low||[])].flat(2));
 const active=new Set(ids.filter(id=>!controlled.has(id)));
 for(let g=0;g<states.length;g++)for(const id of parts.high[g]?.[states[g]]||[])active.add(id);
 return active;
}
