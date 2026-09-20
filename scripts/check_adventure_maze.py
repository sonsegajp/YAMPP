"""Capture the original Underground Maze with isolated settings and a copied card."""
from pathlib import Path
import sys,os,shutil,subprocess,json,re,time,argparse,hashlib
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'scripts'))
from project_config import configuration,disc_path,project_path,game_environment
ap=argparse.ArgumentParser();ap.add_argument('--name',default='maze-stock-preload');ap.add_argument('--exe',default='build/native-game/YAMPP-online-next.exe');ap.add_argument('--late-pick',action='store_true');ap.add_argument('--renderer',default='build/pc/bin/melee_aurora_controllers.dll');ap.add_argument('--wide',action='store_true');ap.add_argument('--frames',type=int,default=4200);a=ap.parse_args()
out=(ROOT/'build/comparisons'/a.name).resolve()
if not out.is_relative_to((ROOT/'build/comparisons').resolve()):ap.error('Output must stay under build/comparisons')
out.mkdir(parents=True,exist_ok=True)
cfg=configuration();env=game_environment(cfg,disc_path(cfg))
for key in list(env):
 if key.startswith(('MELEE_TEST_','MELEE_CAPTURE_','MELEE_NETPLAY_','MELEE_MOD_TEST_','MELEE_CONTENT_','MELEE_MEX_','MELEE_RUNTIME_')) or key in ('MELEE_INPUT','MELEE_VANILLA_BOOT'):env.pop(key)
(out/'settings.xml').write_text('<melee-settings schema="1" width="960" height="720" renderScale="1" widescreen="%d" fullscreen="0" mute="1" />'%a.wide)
shutil.copy2(ROOT/'build/netplay-work/template-card.raw',out/'card.raw')
boot='300:100:6,420:100:6,540:100:6,660:100:6,780:100:6,900:1000:6'
pick='1120:100:8,1480:0:26:0:80,1580:100:8,1780:1000:8,2300:100:8,2450:100:8'
combat='2600:1000:8,2800:0:80:80:0,2930:400:8,3060:100:8,3150:0:70:80:0,3280:400:8,3400:100:8,3500:0:100:80:0'
if a.late_pick:
 pick='1320:100:16,2500:0:26:0:80,2660:100:16,2880:1000:16,3200:100:16'
 combat='3500:0:80:80:0,3630:400:16,3760:100:16,3950:0:70:80:0,4100:400:16,4240:100:16'
exe=ROOT/a.exe;renderer=(ROOT/a.renderer).resolve()
env.update(MELEE_AURORA_DLL=str(renderer),MELEE_SETTINGS=str(out/'settings.xml'),MELEE_MEMORY_CARD=str(out/'card.raw'),GCN_AURORA_HIDDEN='1',GCN_AURORA_DATA_DIR=str(out/'cache'),MELEE_TEST_MENU='6:1',MELEE_TEST_ADVENTURE_STAGE='2',MELEE_INPUT=','.join((boot,pick,combat)),MELEE_CAPTURE_AURORA='1',MELEE_CAPTURE_DIR=str(out),MELEE_TEST_FAST_EXIT='1')
if exe.with_suffix('.core.dll').is_file():
 state=out/'content-state.json';state.write_text(json.dumps({'schema':1,'akaneia':None}))
 plan=out/'boot-plan.env'
 env.update(MELEE_CONTENT_STATE=str(state),MELEE_CONTENT_LAUNCHER='1',MELEE_CONTENT_RESTART=str(out/'restart.json'),MELEE_CONTENT_BOOT_PLAN=str(plan),MELEE_CONTENT_PLAN_SCRIPT=str(ROOT/'scripts/prepare_content_runtime.py'),MELEE_WORKSHOP_PYTHON=sys.executable)
 subprocess.run([sys.executable,env['MELEE_CONTENT_PLAN_SCRIPT'],'--output',str(plan)],cwd=ROOT,env=env,check=True)

env['PATH']=str(ROOT/'build/pc/bin')+';C:/msys64/mingw64/bin;'+env['PATH']
(out/'inputs.json').write_text(json.dumps({'exe':str(exe),'exeSHA256':hashlib.sha256(exe.read_bytes()).hexdigest(),'rendererSHA256':hashlib.sha256(renderer.read_bytes()).hexdigest(),'registry':env.get('MELEE_COSTUME_REGISTRY'),'input':env['MELEE_INPUT'],'adventureBlock':2,'wide':a.wide},indent=2))
with (out/'run.log').open('w') as log:
 proc=subprocess.Popen([str(exe),str(project_path(cfg['assets']['directory'])/'sys/main.dol'),str(a.frames)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
 print('Owned maze test PID',proc.pid,flush=True)
 try:proc.wait(timeout=240)
 except subprocess.TimeoutExpired:proc.terminate();proc.wait(15);raise
text=(out/'run.log').read_text(errors='replace')
report={'exit':proc.returncode,'nativeAdventureStart':'[adventure-test]' in text,'assertions':re.findall(r'^.*assertion .*failed.*$',text,re.M),'costumeLoads':re.findall(r'^\[costumes\] loaded.*$',text,re.M),'fault':'[fault]' in text,'captures':len(list(out.glob('frame_*.ppm')))}
if (ROOT/'build/native-game/last-memory.bin').exists():shutil.copy2(ROOT/'build/native-game/last-memory.bin',out/'last-memory.bin')
(out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))

if report['exit'] or report['fault'] or report['assertions'] or not report['nativeAdventureStart'] or not report['captures']:raise SystemExit(1)
