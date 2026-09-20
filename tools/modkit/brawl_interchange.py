"""Import BrawlCrate's skinned Collada + sampled CHR0 into Workshop/glTF."""
import base64,json,math,xml.etree.ElementTree as ET
from pathlib import Path
import numpy as np
from PIL import Image
from scipy.spatial.transform import Rotation
from interchange import export_model

def import_dae(path, texture_root=None, visibility=None):
    path=Path(path);root=ET.parse(path).getroot();ns={'c':root.tag[1:root.tag.index('}')]}
    def find(n,s):return n.find(s,ns)
    def all(n,s):return n.findall(s,ns)
    def nums(n,s,default=()):
        e=find(n,s);return [float(v) for v in e.text.split()] if e is not None and e.text else list(default)
    bones=[];bone_ids={};instances=[]
    def node(n,parent=-1):
        is_bone=n.get('type')=='JOINT';current=parent
        if is_bone:
            m=np.eye(4)
            for e in n:
                tag=e.tag.split('}')[-1];v=np.fromstring(e.text or '',sep=' ');x=np.eye(4)
                if tag=='translate':x[:3,3]=v
                elif tag=='scale':x[:3,:3]=np.diag(v)
                elif tag=='rotate':x[:3,:3]=Rotation.from_rotvec(v[:3]*math.radians(v[3])).as_matrix()
                elif tag=='matrix':x=v.reshape(4,4)
                else:continue
                m=m@x
            scale=np.linalg.norm(m[:3,:3],axis=0);rot=Rotation.from_matrix(m[:3,:3]/scale).as_euler('xyz')
            current=len(bones);bone_ids[n.get('id')]=current
            bones.append(dict(id=current,name=n.get('name'),parent=parent,translation=m[:3,3].tolist(),rotation=rot.tolist(),scale=scale.tolist(),flags=0))
        for i in all(n,'c:instance_controller')+all(n,'c:instance_geometry'):
            instances.append((i,current))
        for c in all(n,'c:node'):node(c,current)
    for n in all(root,'c:library_visual_scenes/c:visual_scene/c:node'):node(n)
    textures={p.stem:p for p in Path(texture_root or path.parent.parent).rglob('*.png')}
    effects={}
    for e in all(root,'c:library_effects/c:effect'):
        t=find(e,'.//c:diffuse/c:texture');texture=None
        if t is not None:
            sampler=find(e,".//c:newparam[@sid='"+t.get('texture')+"']")
            image=find(sampler,'.//c:instance_image') if sampler is not None else None
            texname=image.get('url').lstrip('#').removesuffix('-image') if image is not None else t.get('texture').removesuffix('-sampler')
            if texname in textures:
                im=Image.open(textures[texname]).convert('RGBA');texture=dict(name=texname,width=im.width,height=im.height,rgba=base64.b64encode(im.tobytes()).decode())
        effects[e.get('id')]=texture
    materials={m.get('id'):effects[find(m,'c:instance_effect').get('url').lstrip('#')] for m in all(root,'c:library_materials/c:material')}
    def sources(n):
        out={}
        for s in all(n,'c:source'):
            a=find(s,'c:float_array');access=find(s,'c:technique_common/c:accessor');stride=int(access.get('stride','1'))
            if a is not None:out[s.get('id')]=np.fromstring(a.text or '',sep=' ').reshape(-1,stride)
            else:out[s.get('id')]=(find(s,'c:Name_array').text or '').split()
        return out
    geometries={e.get('id'):e for e in all(root,'c:library_geometries/c:geometry')}
    controllers={e.get('id'):e for e in all(root,'c:library_controllers/c:controller')}
    meshes=[]
    for instance,parent in instances:
        ref=instance.get('url').lstrip('#');skin=None
        if ref in controllers:
            skin=find(controllers[ref],'c:skin');ref=skin.get('source').lstrip('#')
        if ref not in geometries:raise ValueError('Missing geometry '+ref)
        geom=find(geometries[ref],'c:mesh');src=sources(geom)
        verts={v.get('id'):{i.get('semantic'):i.get('source').lstrip('#') for i in all(v,'c:input')} for v in all(geom,'c:vertices')}
        weights=[]
        if skin is not None:
            ss=sources(skin);vw=find(skin,'c:vertex_weights');inputs={e.get('semantic'):(int(e.get('offset')),e.get('source').lstrip('#')) for e in all(vw,'c:input')}
            count=max(i[0] for i in inputs.values())+1;raw=np.array(nums(vw,'c:v'),int).reshape(-1,count);at=0
            ji,js=inputs['JOINT'];wi,ws=inputs['WEIGHT']
            for n in map(int,nums(vw,'c:vcount')):
                row=sorted([(bone_ids[ss[js][a[ji]]],float(ss[ws][a[wi]][0])) for a in raw[at:at+n]],key=lambda a:-a[1]);at+=n
                if len(row)>4 and sum(w for _,w in row[4:])>1e-4:raise ValueError('More than four bone weights')
                row=row[:4];row+=[(0,0)]*(4-len(row));weights.append(row)
        bindings={b.get('symbol'):b.get('target').lstrip('#') for b in all(instance,'.//c:instance_material')}
        for tri in all(geom,'c:triangles'):
            inputs={e.get('semantic'):(int(e.get('offset')),e.get('source').lstrip('#')) for e in all(tri,'c:input') if e.get('semantic')!='TEXCOORD' or e.get('set','0')=='0'}
            stride=max(int(e.get('offset')) for e in all(tri,'c:input'))+1
            faces=np.asarray(nums(tri,'c:p'),int).reshape(-1,stride)
            pos=[];norm=[];uv=[];joints=[];influence=[];uv_sets={}
            uv_inputs=[(int(e.get("set","0")),int(e.get("offset")),e.get("source").lstrip("#")) for e in all(tri,"c:input") if e.get("semantic")=="TEXCOORD"]
            for f in faces:
                pi,pn=inputs['VERTEX'];pindex=f[pi];pos.append(src[verts[pn]['POSITION']][pindex].tolist())
                ni,nn=inputs.get('NORMAL',(None,None));norm.append(src[nn][f[ni]].tolist() if nn else [0,1,0])
                for channel,offset,uvsource in uv_inputs:
                    coord=src[uvsource][f[offset]].tolist();coord[1]=1-coord[1];uv_sets.setdefault(channel,[]).append(coord)
                ui,un=inputs.get('TEXCOORD',(None,None));u=src[un][f[ui]].tolist() if un else [0,0];u[1]=1-u[1];uv.append(u)
                w=weights[pindex] if weights else [(max(parent,0),1),(0,0),(0,0),(0,0)]
                joints.append([b for b,_ in w]);influence.append([v for _,v in w])
            name=geometries[ref].get('name',ref);mat=bindings.get(tri.get('material'),tri.get('material'))
            meshes.append(dict(name=name,joint=max(parent,0),dobj=len(meshes),positions=pos,normals=norm,texcoords=uv,uvSets=uv_sets,skin=joints,weights=influence,rigid=[False]*len(pos),indices=list(range(len(pos))),texture=materials.get(mat),color=[1,1,1,1],hidden=False,defaultVisible=True))
    if not bones or not meshes:raise ValueError('No skinned model geometry')
    model=dict(name=path.stem,bones=bones,meshes=meshes)
    metadata=path.with_suffix('.mesh-metadata.json')
    if metadata.exists():
        meta={m['name']:m for m in json.loads(metadata.read_text())}
        for m in meshes:
            draws=meta[m['name']]['draws'];m['visibilityBone']=draws[0]['visibilityBone'] if draws else None
            if draws:
                state=draws[0].get('state',{});m['materialState']=state
                if len(state.get('textures',[]))>1:
                    from brawl_materials import bake_layers
                    m['texture']=bake_layers(m,state,textures)
                elif m.get('texture'):
                    m['texture']=dict(m['texture'],alphaMode='BLEND' if state.get('blend') else 'OPAQUE')

        if visibility:
            vis=json.loads(Path(visibility).read_text())['visibility'];groups=[]
            for i,variants in enumerate(vis['references'][0]):
                default=next((d['group'] for d in vis['defaults'] if d['switch']==i),-1)
                group={'id':i,'default':default,'label':'Model switch '+str(i),'variants':[]}
                for j,ids in enumerate(variants):
                    names={bones[b]['name'] for b in ids}
                    group['variants'].append({'id':j,'label':', '.join(sorted(names)) or 'Hidden','dobjs':[m['dobj'] for m in meshes if m.get('visibilityBone') in names]})
                groups.append(group)
            visible=set(m['dobj'] for m in meshes)
            for g in groups:
                for v in g['variants']:visible.difference_update(v['dobjs'])
                for v in g['variants']:
                    if v['id']==g['default']:visible.update(v['dobjs'])
            for m in meshes:m['defaultVisible']=m['dobj'] in visible;m['category']='main' if m['defaultVisible'] else 'submodel'
            model['modelParts']={'groups':groups,'source':'Brawl fighter model visibility'}
    return model

def import_animation(path,model):
    a=json.loads(Path(path).read_text());by_name={b['name']:b for b in a['bones']};nodes=[]
    for b in model['bones']:
        track=by_name.get(b['name']);tracks=[]
        if track:
            for prop,ids in [('translation',[5,6,7]),('rotation',[1,2,3]),('scale',[8,9,10])]:
                for i,id in enumerate(ids):
                    values=[f[prop][i]*(math.pi/180 if prop=='rotation' else 1) for f in track['frames']]
                    t=dict(type=id,values=values)
                    channel={'scale':0,'rotation':3,'translation':6}[prop]+i
                    if track.get('keys') and track['keys'][channel]:
                        factor=math.pi/180 if prop=='rotation' else 1
                        t['keys']=[dict(frame=k['frame'],value=k['value']*factor,tangent=k['tangent']*factor) for k in track['keys'][channel]]
                    tracks.append(t)
        nodes.append(tracks)
    return dict(frames=a['frames']-1,nodes=nodes,loop=a['loop'])

def main():
    import argparse
    ap=argparse.ArgumentParser();ap.add_argument('model',type=Path);ap.add_argument('output',type=Path);ap.add_argument('--textures',type=Path);ap.add_argument('--animations',type=Path);ap.add_argument('--visibility',type=Path);ap.add_argument('--include-submodels',action='store_true');args=ap.parse_args()
    model=import_dae(args.model,args.textures,args.visibility);args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.with_suffix('.model.json').write_text(json.dumps(model,separators=(',',':')))
    animations=[]
    if args.animations:
        for p in sorted(args.animations.rglob('*.animation.json')):
            a=import_animation(p,model)
            if any(a['nodes']):animations.append((p.name.removesuffix('.animation.json'),a))
    if not args.include_submodels:model=dict(model,meshes=[m for m in model['meshes'] if m['defaultVisible']])
    export_model(model,args.output,animations)
    print(json.dumps({'bones':len(model['bones']),'meshes':len(model['meshes']),'animations':len(animations),'output':str(args.output)}))
if __name__=='__main__':main()
