"""Compose native UV tiles for imagegen; this does not paint or alter artwork."""
import base64, io, json, sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/link-fierce-deity'
GROUPS = {
    'details': [44,94,45,50,48,53,70,46,51,33,64,34,65,40,71,72],
    'variants': [83,95,93,88,91,90,58,44,94,45,50,48,53,57,89,75],
    'head': [77,78,81,56,80,84,85,86,87,79,76,82,54,55,57,83],
    'body': [96,95,93,88,91,90,58,74,61,62,69,66,68,73,92,94],
}

def compose(group):
    if group=='variants':
        from build_fierce_deity_pack import textures
        edited=textures()
        edited[89]=edited[90];edited[75]=edited[44]
    atlas = Image.new('RGB', (1024,1024), (255,0,255))
    layout = []
    for slot, index in enumerate(GROUPS[group]):
        original = Image.open(OUT / 'uv-original' / f'texture-{index:02d}.png').convert('RGBA')
        if group=='variants':original=edited[index]
        x,y = slot%4*256+8,slot//4*256+8
        atlas.paste(original.convert('RGB').resize((240,240),Image.Resampling.NEAREST),(x,y))
        layout.append({'id':index,'box':[x,y,x+240,y+240],'size':list(original.size)})
    (OUT / f'{group}-atlas-layout.json').write_text(json.dumps({'canvas':[1024,1024],'tiles':layout},indent=2)+'\n')
    return atlas

if __name__ == '__main__':
    group = sys.argv[1]
    result = compose(group)
    if '--save' in sys.argv:
        result.save(OUT / f'{group}-atlas-original.png')
    else:
        encoded=io.BytesIO(); result.save(encoded,format='PNG')
        print(base64.b64encode(encoded.getvalue()).decode())
