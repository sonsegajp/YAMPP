"""Exercise an additive Link costume with normal native CSS controller input.

Copies the installed skin into an isolated mod folder, selects Link, cycles X through
the original five costumes, and enters a match against a second human port.
"""
import argparse,json,os,re,shutil,subprocess,sys,time,struct,hashlib
from pathlib import Path
from project_config import ROOT,configuration,disc_path,project_path
from check_menu_additions import contact_sheet

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--name',default='additive-link-native')
    ap.add_argument('--slot',type=int,default=5)
    ap.add_argument('--content-root',type=Path,help='Verified selective Akaneia content below build/')
    ap.add_argument('--capture-interval',type=int,default=30)
    ap.add_argument('--frames',type=int,default=3900)
    ap.add_argument('--cursor-right',type=int,default=40)
    ap.add_argument('--player',type=int,choices=(0,2),default=0,help='Port 3 starts below Link in the expanded Akaneia grid')
    ap.add_argument('--executable',default='melee-online-next.exe')
    ap.add_argument('--renderer',default='melee_aurora_online_next.dll')
    a=ap.parse_args()
    if not 5<=a.slot<64:ap.error('Select an additive Link slot (5 through 63)')
    out=(ROOT/'build/comparisons'/a.name).resolve()
    if not out.is_relative_to((ROOT/'build/comparisons').resolve()):ap.error('Invalid output name')
    out.mkdir(parents=True,exist_ok=True)
    mods=(ROOT/'build/modkit'/a.name/'mods').resolve()
    if not mods.is_relative_to((ROOT/'build/modkit').resolve()):ap.error('Invalid mod sandbox')
    mods.mkdir(parents=True,exist_ok=True)
    source=ROOT/'user/mods/fierce-deity-link'
    target=mods/source.name
    shutil.copytree(source,target,dirs_exist_ok=True)
    env=os.environ.copy()
    for k in list(env):
        if k.startswith(('MELEE_TEST_','MELEE_MOD_TEST_','MELEE_NETPLAY_','MELEE_CAPTURE_')) or k in ('MELEE_MODS','MELEE_INPUT','MELEE_MEMORY_CARD'):env.pop(k)
    env['MELEE_WORKSHOP_TEST_MODS']=str(mods)
    subprocess.run([sys.executable,'-c','import sys;sys.path.insert(0,"tools/modkit");from catalog import costume_registry;costume_registry()'],cwd=ROOT,env=env,check=True)
    cfg=configuration();settings=out/'settings.xml'
    settings.write_text('<melee-settings schema="1" width="960" height="720" renderScale="1" widescreen="0" fullscreen="0" vsync="0" volume="100" mute="1" showFps="0" />')
    shutil.copy2(ROOT/'build/netplay-work/template-card.raw',out/'card.raw')
    boot='300:100:6,420:100:6,540:100:6,660:100:6,780:100:6,900:1000:6,1080:4:8,1170:100:8,1260:100:8'
    pick=f'1600:0:26:0:80,1640:0:{a.cursor_right}:80:0,1750:100:8,1600:0:26:0:80:1,1750:100:8:0:0:1'
    if a.player==2:
        if not a.content_root:ap.error('Port 3 fixture requires the expanded Akaneia grid')
        pick='1600:0:26:0:80:2,1750:100:8:0:0:2,1600:0:26:0:80:0,1750:100:8:0:0:0'
    colors=','.join(f'{1820+i*50}:400:8:0:0:{a.player}' for i in range(a.slot))
    start=1900+a.slot*50
    stage=f'{start}:1000:8,{start+190}:0:20:0:80,{start+230}:0:20:80:0,{start+310}:100:8'
    combat=f'{start+650}:0:35:80:0,{start+700}:100:8,{start+740}:100:8,{start+800}:200:8,{start+900}:400:8'
    env.update(MELEE_DISC=str(disc_path(cfg)),MELEE_FST=str(project_path(cfg['assets']['directory'])/'sys/fst.bin'),MELEE_COSTUME_REGISTRY=str(mods/'costumes.tsv'),MELEE_AURORA_DLL=str(ROOT/'build/pc/bin'/a.renderer),GCN_AURORA_HIDDEN='1',GCN_AURORA_DATA_DIR=str(out/'cache'),MELEE_MEMORY_CARD=str(out/'card.raw'),MELEE_SETTINGS=str(settings),MELEE_INPUT=','.join((boot,pick,colors,stage,combat)),MELEE_CAPTURE_AURORA='1',MELEE_CAPTURE_UI='1',MELEE_CAPTURE_DIR=str(out))
    env['PATH']=str(ROOT/'build/pc/bin')+';C:/msys64/mingw64/bin;'+env['PATH']
    exe=ROOT/'build/native-game'/a.executable
    content_record=None
    if a.content_root:
        content=a.content_root.resolve()
        if not content.is_relative_to(ROOT/'build'):ap.error('Content must be below build/')
        proof=json.loads((content/'verification.json').read_text())
        content_record={'directory':content.relative_to(ROOT).as_posix(),'isoSHA256':proof['isoSHA256'],'enabled':True}
        if not exe.with_suffix('.core.dll').is_file():ap.error('Content test requires a reloadable game core')
        env['MELEE_TEST_MENU']='2:0'
        env['MELEE_INPUT']=env['MELEE_INPUT'].replace(',1080:4:8','').replace(',1170:100:8','').replace(',1260:100:8','')
    env['MELEE_CAPTURE_INTERVAL']=str(a.capture_interval)
    if exe.with_suffix('.core.dll').is_file():
        state=out/'content-state.json';state.write_text(json.dumps({'schema':1,'akaneia':content_record}))
        env.update(MELEE_CONTENT_STATE=str(state),MELEE_CONTENT_LAUNCHER='1',MELEE_CONTENT_RESTART=str(out/'restart.json'),
                   MELEE_CONTENT_BOOT_PLAN=str(out/'boot-plan.env'),MELEE_CONTENT_PLAN_SCRIPT=str(ROOT/'scripts/prepare_content_runtime.py'),
                   MELEE_WORKSHOP_PYTHON=sys.executable,MELEE_CONTENT_TEST_MOD_REGISTRY=str(mods/'registry.tsv'),MELEE_TEST_FAST_EXIT='1')
        subprocess.run([sys.executable,env['MELEE_CONTENT_PLAN_SCRIPT'],'--output',env['MELEE_CONTENT_BOOT_PLAN']],cwd=ROOT,env=env,check=True)
    (out/'inputs.json').write_text(json.dumps({'executable':str(exe),'executableSHA256':hashlib.sha256(exe.read_bytes()).hexdigest(),'coreSHA256':hashlib.sha256(exe.with_suffix('.core.dll').read_bytes()).hexdigest() if exe.with_suffix('.core.dll').is_file() else None,'renderer':a.renderer,'slot':a.slot,'selectionPort':a.player,'timeline':env['MELEE_INPUT'],'contentRecord':content_record},indent=2))
    started=time.monotonic();began_wall=time.time()
    with (out/'run.log').open('w') as log:
        p=subprocess.Popen([str(ROOT/'build/native-game'/a.executable),str(project_path(cfg['assets']['directory'])/'sys/main.dol'),str(a.frames)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:p.wait(timeout=max(150,a.frames/60*5))
        except subprocess.TimeoutExpired:p.terminate();p.wait();raise
    text=(out/'run.log').read_text(errors='replace');shots=sorted(f for f in out.glob('frame_*.ppm') if f.stat().st_mtime>=began_wall);contact_sheet(shots,out/'sheet.jpg')
    loads=re.findall(r'\[costumes\] loaded (.*) kind=(\d+) slot=(\d+) base=(\d+)',text)
    assertions=re.findall(r'^.*assertion .*failed.*$',text,re.M)
    report={'exit_code':p.returncode,'wall_seconds':time.monotonic()-started,'slot':a.slot,'diagnostic_cursor':False,'costume_loads':loads,'assertions':assertions,'fault':'[fault]' in text or '[content-host-fault]' in text,'captures':len(shots)}
    report['selectionPort']=a.player
    report['contentRoot']=str(a.content_root) if a.content_root else None
    memory=ROOT/'build/native-game/last-memory.bin'
    if memory.is_file() and memory.stat().st_mtime>=began_wall:
        data=memory.read_bytes();base=0x453080+a.player*0xe90
        report['selectedPlayer']={'state':struct.unpack_from('>I',data,base)[0],'fighter':struct.unpack_from('>I',data,base+4)[0],'costume':data[base+0x44]}
    report['gameplaySlotVerified']=report.get('selectedPlayer')=={'state':2,'fighter':6,'costume':a.slot}
    report['passed']=p.returncode==0 and report['gameplaySlotVerified'] and not assertions and not report['fault'] and any(int(k)==6 and int(c)==a.slot for _,k,c,_ in loads)
    (out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
    if not report['passed']:raise SystemExit(1)
if __name__=='__main__':main()
