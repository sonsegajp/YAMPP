"""Verify native geometry invariants and capture all four costumes in Workshop."""
from pathlib import Path
import base64, json, os, shutil, subprocess, sys
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/modkit'))
import catalog, service, community

WORK=ROOT/'build/link-fierce-deity'
CHECK=ROOT/'build/modkit/electron-costume-check'
PACK=catalog.MODS/'fierce-deity-link'

def main():
    original=json.loads((WORK/'link-original-model.json').read_text())
    report=[]
    for filename in ['PlLkFD.dat','PlLkFR.dat','PlLkFG.dat','PlLkFV.dat']:
        out=WORK/(filename+'.model.json')
        service.run_bridge('model',PACK/'files'/filename,out)
        current=json.loads(out.read_text())
        assert original['bones']==current['bones']
        assert len(original['meshes'])==len(current['meshes'])
        for index,(before,after) in enumerate(zip(original['meshes'],current['meshes'])):
            assert {k:v for k,v in before.items() if k!='texture'}=={k:v for k,v in after.items() if k!='texture'},(filename,index)
        report.append({'file':filename,'originalBones':len(current['bones']),'originalMeshRecords':len(current['meshes']),'geometryUnchanged':True})
    decoded={}
    for code in ['FD','FR','FG','FV']:
        output=WORK/f'PlLk{code}.images.json'
        service.run_bridge('images',PACK/'files'/f'PlLk{code}.dat',output)
        decoded[code]=json.loads(output.read_text())
    pixel_checks=[]
    for code in ['FR','FG','FV']:
        changed=[]
        for before,after in zip(decoded['FD'],decoded[code]):
            assert (before['id'],before['width'],before['height'])==(after['id'],after['width'],after['height'])
            if before['rgba']==after['rgba']:continue
            index=before['id'];mask_id={57:83,75:44,89:90}.get(index,index)
            mask_path=WORK/'cloth-masks'/f'texture-{mask_id:02d}.png'
            assert mask_path.is_file(),('non-cloth image changed',code,index)
            mask=np.asarray(Image.open(mask_path))>0
            shape=(before['height'],before['width'],4)
            a=np.frombuffer(base64.b64decode(before['rgba']),dtype=np.uint8).reshape(shape)
            b=np.frombuffer(base64.b64decode(after['rgba']),dtype=np.uint8).reshape(shape)
            assert np.array_equal(a[~mask],b[~mask]),('non-cloth pixels changed',code,index)
            changed.append(index)
        pixel_checks.append({'file':f'PlLk{code}.dat','changedTextureIds':changed,'allPixelsOutsideClothMasksIdentical':True})
    CHECK.mkdir(parents=True,exist_ok=True)
    shutil.copytree(PACK,CHECK/'mods/fierce-deity-link',dirs_exist_ok=True)
    env=dict(os.environ);env.pop('ELECTRON_RUN_AS_NODE',None);env['MELEE_WORKSHOP_PYTHON']=sys.executable
    with (CHECK/'workshop.log').open('w') as log:
        proc=subprocess.Popen([str(ROOT/'tools/modkit/desktop/node_modules/electron/dist/electron.exe'),str(ROOT/'tools/modkit/desktop'),'--workshop-costume-check'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        try:code=proc.wait(timeout=220)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(proc.pid),'/T','/F'],capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW);raise
    if code:raise RuntimeError((CHECK/'failure.txt').read_text())
    ui=json.loads((CHECK/'report.json').read_text());assert ui['costumes']==4 and not ui['errors']
    prepared=community.prepare({'id':'fierce-deity-link'})
    result={'pixelChecks':pixel_checks,'models':report,'workshop':ui,'prepared':prepared}
    (WORK/'validation-report.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'pixelChecks':pixel_checks,'models':report,'workshop':ui,'preparedId':prepared['preparedId']},indent=2))

if __name__=='__main__':main()
