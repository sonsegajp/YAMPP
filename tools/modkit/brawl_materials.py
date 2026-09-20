"""Bake Brawl's layered eye material to glTF's single base-color texture."""
import base64
from pathlib import Path
import numpy as np
from PIL import Image
from scipy.ndimage import map_coordinates

def bake_layers(mesh,state,textures,size=256):
    layers=state['textures'];names=[x['name'] for x in layers]
    # Diddy's eye shader composites the iris and two eyelids over his face atlas.
    iris=next((n for n in names if n.startswith('G_medama')),None)
    if not iris or set(names)!={'G_mabuta_sita','G_mabuta_ue',iris,'diddy_body_09'}:
        raise ValueError('Unsupported layered material: '+state['name']+' '+str(names))
    layers=[next(l for l in layers if l['name']==n) for n in ['diddy_body_09',iris,'G_mabuta_sita','G_mabuta_ue']]
    images={n:np.asarray(Image.open(textures[n]).convert('RGBA'),float)/255 for n in names}
    uv={int(k):np.asarray(v) for k,v in mesh['uvSets'].items()};target=uv[0];canvas=np.ones((size,size,4))
    for tri in np.asarray(mesh['indices']).reshape(-1,3):
        t=target[tri];a,b,c=t;matrix=np.column_stack((b-a,c-a))
        if abs(np.linalg.det(matrix))<1e-10:continue
        lo=np.maximum(0,np.floor(t.min(axis=0)*size-1).astype(int));hi=np.minimum(size-1,np.ceil(t.max(axis=0)*size+1).astype(int))
        yy,xx=np.mgrid[lo[1]:hi[1]+1,lo[0]:hi[0]+1];points=np.column_stack(((xx.ravel()+.5)/size,(yy.ravel()+.5)/size))
        v=(points-a)@np.linalg.inv(matrix).T;weights=np.column_stack((1-v[:,0]-v[:,1],v));inside=np.all(weights>=-.005,axis=1)
        if not inside.any():continue
        weights=weights[inside];rgb=None
        for layer in layers:
            channel=int(layer['coords'].removeprefix('TexCoord'));coords=weights@uv[channel][tri]
            scale=np.array(layer['scale']);translation=np.array(layer['translation']);coords=(coords-.5)*scale+.5
            # NW4R SRT uses a translation of the image within the texture frame.
            coords[:,0]-=translation[0];coords[:,1]+=translation[1]
            im=images[layer['name']];xy=np.array([np.clip(coords[:,1],0,1)*(im.shape[0]-1),np.clip(coords[:,0],0,1)*(im.shape[1]-1)])
            rgba=np.column_stack([map_coordinates(im[:,:,i],xy,order=1,mode='nearest') for i in range(4)])
            if rgb is None:rgb=rgba[:,:3]
            else:rgb=rgb*(1-rgba[:,3,None])+rgba[:,:3]*rgba[:,3,None]
        canvas[yy.ravel()[inside],xx.ravel()[inside],:3]=rgb
    pixels=np.clip(np.round(canvas*255),0,255).astype('uint8')
    return {'name':state['name']+'-baked','width':size,'height':size,'rgba':base64.b64encode(pixels.tobytes()).decode(),'alphaMode':'OPAQUE','wrapS':0,'wrapT':0}
