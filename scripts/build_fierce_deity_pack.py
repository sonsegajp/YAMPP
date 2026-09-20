"""Package imagegen UV art into original Melee Link DATs; no geometry conversion."""
from pathlib import Path
import base64, hashlib, json, sys
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/modkit'))
import catalog, service
from xml_manifest import write_manifest

WORK=ROOT/'build/link-fierce-deity'
PACK=catalog.MODS/'fierce-deity-link'
ART=ROOT/'assets/costumes/fierce-deity-link'

def textures():
    result={}
    for group in ['head','body','details','armor']:
        atlas=Image.open(ART/f'{group}-atlas-generated.png').convert('RGBA')
        layout=json.loads((ART/f'{group}-atlas-layout.json').read_text())
        for tile in layout['tiles']:
            # Belt/buckle art stays original. Imagegen output is only resampled to
            # the exact native UV size; original alpha remains authoritative.
            if tile['id']==92 or (group=='body' and tile['id']==94) or (group=='armor' and tile['id']==96):continue
            box=tuple(round(v*atlas.width/1024) for v in tile['box'])
            converted=atlas.crop(box).resize(tuple(tile['size']),Image.Resampling.LANCZOS)
            original=Image.open(WORK/'uv-original'/f"texture-{tile['id']:02d}.png").convert('RGBA')
            converted.putalpha(original.getchannel('A'))
            result[tile['id']]=converted
    return result

def variant_textures(base, color):
    import numpy as np
    result=dict(base)
    atlas=Image.open(ART/f'variants-{color}-generated.png').convert('RGBA')
    layout=json.loads((ART/'variants-atlas-layout.json').read_text())
    masks=WORK/'cloth-masks';masks.mkdir(exist_ok=True)
    for tile in layout['tiles']:
        index=tile['id']
        if index in [57,89,75]:continue
        native=Image.open(WORK/'uv-original'/f'texture-{index:02d}.png').convert('RGBA')
        rgb=np.asarray(native).astype('int32')[:,:,:3]
        cloth=(rgb[:,:,1]>rgb[:,:,0]*1.4)&(rgb[:,:,1]>rgb[:,:,2]*1.4)
        if index in [58,88,91]:
            painted=np.asarray(base[index]).astype('int32')[:,:,:3]
            cloth &= (painted[:,:,1]>painted[:,:,0]*1.10)&(painted[:,:,2]<painted[:,:,1]*1.32)
        mask=Image.fromarray((cloth*255).astype('uint8'),'L')
        mask.save(masks/f'texture-{index:02d}.png')
        box=tuple(round(v*atlas.width/1024) for v in tile['box'])
        repaint=atlas.crop(box).resize(tuple(tile['size']),Image.Resampling.LANCZOS)
        original=base[index]
        composed=Image.composite(repaint,original,mask)
        composed.putalpha(original.getchannel('A'))
        before=np.asarray(original);after=np.asarray(composed)
        assert np.array_equal(before[~cloth],after[~cloth]),index
        result[index]=composed
    # The low-detail head has a small hat strip sharing its face image. Copy
    # generated cap cloth only through the original green hat mask.
    native=np.asarray(Image.open(WORK/'uv-original/texture-56.png').convert('RGBA')).astype('int32')
    hat=(native[:,:,1]>native[:,:,0]*1.4)&(native[:,:,1]>native[:,:,2]*1.4)
    mask=Image.fromarray((hat*255).astype('uint8'),'L');mask.save(masks/'texture-56.png')
    composed=Image.composite(result[83].resize(base[56].size,Image.Resampling.LANCZOS),base[56],mask)
    composed.putalpha(base[56].getchannel('A'))
    assert np.array_equal(np.asarray(base[56])[~hat],np.asarray(composed)[~hat])
    result[56]=composed
    return result


def build():
    original_rows=json.loads((WORK/'link-original-images.json').read_text())
    base=textures()
    files=PACK/'files';files.mkdir(parents=True,exist_ok=True)
    source=catalog.ASSETS/'PlLkNr.dat'
    source_hash=hashlib.sha256(source.read_bytes()).hexdigest()
    costumes=[];report=[]
    for color,suffix,label in [('base','FD','Fierce Deity'),('red','FR','Fierce Deity Red'),('green','FG','Fierce Deity Green'),('violet','FV','Fierce Deity Violet')]:
        replacements=base if color=='base' else variant_textures(base,color)
        all_replacements={}
        for index,texture in replacements.items():
            original=original_rows[index]
            for row in original_rows:
                if (row['width'],row['height'],row['rgba'])==(original['width'],original['height'],original['rgba']):
                    all_replacements[row['id']]=texture
        uv=WORK/f'uv-fierce-deity-{color}';uv.mkdir(exist_ok=True)
        patches=[]
        for index,im in sorted(all_replacements.items()):
            im.save(uv/f'texture-{index:02d}.png')
            patches.append(dict(id=index,width=im.width,height=im.height,rgba=base64.b64encode(im.tobytes()).decode()))
        patch=WORK/f'{color}-patches.json';patch.write_text(json.dumps(patches,separators=(',',':')))
        out=files/f'PlLk{suffix}.dat'
        service.run_bridge('patch-image',source,patch,out)
        costumes.append({'id':'fierce-deity'+('' if color=='base' else '-'+color),'name':label,'file':'files/'+out.name})
        report.append({'color':color,'dat':str(out),'sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'patchedTextures':len(patches)})
        print('Built',color,flush=True)
    assert hashlib.sha256(source.read_bytes()).hexdigest()==source_hash
    manifest={'schema':1,'kind':'costume','id':'fierce-deity-link','name':'Fierce Deity Link','version':'1.0.0','base':'stock-Lk','additive':True,'enabled':True,'description':'Four Fierce Deity inspired texture costumes on the original Melee Link model and rig: teal, red, green, and violet cloth. Adds four slots after the original five colors.','costumes':costumes}
    write_manifest(PACK,manifest)
    catalog.runtime_registry()
    service.run_bridge('model',files/'PlLkFD.dat',WORK/'link-fierce-deity-model.json')
    (WORK/'build-report.json').write_text(json.dumps({'pack':str(PACK),'costumes':report,'originalDatSha256':source_hash},indent=2)+'\n')
    print((WORK/'build-report.json').read_text())

if __name__=='__main__':build()
